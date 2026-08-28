#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"

namespace alg_framework {

class CommonNodesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();
    RuntimeOptions options;
    options.device_id = 0;
    session_ctx_->SetRuntimeOptions(options);

    // Register Mock Engines
    auto emb_engine = EngineFactory::Instance().Create("mock_npu_embedding");
    ASSERT_NE(emb_engine, nullptr);
    emb_engine->Load("./models/bge_base.bin",
                     {{"max_batch_size", 4}, {"embedding_dim", 384}});
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "embed_model_v1", std::move(emb_engine)));

    auto rerank_engine = EngineFactory::Instance().Create("mock_npu_rerank");
    ASSERT_NE(rerank_engine, nullptr);
    rerank_engine->Load("./models/bge_reranker.bin", {{"max_batch_size", 4}});
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "rerank_model_v1", std::move(rerank_engine)));

    auto llm_engine = EngineFactory::Instance().Create("mock_npu_llm");
    ASSERT_NE(llm_engine, nullptr);
    llm_engine->Load("./models/qwen.bin",
                     {{"max_batch_size", 2}, {"max_seq_len", 512}});
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "llm_model_v1", std::move(llm_engine)));

    auto asr_engine = EngineFactory::Instance().Create("mock_npu_asr");
    ASSERT_NE(asr_engine, nullptr);
    asr_engine->Load("./models/paraformer.bin", {{"max_batch_size", 2}});
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "mock_asr_model", std::move(asr_engine)));

    auto ocr_engine = EngineFactory::Instance().Create("mock_npu_ocr");
    ASSERT_NE(ocr_engine, nullptr);
    ocr_engine->Load("./models/ch_ppocr.bin", {{"max_batch_size", 2}});
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "ocr_model_v1", std::move(ocr_engine)));
  }

  std::unique_ptr<SessionContext> session_ctx_;
};

// 1. TextTemplateNode: placeholder validation, join, overflow policy, control
TEST_F(CommonNodesTest, TextTemplateNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  // 1.1 Invalid placeholder should fail init
  nlohmann::json invalid_cfg = {{"template", "Hello {{unknown_variable}}!"}};
  EXPECT_FALSE(node->Init(invalid_cfg, session_ctx_.get()));

  // 1.2 Valid placeholder and static values
  nlohmann::json valid_cfg = {
      {"template",
       "Prefix: {{tag}} | Query: {{primary}} | Context: {{context}}"},
      {"values", {{"tag", "TEST_TAG"}}},
      {"overflow_policy", "truncate"},
      {"max_length", 128}};
  EXPECT_TRUE(node->Init(valid_cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch primary;
  primary.emplace_back(1, 0, "What is LLM?");
  primary.emplace_back(2, 0, "How to build?");
  ctx.Set("primary", primary);

  RankedTextBatch context;
  context.emplace_back(
      1, 0, RankedCandidate("LLM is Large Language Model", 0.95f, 1));
  context.emplace_back(
      2, 0, RankedCandidate("Follow 4-layer architecture", 0.90f, 1));
  ctx.Set("context", context);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_NE((*out)[0].data.find("TEST_TAG"), std::string::npos);
  EXPECT_NE((*out)[0].data.find("What is LLM?"), std::string::npos);
  EXPECT_NE((*out)[0].data.find("Large Language Model"), std::string::npos);

  // 1.3 Control update prompt
  nlohmann::json update_json = {{"template", "NewTemplate: {{primary}}"},
                                {"values", nlohmann::json::object()}};
  NodeControlResult c_res =
      node->Control(kControlCmdUpdatePrompt, update_json.dump());
  EXPECT_EQ(c_res.status, NodeControlStatus::kHandled);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out2 = ctx.Get<TextBatch>("text");
  ASSERT_NE(out2, nullptr);
  EXPECT_EQ((*out2)[0].data, "NewTemplate: What is LLM?");
}

// 2. TextChunkNode: chunking, overlap, provenance
TEST_F(CommonNodesTest, TextChunkNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"chunk_size", 10}, {"overlap", 2}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch input;
  input.emplace_back(101, 0, "0123456789abcdefghij");  // 20 chars
  ctx.Set("text", input);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<TextBatch>("chunks");
  ASSERT_NE(out, nullptr);
  ASSERT_GE(out->size(), 2u);
  EXPECT_EQ((*out)[0].req_id, 101u);
  EXPECT_EQ((*out)[0].sub_id, 0u);
  EXPECT_EQ((*out)[1].req_id, 101u);
  EXPECT_EQ((*out)[1].sub_id, 1u);
}

// 3. TextRuleMatchNode: categories, regex named captures, constants, control
TEST_F(CommonNodesTest, TextRuleMatchNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("TextRuleMatchNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"categories", {{"GREETING", {"你好", "hello"}}}},
                        {"rules",
                         {{{"id", "nav_dest"},
                           {"strategy", "regex"},
                           {"pattern", "导航到(?<destination>.+)"},
                           {"category", "NAVIGATION"},
                           {"score", 1.0},
                           {"constants", {{"avoid_toll", "false"}}}}}}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch input;
  input.emplace_back(1, 0, "你好，请帮我导航到北京天安门");
  input.emplace_back(2, 0, "今天天气怎么样");
  ctx.Set("text", input);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<RuleMatchBatch>("matches");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);

  // Sample 1 hits both GREETING and NAVIGATION
  EXPECT_EQ((*out)[0].data.is_hit, 1);
  EXPECT_EQ((*out)[0].data.captures.at("destination"), "北京天安门");
  EXPECT_EQ((*out)[0].data.constants.at("avoid_toll"), "false");
  EXPECT_EQ((*out)[0].data.details["slots"]["destination"], "北京天安门");

  // Sample 2 no hit
  EXPECT_EQ((*out)[1].data.is_hit, 0);

  // Dynamic rule update via Control
  nlohmann::json update_rules = {{"rules",
                                  {{{"id", "weather"},
                                    {"strategy", "regex"},
                                    {"pattern", "(?<city>.+)天气"},
                                    {"category", "WEATHER"},
                                    {"score", 0.9}}}}};
  NodeControlResult c_res =
      node->Control(kControlCmdUpdateRules, update_rules.dump());
  EXPECT_EQ(c_res.status, NodeControlStatus::kHandled);

  AlgContext ctx2;
  TextBatch input2;
  input2.emplace_back(3, 0, "北京天气怎么样");
  ctx2.Set("text", input2);
  EXPECT_EQ(node->Process(&ctx2), 0);
  const auto* out2 = ctx2.Get<RuleMatchBatch>("matches");
  ASSERT_NE(out2, nullptr);
  EXPECT_EQ((*out2)[0].data.is_hit, 1);
  EXPECT_EQ((*out2)[0].data.captures.at("city"), "北京");
}

// 4. StructuredJsonParseNode: direct, markdown block, auto-close, failure
// policies
TEST_F(CommonNodesTest, StructuredJsonParseNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("StructuredJsonParseNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"fallback_json", "{\"entities\":[]}"},
                        {"extract_json_block", true},
                        {"failure_policy", "configured_fallback"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch input;
  // 1. Direct JSON
  input.emplace_back(1, 0, "{\"entities\": [\"Apple\", \"Google\"]}");
  // 2. Markdown block
  input.emplace_back(
      2, 0,
      "Here is the result:\n```json\n{\"entities\": [\"DeepMind\"]}\n```");
  // 3. Auto-close unclosed array
  input.emplace_back(3, 0, "Found entities: [\"TensorFlow\", \"PyTorch\"");
  // 4. Broken text
  input.emplace_back(4, 0, "No valid json here at all");
  ctx.Set("text", input);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<StructuredDocumentBatch>("document");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 4u);

  EXPECT_EQ((*out)[0].data.parse_status, JsonParseStatus::kOk);
  EXPECT_EQ((*out)[1].data.parse_status,
            JsonParseStatus::kExtractedFromMarkdown);
  EXPECT_EQ((*out)[2].data.parse_status, JsonParseStatus::kAutoClosed);
  EXPECT_EQ((*out)[3].data.parse_status, JsonParseStatus::kFallbackApplied);
  EXPECT_EQ((*out)[3].data.json_payload, "{\"entities\":[]}");
}

// 5. TextEmbeddingNode: L2 normalization & session-level cache
TEST_F(CommonNodesTest, TextEmbeddingNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "embed_model_v1"},
                        {"normalize", true},
                        {"lifetime", "session"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx1;
  TextBatch input;
  input.emplace_back(1, 0, "Static Policy Document 1");
  input.emplace_back(1, 1, "Static Policy Document 2");
  ctx1.Set("text", input);

  EXPECT_EQ(node->Process(&ctx1), 0);
  const auto* out1 = ctx1.Get<EmbeddingBatch>("embedding");
  ASSERT_NE(out1, nullptr);
  ASSERT_EQ(out1->size(), 2u);

  // Subsequent call should reuse session cache seamlessly
  AlgContext ctx2;
  ctx2.Set("text", input);
  EXPECT_EQ(node->Process(&ctx2), 0);
  const auto* out2 = ctx2.Get<EmbeddingBatch>("embedding");
  ASSERT_NE(out2, nullptr);
  ASSERT_EQ(out2->size(), 2u);
  EXPECT_EQ((*out1)[0].data, (*out2)[0].data);
}

// 6. VectorTopKNode: cosine similarity & shared candidate pool
TEST_F(CommonNodesTest, VectorTopKNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("VectorTopKNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"top_k", 2}, {"min_score", 0.0}, {"metric", "cosine"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  EmbeddingBatch queries;
  queries.emplace_back(1, 0, std::vector<float>{1.0f, 0.0f, 0.0f});
  ctx.Set("queries", queries);

  EmbeddingBatch candidates;
  candidates.emplace_back(0, 0,
                          std::vector<float>{1.0f, 0.0f, 0.0f});  // sim 1.0
  candidates.emplace_back(
      0, 1, std::vector<float>{0.707f, 0.707f, 0.0f});  // sim 0.707
  candidates.emplace_back(0, 2,
                          std::vector<float>{0.0f, 1.0f, 0.0f});  // sim 0.0
  ctx.Set("candidates", candidates);

  TextBatch cand_texts;
  cand_texts.emplace_back(0, 0, "Exact match passage");
  cand_texts.emplace_back(0, 1, "Partial match passage");
  cand_texts.emplace_back(0, 2, "Orthogonal passage");
  ctx.Set("candidate_texts", cand_texts);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<RankedTextBatch>("ranked");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ((*out)[0].data.text, "Exact match passage");
  EXPECT_FLOAT_EQ((*out)[0].data.score, 1.0f);
  EXPECT_EQ((*out)[1].data.text, "Partial match passage");
}

// 7. TextRerankNode: cross-encoder reranking
TEST_F(CommonNodesTest, TextRerankNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("TextRerankNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "rerank_model_v1"}, {"top_k", 1}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch queries;
  queries.emplace_back(1, 0, "user query");
  ctx.Set("queries", queries);

  RankedTextBatch candidates;
  candidates.emplace_back(1, 0, RankedCandidate("Candidate A", 0.5f, 1));
  candidates.emplace_back(1, 1, RankedCandidate("Candidate B", 0.8f, 2));
  ctx.Set("candidates", candidates);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<RankedTextBatch>("ranked");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
}

// 8. LlmGenerateNode: prompt inference
TEST_F(CommonNodesTest, LlmGenerateNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("LlmGenerateNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {
      {"bind_model", "llm_model_v1"}, {"temperature", 0.5}, {"max_tokens", 64}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch prompts;
  prompts.emplace_back(1, 0, "Explain quantum physics");
  ctx.Set("prompt", prompts);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_FALSE((*out)[0].data.empty());
}

// 9. AsrTranscribeNode: speech transcription
TEST_F(CommonNodesTest, AsrTranscribeNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("AsrTranscribeNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "mock_asr_model"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  AudioPcmBatch audio;
  audio.emplace_back(1, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.1f), 16000));
  ctx.Set("audio", audio);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_FALSE((*out)[0].data.empty());
}

// 10. OcrDetectNode: OCR bounding box & text recognition
TEST_F(CommonNodesTest, OcrDetectNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("OcrDetectNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "ocr_model_v1"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  ImageRefBatch images;
  images.emplace_back(1, 0, "mock_invoice.jpg");
  ctx.Set("images", images);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out_doc = ctx.Get<OcrDocumentBatch>("document");
  const auto* out_text = ctx.Get<TextBatch>("text");
  ASSERT_NE(out_doc, nullptr);
  ASSERT_NE(out_text, nullptr);
  ASSERT_EQ(out_doc->size(), 1u);
  EXPECT_FALSE((*out_text)[0].data.empty());
}

// 11. TextCorpusSourceNode: static corpus emission
TEST_F(CommonNodesTest, TextCorpusSourceNodeComprehensive) {
  auto node = NodeFactory::Instance().Create("TextCorpusSourceNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {
      {"corpus", {"Clause 1: Compliance", "Clause 2: Security"}}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Get<TextBatch>("corpus");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ((*out)[0].data, "Clause 1: Compliance");
  EXPECT_EQ((*out)[1].data, "Clause 2: Security");
}

}  // namespace alg_framework
