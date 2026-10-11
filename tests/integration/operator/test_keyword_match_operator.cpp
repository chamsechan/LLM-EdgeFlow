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

using KeywordMatchOperatorTest = test_support::OperatorTestFixture;

TEST_F(KeywordMatchOperatorTest, CompleteResponse) {
  using namespace llm_edgeflow::operator_api;
  ScopedTestOperator instance(ops_);
  ASSERT_EQ(instance.Create("configs/pipeline_keyword_match_rules.conf"), 0)
      << instance.create_diagnostic();
  ASSERT_NE(instance.get(), nullptr);

  std::string sentence = "客户要求加急处理VIP订单";
  CompanyString cs_sentence{static_cast<int32_t>(sentence.size()),
                            const_cast<char*>(sentence.data())};
  CompanyOperatorKeywordInput in{};
  in.service_type = kMockServiceKeywordMatch;
  in.sentence_text = &cs_sentence;

  ControlUpdateRulesParam rules_param{
      "{\"categories\":{\"VIP_SERVICE\":[\"VIP\",\"加急\"]}}"};
  ASSERT_EQ(ops_.Control(instance.get(),
                         static_cast<int>(ControlCommand::kUpdateRules),
                         &rules_param),
            0);

  NamedIoBatch inputs(1);
  inputs[0]["chan.keyword_in"] = MakeBorrowedOperatorInput(&in);

  NamedIoBatch outputs(1);
  outputs[0]["chan.json_out"] = std::shared_ptr<void>();

  int p_ret = ops_.Process(instance.get(), inputs, outputs);
  ASSERT_EQ(p_ret, 0) << "Process error: "
                      << llm_edgeflow::operator_api::GetOperatorLastError();
  auto out_sp = outputs[0]["chan.json_out"];
  ASSERT_NE(out_sp, nullptr);
  auto* out_dto = static_cast<CompanyOperatorOutput*>(out_sp.get());
  const auto out_dto_json = ParseJsonOutput(out_dto);
  EXPECT_EQ(out_dto->service_type, kMockServiceKeywordMatch);
  EXPECT_EQ(out_dto_json.at("is_hit").get<int>(), 1);
  ASSERT_NE(out_dto->result_json, nullptr);
  EXPECT_NE(
      std::string(out_dto->result_json->data, out_dto->result_json->length)
          .find("VIP"),
      std::string::npos);

  outputs.clear();
  out_sp.reset();
  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
}

}  // namespace llm_edgeflow
