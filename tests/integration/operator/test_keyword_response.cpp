#include <gtest/gtest.h>

#include <array>
#include <nlohmann/json.hpp>
#include <string>

#include "edgeflow/operator/interface.h"

namespace llm_edgeflow {
namespace {
namespace api = operator_api;
using Json = nlohmann::json;

class KeywordResponseTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(ops_.Init(), 0); }
  void TearDown() override {
    outputs_.clear();
    if (handle_) {
      EXPECT_EQ(ops_.Destroy(handle_), 0);
    }
    EXPECT_EQ(ops_.DeInit(), 0);
  }
  api::OperatorFunc ops_ = api::Get_LLM_EDGEFLOW_OperatorTable();
  void* handle_ = nullptr;
  api::NamedIoBatch outputs_;
};

TEST_F(KeywordResponseTest, CompleteRowsAndUpdatedRulesUseTheRealPipeline) {
  api::CreateParam create;
  create.model_path = ".";
  create.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  create.compute_platform = api::ComputePlatform::kCpu;
  create.max_frame_depth = 3;
  ASSERT_EQ(ops_.Create(&handle_, &create), 0) << api::GetOperatorLastError();
  ASSERT_NE(handle_, nullptr);

  std::array<std::string, 3> texts{"执行初始化", "普通查询", "先自检再初始化"};
  std::array<CompanyString, 3> strings;
  std::array<CompanyOperatorKeywordInput, 3> requests;
  api::NamedIoBatch inputs(3);
  for (size_t i = 0; i < texts.size(); ++i) {
    strings[i] = {static_cast<int32_t>(texts[i].size()), texts[i].data()};
    requests[i] = {kMockServiceKeywordMatch, &strings[i]};
    inputs[i]["keyword.keyword_in"] =
        api::MakeBorrowedOperatorInput(&requests[i]);
  }
  const std::array<Json, 3> expected = {
      Json{{"is_hit", 1},
           {"intent", "SYSTEM_INIT"},
           {"confidence", 1.0},
           {"matched_word", "初始化"},
           {"slots", Json::object()},
           {"matches", Json::array({{{"category", "SYSTEM_INIT"},
                                     {"matched_word", "初始化"}}})}},
      Json{{"is_hit", 0}, {"matches", Json::array()}},
      Json{{"is_hit", 1},
           {"intent", "SYSTEM_INIT"},
           {"confidence", 1.0},
           {"matched_word", "初始化"},
           {"slots", Json::object()},
           {"matches",
            Json::array(
                {{{"category", "SYSTEM_INIT"}, {"matched_word", "初始化"}},
                 {{"category", "SYSTEM_INIT"}, {"matched_word", "自检"}}})}}};

  // Refill every slot twice. Clearing all output views returns the three
  // leases.
  for (int round = 0; round < 2; ++round) {
    SCOPED_TRACE(round);
    outputs_.resize(3);
    for (auto& row : outputs_) row["keyword.json_out"] = nullptr;
    ASSERT_EQ(ops_.Process(handle_, inputs, outputs_), 0)
        << api::GetOperatorLastError();
    ASSERT_EQ(outputs_.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      SCOPED_TRACE(i);
      const auto* value = static_cast<const CompanyOperatorOutput*>(
          outputs_[i].at("keyword.json_out").get());
      ASSERT_NE(value, nullptr);
      EXPECT_EQ(value->service_type, kMockServiceKeywordMatch);
      EXPECT_EQ(value->status_code, 0);
      ASSERT_NE(value->result_json, nullptr);
      EXPECT_EQ(Json::parse(std::string(value->result_json->data,
                                        value->result_json->length)),
                expected[i]);
    }
    outputs_.clear();
  }

  api::ControlUpdateRulesParam update{R"({"categories":{"UPDATED":["自检"]}})"};
  ASSERT_EQ(
      ops_.Control(handle_, static_cast<int>(api::ControlCommand::kUpdateRules),
                   &update),
      0);
  outputs_.resize(3);
  for (auto& row : outputs_) row["keyword.json_out"] = nullptr;
  ASSERT_EQ(ops_.Process(handle_, inputs, outputs_), 0)
      << api::GetOperatorLastError();
  const auto* changed = static_cast<const CompanyOperatorOutput*>(
      outputs_[2].at("keyword.json_out").get());
  ASSERT_NE(changed, nullptr);
  ASSERT_NE(changed->result_json, nullptr);
  EXPECT_EQ(Json::parse(std::string(changed->result_json->data,
                                    changed->result_json->length)),
            (Json{{"is_hit", 1},
                  {"intent", "UPDATED"},
                  {"confidence", 1.0},
                  {"matched_word", "自检"},
                  {"slots", Json::object()},
                  {"matches", Json::array({{{"category", "UPDATED"},
                                            {"matched_word", "自检"}}})}}));
}
}  // namespace
}  // namespace llm_edgeflow
