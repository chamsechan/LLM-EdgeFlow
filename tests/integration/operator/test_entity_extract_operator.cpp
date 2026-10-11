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

using EntityExtractOperatorTest = test_support::OperatorTestFixture;

TEST_F(EntityExtractOperatorTest, CompleteResponse) {
  using namespace llm_edgeflow::operator_api;
  ScopedTestOperator instance(ops_);
  ASSERT_EQ(instance.Create("demo/fixtures/mock/pipeline_entity_extract.conf"),
            0)
      << instance.create_diagnostic();
  ASSERT_NE(instance.get(), nullptr);

  std::vector<std::string> text = {"张三在北京大学计算机学院攻读博士学位。",
                                   "User 1 works at Acme in Beijing.",
                                   "User 2 works at Acme in Beijing.",
                                   "User 3 works at Acme in Beijing."};
  std::vector<CompanyString> carriers(text.size());
  std::vector<CompanyOperatorEntityInput> requests(text.size());
  NamedIoBatch inputs(text.size()), outputs(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    carriers[i] = {static_cast<int32_t>(text[i].size()), text[i].data()};
    requests[i] = {kMockServiceEntityExtract, &carriers[i]};
    inputs[i]["ner_channel.entity_in"] =
        MakeBorrowedOperatorInput(&requests[i]);
    outputs[i]["ner_channel.json_out"] = nullptr;
  }
  ASSERT_EQ(ops_.Process(instance.get(), inputs, outputs), 0)
      << GetOperatorLastError();
  ASSERT_EQ(outputs.size(), text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    SCOPED_TRACE(i);
    const auto* output = static_cast<const CompanyOperatorOutput*>(
        outputs[i].at("ner_channel.json_out").get());
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->service_type, kMockServiceEntityExtract);
    EXPECT_EQ(output->status_code, 0);
    ASSERT_NE(output->result_json, nullptr);
    EXPECT_GT(output->result_json->length, 0);
    EXPECT_FALSE(ParseJsonOutput(output).is_discarded());
  }
  outputs.clear();
  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
}

}  // namespace llm_edgeflow
