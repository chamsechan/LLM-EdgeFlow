#include <gtest/gtest.h>

#include <array>
#include <deque>
#include <nlohmann/json.hpp>
#include <string>

#include "edgeflow/operator/interface.h"
#include "tests/support/mock_text_carrier.h"

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

TEST_F(KeywordResponseTest, CompleteRowsUseTheConfiguredRulePipeline) {
  api::CreateParam create;
  create.model_path = ".";
  create.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  create.compute_platform = api::ComputePlatform::kCpu;
  create.max_frame_depth = 3;
  ASSERT_EQ(ops_.Create(&handle_, &create), 0) << api::GetOperatorLastError();
  ASSERT_NE(handle_, nullptr);

  std::array<std::string, 3> texts{"执行初始化", "普通查询", "先自检再初始化"};
  std::deque<test_support::MockTextCarrier> requests;
  api::NamedIoBatch inputs(3);
  for (size_t i = 0; i < texts.size(); ++i) {
    inputs[i]["keyword.keyword_in"] =
        api::MakeBorrowedOperatorInput(&requests.emplace_back(texts[i]).root);
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

  outputs_.resize(3);
  for (auto& row : outputs_) row["keyword.json_out"] = nullptr;
  ASSERT_EQ(ops_.Process(handle_, inputs, outputs_), 0)
      << api::GetOperatorLastError();
  ASSERT_EQ(outputs_.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    const auto* value = static_cast<const MockOutput*>(
        outputs_[i].at("keyword.json_out").get());
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->service_type, kMockServiceKeywordMatch);
    EXPECT_EQ(Json::parse(test_support::MockOutputText(value)), expected[i]);
  }
}
}  // namespace
}  // namespace llm_edgeflow
