#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/operator/mock/operator_process_binding.h"
#include "edgeflow/operator/interface.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {
namespace {
namespace api = operator_api;

struct Destination {
  int service = 0;
  int value = 0;
};

struct Publication {
  std::shared_ptr<OutputConverterDefinition> definition;
  RuntimeOutputBatch staged;
  std::vector<std::vector<FrameOutputBinding>> bindings;
  std::vector<SelectedOutput> selected;
  api::NamedIoBatch external;
  std::array<std::weak_ptr<Destination>, 2> payloads;
  int writes = 0;

  Publication() {
    definition = std::make_shared<OutputConverterDefinition>();
    definition->type = "publication";
    definition->name = "destination";
    SelectedOutput output;
    output.converter = definition.get();
    output.host_binding.services = {{"destination", 9}};
    output.host_binding.write_service_type = [this](void *pointer,
                                                    int service) {
      ++writes;
      static_cast<Destination *>(pointer)->service = service;
    };
    selected.push_back(std::move(output));
    for (int value : {17, 29}) {
      auto payload = std::make_shared<Destination>();
      payload->value = value;
      payloads[staged.rows.size()] = payload;
      staged.rows.push_back({{0, std::move(payload)}});
      bindings.push_back({{"destination.publication", 0}});
      external.push_back({{"destination.publication", nullptr}});
    }
  }
};

TEST(OperatorPublicationTest,
     EveryServiceWriteAndDestinationIsPreparedBeforeMovingValues) {
  Publication test;
  test.selected[0].host_binding.write_service_type = [&](void *pointer,
                                                         int service) {
    EXPECT_EQ(test.external[0].at("destination.publication"), nullptr);
    EXPECT_EQ(test.external[1].at("destination.publication"), nullptr);
    ++test.writes;
    static_cast<Destination *>(pointer)->service = service;
  };
  PublishOperatorOutputs(&test.staged, test.bindings, test.selected,
                         &test.external);
  EXPECT_EQ(test.writes, 2);
  for (size_t i = 0; i < 2; ++i) {
    ASSERT_NE(test.external[i].at("destination.publication"), nullptr);
    const auto *result = static_cast<const Destination *>(
        test.external[i].at("destination.publication").get());
    EXPECT_EQ(result->service, 9);
    EXPECT_EQ(result->value, i == 0 ? 17 : 29);
    EXPECT_EQ(test.staged.rows[i][0].value, nullptr);
  }
  test.external.clear();
  for (const auto &payload : test.payloads) EXPECT_TRUE(payload.expired());
}

TEST(OperatorPublicationTest,
     ThrowingSecondServiceWriterKeepsAllExternalSlotsEmpty) {
  Publication test;
  test.selected[0].host_binding.write_service_type = [&](void *pointer,
                                                         int service) {
    if (++test.writes == 2) throw std::runtime_error("second service write");
    static_cast<Destination *>(pointer)->service = service;
  };
  EXPECT_THROW(PublishOperatorOutputs(&test.staged, test.bindings,
                                      test.selected, &test.external),
               std::runtime_error);
  EXPECT_EQ(test.writes, 2);
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(test.external[i].at("destination.publication"), nullptr);
    EXPECT_NE(test.staged.rows[i][0].value, nullptr);
  }
  // Service fields may have changed; ownership has not been committed.
  test.staged.rows.clear();
  for (const auto &payload : test.payloads) EXPECT_TRUE(payload.expired());
}

TEST(OperatorPublicationTest,
     MissingLaterDestinationDoesNotCommitAnEarlierSlot) {
  Publication test;
  test.external[1].clear();
  EXPECT_THROW(PublishOperatorOutputs(&test.staged, test.bindings,
                                      test.selected, &test.external),
               std::out_of_range);
  EXPECT_EQ(test.writes, 2);
  EXPECT_EQ(test.external[0].at("destination.publication"), nullptr);
  EXPECT_NE(test.staged.rows[0][0].value, nullptr);
  EXPECT_NE(test.staged.rows[1][0].value, nullptr);
}

class OperatorBoundaryTest : public ::testing::Test {
 protected:
  struct Recording {
    int mode = 0, writes = 0, resets = 0;
  };
  void SetUp() override {
    auto &values = OperatorValueTypeRegistry::Instance();
    auto binding = values.CopyBindingBySuffix("json_out");
    ASSERT_TRUE(binding);
    const auto writer = binding->write_service_type;
    const auto reset = binding->reset_external;
    binding->write_service_type = [state = recording_, writer](
                                      void *value, int32_t service) {
      if (++state->writes == 2) {
        if (state->mode == 1)
          throw std::runtime_error("public service writer failed");
        if (state->mode == 2) throw 19;
      }
      writer(value, service);
    };
    binding->reset_external = [state = recording_, reset](
                                  void *value,
                                  const ResolvedOutputPoolSpec &spec) {
      ++state->resets;
      reset(value, spec);
    };
    test_support::RegistryTestAccess::SetValueBinding(std::move(*binding));
    ASSERT_EQ(ops_.Init(), 0) << api::GetOperatorLastError();
    api::CreateParam create;
    create.model_path = ".";
    create.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
    create.compute_platform = api::ComputePlatform::kCpu;
    create.max_frame_depth = 2;
    ASSERT_EQ(ops_.Create(&handle_, &create), 0) << api::GetOperatorLastError();
    for (size_t i = 0; i < 2; ++i) {
      texts_[i] = "初始化";
      strings_[i] = {static_cast<int32_t>(texts_[i].size()), texts_[i].data()};
      requests_[i] = {kMockServiceKeywordMatch, &strings_[i]};
      inputs_.push_back({{"keyword.keyword_in",
                          api::MakeBorrowedOperatorInput(&requests_[i])}});
      outputs_.push_back({{"keyword.json_out", nullptr}});
    }
  }
  void TearDown() override {
    outputs_.clear();
    if (handle_) {
      EXPECT_EQ(ops_.Destroy(handle_), 0);
    }
    EXPECT_EQ(ops_.DeInit(), 0);
  }
  test_support::RegistryTestAccess::ScopedValueTypeState saved_;
  std::shared_ptr<Recording> recording_ = std::make_shared<Recording>();
  api::OperatorFunc ops_ = api::Get_LLM_EDGEFLOW_OperatorTable();
  void *handle_ = nullptr;
  std::array<std::string, 2> texts_;
  std::array<CompanyString, 2> strings_;
  std::array<CompanyOperatorKeywordInput, 2> requests_;
  api::NamedIoBatch inputs_, outputs_;
};

TEST_F(OperatorBoundaryTest,
       BothExceptionBarriersReturnEveryLeaseAndPermitTheNextCall) {
  for (int mode : {1, 2}) {
    SCOPED_TRACE(mode);
    recording_->mode = mode;
    recording_->writes = recording_->resets = 0;
    EXPECT_EQ(ops_.Process(handle_, inputs_, outputs_), mode == 1 ? -99 : -100);
    EXPECT_EQ(recording_->writes, 2);
    EXPECT_EQ(recording_->resets, 2);
    for (const auto &row : outputs_)
      EXPECT_EQ(row.at("keyword.json_out"), nullptr);
    const std::string diagnostic = api::GetOperatorLastError();
    EXPECT_NE(diagnostic.find(mode == 1 ? "public service writer failed"
                                        : "Unknown exception in Process"),
              std::string::npos);
    recording_->mode = recording_->writes = 0;
    ASSERT_EQ(ops_.Process(handle_, inputs_, outputs_), 0)
        << api::GetOperatorLastError();
    for (const auto &row : outputs_)
      EXPECT_NE(row.at("keyword.json_out"), nullptr);
    for (auto &row : outputs_) row.at("keyword.json_out").reset();
    EXPECT_EQ(recording_->resets, 4);
  }
}

}  // namespace
}  // namespace llm_edgeflow
