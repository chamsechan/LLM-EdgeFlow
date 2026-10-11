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

using OcrInvoiceQaOperatorTest = test_support::OperatorTestFixture;

TEST_F(OcrInvoiceQaOperatorTest, BatchWithAlternateChannel) {
  ScopedTestOperator instance(ops_);
  ASSERT_EQ(instance.Create("demo/fixtures/mock/pipeline_ocr_invoice_qa.conf"),
            0)
      << instance.create_diagnostic();
  ASSERT_NE(instance.get(), nullptr);

  const std::vector<uint8_t> pixels1{255, 0, 0, 0, 255, 0};
  std::string p1 = "提取发票代码、号码与总金额";
  const std::vector<uint8_t> pixels2{0, 0, 255};
  std::string p2 = "提取购买方公司名称与税额";

  CompanyFrame frame1{kMockServiceOcrInvoiceQa, 1,      2, 6,
                      pixels1.data(),           nullptr};
  CompanyString p1_cs{static_cast<int32_t>(p1.size()),
                      const_cast<char*>(p1.data())};

  CompanyFrame frame2{kMockServiceOcrInvoiceQa, 1,      1, 3,
                      pixels2.data(),           nullptr};
  CompanyString p2_cs{static_cast<int32_t>(p2.size()),
                      const_cast<char*>(p2.data())};

  operator_api::NamedIoBatch inputs(2);
  inputs[0]["camera_0.frame"] =
      operator_api::MakeBorrowedOperatorInput(&frame1);
  inputs[0]["query_channel.string"] =
      operator_api::MakeBorrowedOperatorInput(&p1_cs);
  inputs[1]["camera_0.frame"] =
      operator_api::MakeBorrowedOperatorInput(&frame2);
  inputs[1]["query_channel.string"] =
      operator_api::MakeBorrowedOperatorInput(&p2_cs);

  operator_api::NamedIoBatch outputs(2);
  outputs[0]["ocr_result.json_out"] = nullptr;
  outputs[1]["ocr_result.json_out"] = nullptr;

  const int ret = ops_.Process(instance.get(), inputs, outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 2u);

  auto out1_sp = outputs[0]["ocr_result.json_out"];
  auto out2_sp = outputs[1]["ocr_result.json_out"];
  ASSERT_NE(out1_sp, nullptr);
  ASSERT_NE(out2_sp, nullptr);

  auto* out1 = static_cast<CompanyOperatorOutput*>(out1_sp.get());
  const auto out1_json = ParseJsonOutput(out1);
  auto* out2 = static_cast<CompanyOperatorOutput*>(out2_sp.get());
  const auto out2_json = ParseJsonOutput(out2);

  for (const auto* output : {out1, out2}) {
    EXPECT_EQ(output->service_type, kMockServiceOcrInvoiceQa);
    EXPECT_EQ(output->status_code, 0);
    ASSERT_NE(output->result_json, nullptr);
    EXPECT_GT(output->result_json->length, 0);
  }

  EXPECT_EQ(out1_json.at("detected_box_count").get<int>(), 6);
  EXPECT_EQ(out2_json.at("detected_box_count").get<int>(), 6);

  const auto& j1 = out1_json.at("result");
  EXPECT_TRUE(j1.contains("invoice_code") && j1.contains("total_amount"));

  out1_sp.reset();
  out2_sp.reset();
  outputs.clear();
  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
}

}  // namespace llm_edgeflow
