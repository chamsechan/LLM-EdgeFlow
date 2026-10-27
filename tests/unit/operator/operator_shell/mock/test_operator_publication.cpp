#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "adapter/operator/mock/operator_process_binding.h"
#include "edgeflow/operator/interface.h"

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
  // 服务字段可能已被修改，但所有权尚未提交。
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

}  // namespace
}  // namespace llm_edgeflow
