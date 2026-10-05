#include <gtest/gtest.h>

#include <algorithm>
#include <nlohmann/json.hpp>

#include "adapter/adapter_status.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "contracts/inference_payloads.h"
#include "core/alg_context.h"
#include "core/pipeline_catalog.h"
#include "edgeflow/operator/types.h"
#include "platform_mock/operator_data_types.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {

class TextConvertersTest : public ::testing::Test {};

TEST_F(TextConvertersTest, TextPlainOperatorInputDecodeSuccess) {
  const auto* conv = IoConverterRegistry::Instance().FindInputConverter(
      "text.plain.operator.v1");
  ASSERT_NE(conv, nullptr);
  ASSERT_NE(conv->decode_fn, nullptr);

  std::string text1 = "Hello world";
  std::string text2 = "Second sentence";
  CompanyString cs1{static_cast<int32_t>(text1.size()),
                    const_cast<char*>(text1.data())};
  CompanyString cs2{static_cast<int32_t>(text2.size()),
                    const_cast<char*>(text2.data())};

  CompanyOperatorEntityInput s1{1001, &cs1};
  CompanyOperatorEntityInput s2{1002, &cs2};

  ExternalInputBatchView view;
  view.count = 2;
  view.slots["entity_in"] = llm_edgeflow::BorrowInputForTest({&s1, &s2});
  view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

  std::vector<uint64_t> request_ids;
  InputDecodeOptions options;
  options.request_ids = &request_ids;
  options.converter_id = conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  int ret = conv->decode_fn(view, options, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  ASSERT_EQ(request_ids.size(), 2U);
  EXPECT_EQ(request_ids[0], 1001U);
  EXPECT_EQ(request_ids[1], 1002U);

  const auto* sentences = ctx.Read<TextBatch>("input_sentences");
  ASSERT_NE(sentences, nullptr);
  ASSERT_EQ(sentences->size(), 2U);
  EXPECT_EQ((*sentences)[0].data, "Hello world");
  EXPECT_EQ((*sentences)[1].data, "Second sentence");
}

TEST_F(TextConvertersTest, TranslateJsonInputDecodeValidAndInvalid) {
  const auto* conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(conv, nullptr);
  ASSERT_NE(conv->decode_fn, nullptr);

  // 1. 含 query 字段的合法 JSON
  std::string valid_json = "{\"query\": \"Translate me!\", \"lang\": \"en\"}";
  CompanyString cs_valid{static_cast<int32_t>(valid_json.size()),
                         const_cast<char*>(valid_json.data())};
  CompanyOperatorEntityInput valid_s{2001, &cs_valid};

  ExternalInputBatchView valid_view;
  valid_view.count = 1;
  valid_view.slots["entity_in"] = llm_edgeflow::BorrowInputForTest({&valid_s});
  valid_view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

  std::vector<uint64_t> request_ids;
  InputDecodeOptions options;
  options.request_ids = &request_ids;
  options.converter_id = conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  int ret = conv->decode_fn(valid_view, options, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  const auto* sentences = ctx.Read<TextBatch>("input_sentences");
  ASSERT_NE(sentences, nullptr);
  ASSERT_EQ(sentences->size(), 1U);
  EXPECT_EQ((*sentences)[0].data, "Translate me!");

  // 2. 不含 query 的非法 JSON
  std::string invalid_json = "{\"text\": \"No query field\"}";
  CompanyString cs_invalid{static_cast<int32_t>(invalid_json.size()),
                           const_cast<char*>(invalid_json.data())};
  CompanyOperatorEntityInput invalid_s{2002, &cs_invalid};

  ExternalInputBatchView invalid_view;
  invalid_view.count = 1;
  invalid_view.slots["entity_in"] =
      llm_edgeflow::BorrowInputForTest({&invalid_s});
  invalid_view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

  AlgContext bad_ctx;
  AdapterStatus bad_status;
  ret = conv->decode_fn(invalid_view, options, &bad_ctx, &bad_status);
  EXPECT_EQ(ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(bad_status.FieldPath(), "json");
}

TEST_F(TextConvertersTest, TranslationJsonOutputEncodeOperator) {
  const auto* conv = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(conv, nullptr);
  ASSERT_NE(conv->encode_fn, nullptr);

  AlgContext ctx;
  std::vector<uint64_t> req_ids = {3001};
  TextBatch answers = {{0, 0, "Bonjour le monde"}};
  ctx.Publish("llm_answers", answers);

  char buf[2048] = {0};
  CompanyString cs_buf{2047, buf};
  CompanyOperatorEntityOutput out_struct{};
  out_struct.entities_json = &cs_buf;

  TestOutputBatchView dest;
  dest.count = 1;
  dest.leased_slots["entity_out"] = {&out_struct};
  dest.slot_types["entity_out"] = "CompanyOperatorEntityOutput";
  dest.SetCapacity("entity_out", "entities_json", sizeof(buf) - 1);

  OutputEncodeOptions options;
  options.request_ids = &req_ids;
  options.converter_id = conv->converter_id;

  size_t written = 0;
  AdapterStatus status;
  int ret = conv->encode_fn(&ctx, options, &dest, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(out_struct.request_id, 3001U);
  EXPECT_EQ(out_struct.status_code, 0);

  ASSERT_NE(out_struct.entities_json, nullptr);
  std::string json_res(out_struct.entities_json->data,
                       out_struct.entities_json->length);
  auto parsed = nlohmann::json::parse(json_res);
  EXPECT_EQ(parsed["translated"], "Bonjour le monde");
}

TEST_F(TextConvertersTest, ProductionBindingsUseDeclaredHostTypes) {
  const auto* entity_binding =
      IoBindingRegistry::Instance().FindBinding("entity_extract.operator.v1");
  ASSERT_NE(entity_binding, nullptr);

  const auto* keyword_binding =
      IoBindingRegistry::Instance().FindBinding("keyword_match.operator.v1");
  ASSERT_NE(keyword_binding, nullptr);

  // 实体抽取经 text.plain.operator.v1 使用 CompanyOperatorEntityInput
  EXPECT_EQ(entity_binding->input_converter_id, "text.plain.operator.v1");
  const auto* entity_conv = IoConverterRegistry::Instance().FindInputConverter(
      entity_binding->input_converter_id);
  ASSERT_NE(entity_conv, nullptr);
  EXPECT_EQ(ExternalType(entity_conv->external_slots),
            "CompanyOperatorEntityInput");

  // 关键词匹配经 keyword.plain.operator.v1 使用
  // CompanyOperatorKeywordInput
  EXPECT_EQ(keyword_binding->input_converter_id, "keyword.plain.operator.v1");
  const auto* keyword_conv = IoConverterRegistry::Instance().FindInputConverter(
      keyword_binding->input_converter_id);
  ASSERT_NE(keyword_conv, nullptr);
  EXPECT_EQ(ExternalType(keyword_conv->external_slots),
            "CompanyOperatorKeywordInput");

  // 输出 Converter 互不相同
  EXPECT_EQ(entity_binding->output_converter_id,
            "document.structured.operator.v1");
  EXPECT_EQ(keyword_binding->output_converter_id, "keyword.result.operator.v1");
}

TEST_F(TextConvertersTest, InputConverterReusedAcrossTestBindings) {
  // 证明同一个转换器 ID 可以在不同业务的绑定间复用：通过测试专用业务
  auto reuse_biz = *PipelineCatalog::FindBiz("entity_extract");
  reuse_biz.biz_name = "test_text_reuse";
  PipelineCatalog::RegisterBizDefinition(reuse_biz);
  IoBindingDefinition test_reuse_binding;
  test_reuse_binding.binding_id = "test_text_reuse.operator.v1";
  test_reuse_binding.biz_name = reuse_biz.biz_name;

  test_reuse_binding.input_converter_id = "text.plain.operator.v1";
  test_reuse_binding.output_converter_id = "document.structured.operator.v1";
  test_reuse_binding.max_batch_size = 64;

  if (!IoBindingRegistry::Instance().FindBinding(
          test_reuse_binding.binding_id)) {
    ASSERT_TRUE(
        IoBindingRegistry::Instance().RegisterBinding(test_reuse_binding));
  }

  const auto* b1 =
      IoBindingRegistry::Instance().FindBinding("entity_extract.operator.v1");
  const auto* b2 =
      IoBindingRegistry::Instance().FindBinding("test_text_reuse.operator.v1");
  ASSERT_NE(b1, nullptr);
  ASSERT_NE(b2, nullptr);
  EXPECT_EQ(b1->input_converter_id, b2->input_converter_id);
}

TEST_F(TextConvertersTest, SameCarrierDifferentPayloadBindingIsRejected) {
  // 两个输入转换器槽位与载体完全相同，但对同一请求的解析语义不同。
  const auto decode = [](const char* converter_id, std::string text) {
    const auto* conv =
        IoConverterRegistry::Instance().FindInputConverter(converter_id);
    EXPECT_NE(conv, nullptr);
    if (!conv) return std::string();
    CompanyString value{static_cast<int32_t>(text.size()), text.data()};
    CompanyOperatorEntityInput input{1, &value};
    ExternalInputBatchView view;
    view.count = 1;
    view.slots["entity_in"] = BorrowInputForTest({&input});
    view.slot_types["entity_in"] = "CompanyOperatorEntityInput";
    std::vector<uint64_t> request_ids;
    InputDecodeOptions options;
    options.request_ids = &request_ids;
    options.converter_id = conv->converter_id;
    AlgContext context;
    AdapterStatus status;
    EXPECT_EQ(conv->decode_fn(view, options, &context, &status),
              COMPANY_ALG_SUCCESS);
    const auto* sentences = context.Read<TextBatch>("input_sentences");
    return sentences && sentences->size() == 1 ? sentences->front().data
                                               : std::string();
  };
  const std::string request = R"({"query":"hello"})";
  ASSERT_EQ(decode("translate.json.operator.v1", request), "hello");
  ASSERT_EQ(decode("text.plain.operator.v1", request), request);

  // 同一业务只登记一个 binding：translate 不能再挂另一协议的 Converter。
  auto& bindings = IoBindingRegistry::Instance();
  auto alternate = *bindings.FindBinding("translate.operator.v1");
  alternate.binding_id = "translate.plain_text_input";
  alternate.input_converter_id = "text.plain.operator.v1";
  EXPECT_FALSE(bindings.RegisterBinding(alternate));
  EXPECT_EQ(bindings.FindBinding(alternate.binding_id), nullptr);
  std::vector<std::string> errors;
  EXPECT_FALSE(bindings.Audit(&errors));
  EXPECT_TRUE(std::any_of(errors.begin(), errors.end(), [](const auto& error) {
    return error.find("Duplicate IoBinding for biz_name 'translate'") !=
           std::string::npos;
  }));
  bindings.ResetConflictForTesting();
}

TEST_F(TextConvertersTest, MissingResultsDifferFromOutputCapacityFailures) {
  const auto* conv = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(conv, nullptr);
  OutputEncodeOptions options;
  options.converter_id = conv->converter_id;
  AlgContext context;
  TestOutputBatchView destination;
  AdapterStatus status;
  size_t written = 0;
  EXPECT_EQ(conv->encode_fn(&context, options, &destination, &written, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  ASSERT_TRUE(context.Publish("llm_answers", TextBatch{{0, 0, "hello"}}));
  EXPECT_EQ(conv->encode_fn(&context, options, &destination, &written, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "request_ids");
  const std::vector<uint64_t> request_ids{42};
  options.request_ids = &request_ids;
  EXPECT_EQ(conv->encode_fn(&context, options, &destination, &written, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.FieldPath(), "destination");
  destination.count = 1;
  EXPECT_EQ(conv->encode_fn(&context, options, &destination, &written, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.FieldPath(), "entity_out");
  char bytes[2] = {};
  CompanyString text{1, bytes};
  CompanyOperatorEntityOutput output{};
  output.entities_json = &text;
  destination.leased_slots["entity_out"] = {&output};
  destination.slot_types["entity_out"] = "CompanyOperatorEntityOutput";
  destination.SetCapacity("entity_out", "entities_json", 1);
  EXPECT_EQ(conv->encode_fn(&context, options, &destination, &written, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.FieldPath(), "entities_json");
  EXPECT_EQ(written, 0U);
}

}  // namespace llm_edgeflow
