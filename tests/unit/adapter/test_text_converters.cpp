#include <gtest/gtest.h>

#include "adapter/adapter_status.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "contracts/inference_payloads.h"
#include "core/alg_context.h"
#include "edgeflow/c_api.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {

class TextConvertersTest : public ::testing::Test {};

TEST_F(TextConvertersTest, TextPlainCAbiInputDecodeSuccess) {
  const auto* conv =
      IoConverterRegistry::Instance().FindInputConverter("text.plain.cabi.v1");
  ASSERT_NE(conv, nullptr);
  ASSERT_NE(conv->decode_fn, nullptr);

  CompanyEntityInputStruct s1{1001, "Hello world"};
  CompanyEntityInputStruct s2{1002, "Second sentence"};
  const void* items[] = {&s1, &s2};

  ExternalInputBatchView view;
  view.items = items;
  view.count = 2;

  InputPortBindings bindings({{"raw_request_ids", "raw_request_ids"},
                              {"input_sentences", "input_sentences"}});
  InputDecodeOptions options;
  options.converter_id = conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  int ret = conv->decode_fn(view, options, bindings, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  const auto* req_ids = ctx.Read<std::vector<uint64_t>>("raw_request_ids");
  ASSERT_NE(req_ids, nullptr);
  ASSERT_EQ(req_ids->size(), 2U);
  EXPECT_EQ((*req_ids)[0], 1001U);
  EXPECT_EQ((*req_ids)[1], 1002U);

  const auto* sentences = ctx.Read<TextBatch>("input_sentences");
  ASSERT_NE(sentences, nullptr);
  ASSERT_EQ(sentences->size(), 2U);
  EXPECT_EQ((*sentences)[0].data, "Hello world");
  EXPECT_EQ((*sentences)[1].data, "Second sentence");
}

TEST_F(TextConvertersTest, TranslateJsonInputDecodeValidAndInvalid) {
  const auto* conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.cabi.v1");
  ASSERT_NE(conv, nullptr);
  ASSERT_NE(conv->decode_fn, nullptr);

  // 1. Valid JSON with query field
  CompanyEntityInputStruct valid_s{
      2001, "{\"query\": \"Translate me!\", \"lang\": \"en\"}"};
  const void* valid_items[] = {&valid_s};

  ExternalInputBatchView valid_view;
  valid_view.items = valid_items;
  valid_view.count = 1;

  InputPortBindings bindings({{"raw_request_ids", "raw_request_ids"},
                              {"input_sentences", "input_sentences"}});
  InputDecodeOptions options;
  options.converter_id = conv->converter_id;

  AlgContext ctx;
  AdapterStatus status;
  int ret = conv->decode_fn(valid_view, options, bindings, &ctx, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);

  const auto* sentences = ctx.Read<TextBatch>("input_sentences");
  ASSERT_NE(sentences, nullptr);
  ASSERT_EQ(sentences->size(), 1U);
  EXPECT_EQ((*sentences)[0].data, "Translate me!");

  // 2. Invalid JSON without query
  CompanyEntityInputStruct invalid_s{2002, "{\"text\": \"No query field\"}"};
  const void* invalid_items[] = {&invalid_s};
  ExternalInputBatchView invalid_view;
  invalid_view.items = invalid_items;
  invalid_view.count = 1;

  AlgContext bad_ctx;
  AdapterStatus bad_status;
  ret = conv->decode_fn(invalid_view, options, bindings, &bad_ctx, &bad_status);
  EXPECT_EQ(ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(bad_status.FieldPath(), "json");
}

TEST_F(TextConvertersTest, TranslationJsonOutputEncodeCAbi) {
  const auto* conv = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.cabi.v1");
  ASSERT_NE(conv, nullptr);
  ASSERT_NE(conv->encode_fn, nullptr);

  AlgContext ctx;
  std::vector<uint64_t> req_ids = {3001};
  TextBatch answers = {{0, 0, "Bonjour le monde"}};
  ctx.Publish("raw_request_ids", req_ids);
  ctx.Publish("llm_answers", answers);

  CompanyEntityOutputStruct out_struct{};
  void* out_ptrs[] = {&out_struct};

  ExternalOutputBatchView dest;
  dest.items = out_ptrs;
  dest.capacity = 1;

  OutputPortBindings bindings(
      {{"raw_request_ids", "raw_request_ids"}, {"llm_answers", "llm_answers"}});
  OutputEncodeOptions options;
  options.converter_id = conv->converter_id;

  size_t written = 0;
  AdapterStatus status;
  int ret = conv->encode_fn(&ctx, bindings, options, &dest, &written, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(out_struct.request_id, 3001U);
  EXPECT_EQ(out_struct.status_code, 0);

  auto parsed = nlohmann::json::parse(out_struct.entities_json);
  EXPECT_EQ(parsed["translated"], "Bonjour le monde");
}

TEST_F(TextConvertersTest, InputConverterReusedAcrossBindings) {
  const auto* entity_binding =
      IoBindingRegistry::Instance().FindBinding("entity_extract.cabi.v1");
  ASSERT_NE(entity_binding, nullptr);

  const auto* keyword_binding =
      IoBindingRegistry::Instance().FindBinding("keyword_match.cabi.v1");
  ASSERT_NE(keyword_binding, nullptr);

  // Both bindings must reuse the exact same text.plain.cabi.v1 converter
  EXPECT_EQ(entity_binding->input_converter_id, "text.plain.cabi.v1");
  EXPECT_EQ(keyword_binding->input_converter_id, "text.plain.cabi.v1");

  // But have distinct output converters
  EXPECT_EQ(entity_binding->output_converter_id, "document.structured.cabi.v1");
  EXPECT_EQ(keyword_binding->output_converter_id, "keyword.result.cabi.v1");
}

}  // namespace llm_edgeflow
