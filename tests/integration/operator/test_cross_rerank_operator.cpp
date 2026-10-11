#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "engine/backend_registry.h"
#include "platform_mock/operator_data_types.h"
#include "tests/integration/json_output_test_utils.h"
#include "tests/support/operator_test_fixture.h"

namespace llm_edgeflow {
using test_support::ParseJsonOutput;
using test_support::ScopedTestOperator;

class CrossRerankOperatorTest : public test_support::OperatorTestFixture {
 protected:
  void SetUp() override {
    OperatorTestFixture::SetUp();
    if (HasFatalFailure()) return;
#ifndef HAVE_ONNXRUNTIME
    GTEST_SKIP() << "ONNX Runtime disabled in this build";
#else
    if (!BackendRegistry::Instance().Find("onnxruntime").has_value())
      GTEST_SKIP() << "ONNX Runtime backend disabled in this build";
    const auto candidate =
        std::filesystem::temp_directory_path() /
        ("edgeflow-rerank-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(candidate));
    temp_dir_ = candidate;
    std::filesystem::create_directory(temp_dir_ / "models");
    std::filesystem::copy_file(EDGEFLOW_RERANK_ONNX_FIXTURE,
                               temp_dir_ / "models/rerank.onnx");
    std::filesystem::copy_file(EDGEFLOW_VOCAB_FIXTURE,
                               temp_dir_ / "models/vocab.txt");
    std::ifstream source("configs/pipeline_cross_rerank_cpu.json");
    ASSERT_TRUE(source.good());
    auto pipeline = nlohmann::json::parse(source);
    pipeline["models"][0]["file"] = "models/rerank.onnx";
    pipeline["models"][0]["params"]["tokenizer_file"] = "models/vocab.txt";
    pipeline["models"][0]["params"]["max_tokens"] = 32;
    std::ofstream json(temp_dir_ / "pipeline_cross_rerank.json");
    json << pipeline.dump(2);
    ASSERT_TRUE(json.good());
    std::ofstream conf(temp_dir_ / "pipeline_cross_rerank.conf");
    conf << R"({"pipe_path":"pipeline_cross_rerank.json"})";
    ASSERT_TRUE(conf.good());
#endif
  }
  void TearDown() override {
    OperatorTestFixture::TearDown();
    if (!temp_dir_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(temp_dir_, error);
      EXPECT_FALSE(error) << error.message();
    }
  }
  std::filesystem::path temp_dir_;
};

TEST_F(CrossRerankOperatorTest, DeviceValidationAndRankedCandidates) {
  ScopedTestOperator instance(ops_);
  EXPECT_NE(instance.Create("pipeline_cross_rerank.conf", temp_dir_.string(),
                            operator_api::ComputePlatform::kCpu, 25, 1),
            0);
  EXPECT_EQ(instance.get(), nullptr);
  ASSERT_EQ(instance.Create("pipeline_cross_rerank.conf", temp_dir_.string()),
            0)
      << instance.create_diagnostic();
  ASSERT_NE(instance.get(), nullptr);

  // Candidate cardinality and request batching share the same assertions.
  for (const auto& [rows, candidates] :
       {std::pair{1, 2}, std::pair{1, 5}, std::pair{4, 8}}) {
    SCOPED_TRACE(::testing::Message()
                 << "rows=" << rows << " candidates=" << candidates);
    std::vector<std::string> queries(rows);
    std::vector<std::vector<std::string>> passages(
        rows, std::vector<std::string>(candidates));
    std::vector<CompanyString> query_carriers(rows);
    std::vector<std::vector<CompanyString>> passage_carriers(
        rows, std::vector<CompanyString>(candidates));
    std::vector<CompanyOperatorRerankInput> requests(rows);
    operator_api::NamedIoBatch inputs(rows), outputs(rows);
    for (int row = 0; row < rows; ++row) {
      queries[row] = "如何申请7天无理由退款？请求" + std::to_string(row);
      query_carriers[row] = {static_cast<int32_t>(queries[row].size()),
                             queries[row].data()};
      requests[row].service_type = kMockServiceCrossRerank;
      requests[row].query_text = &query_carriers[row];
      requests[row].candidate_count = candidates;
      for (int i = 0; i < candidates; ++i) {
        passages[row][i] =
            "请求" + std::to_string(row) + "的候选条款" + std::to_string(i);
        passage_carriers[row][i] = {
            static_cast<int32_t>(passages[row][i].size()),
            passages[row][i].data()};
        requests[row].candidate_passages[i] = &passage_carriers[row][i];
      }
      inputs[row]["ranker.rerank_in"] =
          operator_api::MakeBorrowedOperatorInput(&requests[row]);
      outputs[row]["ranker.json_out"] = nullptr;
    }
    ASSERT_EQ(ops_.Process(instance.get(), inputs, outputs), 0)
        << operator_api::GetOperatorLastError();
    ASSERT_EQ(outputs.size(), rows);
    for (int row = 0; row < rows; ++row) {
      SCOPED_TRACE(row);
      const auto* output = static_cast<const CompanyOperatorOutput*>(
          outputs[row].at("ranker.json_out").get());
      ASSERT_NE(output, nullptr);
      EXPECT_EQ(output->service_type, kMockServiceCrossRerank);
      EXPECT_EQ(output->status_code, 0);
      const auto response = ParseJsonOutput(output);
      EXPECT_EQ(response.at("count"), candidates);
      ASSERT_EQ(response.at("scores").size(), candidates);
      ASSERT_EQ(response.at("sorted_indices").size(), candidates);
      std::vector<bool> seen(candidates, false);
      for (int i = 0; i < candidates; ++i) {
        const int index = response.at("sorted_indices").at(i).get<int>();
        ASSERT_GE(index, 0);
        ASSERT_LT(index, candidates);
        EXPECT_FALSE(seen[index]);
        seen[index] = true;
        if (i > 0) {
          EXPECT_GE(response.at("scores").at(i - 1).get<float>(),
                    response.at("scores").at(i).get<float>());
        }
      }
    }
    outputs.clear();
  }
  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
}

}  // namespace llm_edgeflow
