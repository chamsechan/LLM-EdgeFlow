#include <gtest/gtest.h>

#include <algorithm>
#include <nlohmann/json.hpp>

#include "adapter/adapter_status.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/mock/platform_value_binding.h"
#include "contracts/inference_payloads.h"
#include "core/alg_context.h"
#include "core/pipeline_catalog.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_data_types.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {

class TextConvertersTest : public ::testing::Test {};

TEST_F(TextConvertersTest, TextPlainOperatorInputDecodeSuccess) {
  const auto* conv = IoConverterRegistry::Instance().FindInputConverter(
      "entity_in", "entity_extract");
  ASSERT_NE(conv, nullptr);
  ASSERT_NE(conv->decode_fn, nullptr);

  std::string text1 = "Hello world";
  std::string text2 = "Second sentence";
  CompanyString cs1{static_cast<int32_t>(text1.size()),
                    const_cast<char*>(text1.data())};
  CompanyString cs2{static_cast<int32_t>(text2.size()),
                    const_cast<char*>(text2.data())};

  CompanyOperatorEntityInput s1{kMockServiceEntityExtract, &cs1};
  CompanyOperatorEntityInput s2{kMockServiceEntityExtract, &cs2};

  ExternalInputBatchView view;
  view.count = 2;
  view.slots["entity_in"] = llm_edgeflow::BorrowInputForTest({&s1, &s2});
  view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

  test::ParsedInputOptions options(*conv);

  AlgContext ctx;
  AdapterStatus status;
  int ret =
      ::llm_edgeflow::test::DecodeForTest(*conv, view, options, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix(conv->type);
  ASSERT_NE(binding, nullptr);

  const auto* sentences = ctx.Read<TextBatch>("sentence_text");
  ASSERT_NE(sentences, nullptr);
  ASSERT_EQ(sentences->size(), 2U);
  EXPECT_EQ((*sentences)[0].data, "Hello world");
  EXPECT_EQ((*sentences)[1].data, "Second sentence");
}

TEST_F(TextConvertersTest, SameCarrierSelectsBusinessPayload) {
  // 两个输入转换器槽位与载体完全相同，但对同一请求的解析语义不同。
  const auto decode = [](const char* name, std::string text) {
    const auto* conv =
        IoConverterRegistry::Instance().FindInputConverter("entity_in", name);
    EXPECT_NE(conv, nullptr);
    if (!conv) return std::string();
    CompanyString value{static_cast<int32_t>(text.size()), text.data()};
    CompanyOperatorEntityInput input{*OperatorValueTypeRegistry::Instance()
                                          .GetBindingBySuffix(conv->type)
                                          ->ServiceType(conv->name),
                                     &value};
    ExternalInputBatchView view;
    view.count = 1;
    view.slots["entity_in"] = BorrowInputForTest({&input});
    view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

    test::ParsedInputOptions options(*conv);

    AlgContext context;
    AdapterStatus status;
    EXPECT_EQ(::llm_edgeflow::test::DecodeForTest(*conv, view, options,
                                                  &context, &status),
              COMPANY_ALG_SUCCESS);
    const auto* sentences = context.Read<TextBatch>(options.Port(
        name == std::string("translate") ? "query" : "sentence_text"));
    return sentences && sentences->size() == 1 ? sentences->front().data
                                               : std::string();
  };
  const std::string request = R"({"query":"hello"})";
  ASSERT_EQ(decode("translate", request), "hello");
  ASSERT_EQ(decode("entity_extract", request), request);
}

TEST_F(TextConvertersTest, MissingResultsDifferFromOutputCapacityFailures) {
  const auto* conv = IoConverterRegistry::Instance().FindOutputConverter(
      "json_out", "translate");
  ASSERT_NE(conv, nullptr);
  test::ParsedOutputOptions options(*conv);

  AlgContext context;
  TestOutputBatchView destination;
  AdapterStatus status;
  size_t written = 0;
  EXPECT_EQ(::llm_edgeflow::test::EncodeForTest(
                *conv, &context, options, &destination, &written, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  ASSERT_TRUE(context.Publish("translation", TextBatch{{0, 0, "hello"}}));
  EXPECT_EQ(::llm_edgeflow::test::EncodeForTest(
                *conv, &context, options, &destination, &written, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.FieldPath(), "destination");
  destination.count = 1;
  EXPECT_EQ(::llm_edgeflow::test::EncodeForTest(
                *conv, &context, options, &destination, &written, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.FieldPath(), "json_out");
  char bytes[2] = {};
  CompanyString text{1, bytes};
  CompanyOperatorOutput output{kMockServiceTranslate, nullptr, 0};
  output.result_json = &text;
  destination.leased_slots["json_out"] = {&output};
  destination.slot_types["json_out"] = "CompanyOperatorOutput";
  destination.SetCapacity("json_out", "result_json", 1);
  EXPECT_EQ(::llm_edgeflow::test::EncodeForTest(
                *conv, &context, options, &destination, &written, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.FieldPath(), "result_json");
  EXPECT_EQ(written, 0U);
}

TEST_F(TextConvertersTest, TranslationRowsOwnValuesAndFailAtomically) {
  const auto* input_converter =
      IoConverterRegistry::Instance().FindInputConverter("entity_in",
                                                         "translate");
  const auto* output_converter =
      IoConverterRegistry::Instance().FindOutputConverter("json_out",
                                                          "translate");
  ASSERT_NE(input_converter, nullptr);
  ASSERT_NE(output_converter, nullptr);
  std::string first = R"({"query":"first","lang":"en"})";
  std::string second = R"({"query":"second"})";
  CompanyString first_text{static_cast<int32_t>(first.size()), first.data()};
  CompanyString second_text{static_cast<int32_t>(second.size()), second.data()};
  CompanyOperatorEntityInput first_input{kMockServiceTranslate, &first_text};
  CompanyOperatorEntityInput second_input{kMockServiceTranslate, &second_text};
  test::AdapterHarness harness(input_converter, output_converter);
  ASSERT_EQ(harness.DecodeOperator({&first_input, &second_input}), 0);
  // Converted data must outlive mutation of the caller's bytes.
  first.assign(first.size(), 'x');
  const auto* queries = harness.Context().Read<TextBatch>("query");
  ASSERT_NE(queries, nullptr);
  ASSERT_EQ(queries->size(), 2U);
  EXPECT_EQ((*queries)[0].data, "first");
  EXPECT_EQ((*queries)[1].data, "second");
  EXPECT_EQ((*queries)[0].req_id, 0U);
  EXPECT_EQ((*queries)[1].req_id, 1U);
  EXPECT_EQ((*queries)[0].sub_id, 0U);
  EXPECT_EQ((*queries)[1].sub_id, 0U);
  ASSERT_TRUE(harness.Publish("translation",
                              TextBatch{{1, 0, "ans1"}, {0, 0, "ans0"}}));
  char bytes[2][128]{};
  CompanyString text[] = {{0, bytes[0]}, {0, bytes[1]}};
  std::vector<CompanyOperatorOutput> outputs = {
      {kMockServiceTranslate, &text[0], -1},
      {kMockServiceTranslate, &text[1], -1}};
  ASSERT_EQ(harness.EncodeOperator(&outputs, {{"result_json", 127}}), 0);
  ASSERT_EQ(outputs.size(), 2U);
  for (size_t i = 0; i < outputs.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(outputs[i].status_code, 0);
    EXPECT_EQ(nlohmann::json::parse(std::string(text[i].data, text[i].length)),
              nlohmann::json({{"translated", "ans" + std::to_string(i)}}));
  }
  // Invalid second rows must not leave even the first converted row published.
  first = R"({"query":"valid"})";
  first_text = {static_cast<int32_t>(first.size()), first.data()};
  for (std::string invalid :
       {"Hello plain text", R"({"text":"missing query"})"}) {
    SCOPED_TRACE(invalid);
    second_text = {static_cast<int32_t>(invalid.size()), invalid.data()};
    test::AdapterHarness rejected(input_converter);
    EXPECT_EQ(rejected.DecodeOperator({&first_input, &second_input}),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(rejected.Status().FieldPath(), "json");
    EXPECT_EQ(rejected.Status().SampleIndex(), 1);
    EXPECT_EQ(rejected.Context().Read<TextBatch>("query"), nullptr);
  }
}

}  // namespace llm_edgeflow
