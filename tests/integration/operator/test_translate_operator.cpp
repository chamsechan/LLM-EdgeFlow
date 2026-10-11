#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_data_types.h"
#include "tests/integration/json_output_test_utils.h"
#include "tests/support/operator_test_fixture.h"

namespace llm_edgeflow {
using test_support::ParseJsonOutput;
using test_support::ScopedTestOperator;

using TranslateOperatorTest = test_support::OperatorTestFixture;

TEST_F(TranslateOperatorTest, CompleteResponse) {
  using namespace llm_edgeflow::operator_api;
  ScopedTestOperator instance(ops_);
  ASSERT_EQ(instance.Create("demo/fixtures/mock/pipeline_translate.conf"), 0)
      << instance.create_diagnostic();
  ASSERT_NE(instance.get(), nullptr);

  std::string json_input = "{\"query\": \"Hello world\"}";
  CompanyString cs_text{static_cast<int32_t>(json_input.size()),
                        const_cast<char*>(json_input.data())};

  CompanyOperatorEntityInput in{};
  in.service_type = kMockServiceTranslate;
  in.sentence_text = &cs_text;

  NamedIoBatch inputs(1);
  inputs[0]["trans_channel.entity_in"] = MakeBorrowedOperatorInput(&in);

  NamedIoBatch outputs(1);
  outputs[0]["trans_channel.json_out"] = std::shared_ptr<void>();

  int p_ret = ops_.Process(instance.get(), inputs, outputs);
  ASSERT_EQ(p_ret, 0) << "Process error: "
                      << llm_edgeflow::operator_api::GetOperatorLastError();
  auto out_sp = outputs[0]["trans_channel.json_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_dto = static_cast<CompanyOperatorOutput*>(out_sp.get());
  EXPECT_EQ(out_dto->service_type, kMockServiceTranslate);
  EXPECT_EQ(out_dto->status_code, 0);
  ASSERT_NE(out_dto->result_json, nullptr);
  EXPECT_GT(out_dto->result_json->length, 0);
  auto res_json = nlohmann::json::parse(
      std::string(out_dto->result_json->data, out_dto->result_json->length));
  EXPECT_TRUE(res_json.contains("translated"));

  outputs.clear();
  out_sp.reset();
  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
}

}  // namespace llm_edgeflow
