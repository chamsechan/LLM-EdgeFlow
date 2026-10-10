#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "dev_support/inference/test_biz_models.h"
#include "dev_support/inference/test_capability_models.h"
#include "dev_support/inference/test_causal_lm_backend.h"
#include "dev_support/inference/test_tensor_backend.h"
#include "engine/model_interface.h"
#include "nodes/generate_parameters.h"
#include "nodes/node_error_codes.h"
#include "tests/support/model_registration.h"
#include "tests/support/node_harness.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/pipeline_test_utils.h"

namespace llm_edgeflow {

class NodeAuthoringExamplesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    session_ctx_ = std::make_unique<SessionContext>();
    RuntimeOptions options;
    options.device_id = 0;
    session_ctx_->SetRuntimeOptions(options);

    ASSERT_TRUE(
        RegisterTestModel(session_ctx_->GetModelManager(), "embed_model",
                          std::make_shared<test::TestBizEmbeddingModel>(
                              std::make_shared<test::TestTensorSession>(
                                  "embedding.fixture", BatchPolicy{4, 4}),
                              384),
                          "test-v1"));

    ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(),
                                  "rerank_model",
                                  std::make_shared<test::TestBizRerankModel>(
                                      std::make_shared<test::TestTensorSession>(
                                          "rerank.fixture", BatchPolicy{4, 4})),
                                  "test-v1"));

    ASSERT_TRUE(
        RegisterTestModel(session_ctx_->GetModelManager(), "llm_model",
                          std::make_shared<test::TestBizLlmModel>(
                              std::make_shared<test::TestCausalLmSession>(
                                  "llm.fixture", BatchPolicy{2, 2})),
                          "test-v1"));

    auto asr_model = std::make_shared<test::TestAsrModel>();
    ASSERT_TRUE(RegisterTestModel(
        session_ctx_->GetModelManager(), "asr_model", std::move(asr_model),
        "test-revision", "test_asr_model", "asr", "test_tensor_backend"));

    auto ocr_model = std::make_shared<test::TestOcrModel>();
    ASSERT_TRUE(RegisterTestModel(
        session_ctx_->GetModelManager(), "ocr_model", std::move(ocr_model),
        "test-revision", "test_ocr_model", "ocr", "test_tensor_backend"));
  }

  std::unique_ptr<SessionContext> session_ctx_;
};

TEST_F(NodeAuthoringExamplesTest,
       ModelBindingsAreExplicitWithoutInstanceDefaults) {
  for (const char* name :
       {"llm_generate", "text_embedding", "text_rerank", "asr_transcribe",
        "ocr_detect", "prompt_guided_llm"}) {
    SCOPED_TRACE(name);
    const auto definition = PipelineCatalog::FindNode(name);
    ASSERT_TRUE(definition.has_value());
    ASSERT_EQ(definition->model_dependencies.size(), 1U);
    const auto& dependency = definition->model_dependencies.front();
    const auto field = std::find_if(
        definition->config_fields.begin(), definition->config_fields.end(),
        [&](const auto& candidate) {
          return candidate.name == dependency.config_field;
        });
    ASSERT_NE(field, definition->config_fields.end());
    EXPECT_TRUE(field->required);
    EXPECT_TRUE(field->default_value.is_null());
    EXPECT_FALSE(field->semantic.empty());

    auto node = NodeRegistry::Instance().Create(name);
    ASSERT_NE(node, nullptr);
    std::string error;
    nlohmann::json params = nlohmann::json::object();
    if (std::string(name) == "llm_generate")
      params["endpoints"] = {{"answer", nlohmann::json::object()}};
    EXPECT_FALSE(InitNodeForTest(*node, params, session_ctx_.get(), &error));
    EXPECT_NE(error.find("bind_model"), std::string::npos) << error;
  }
}

namespace {

class StarterEmbeddingModel final : public IEmbeddingModel {
 public:
  const std::string& ImplName() const noexcept override {
    static const std::string type = "starter_embedding";
    return type;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string model_type = "embedding";
    return model_type;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  int Embed(const TextBatch& input, EmbeddingBatch* output,
            std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
    ++calls;
    output->clear();
    for (const auto& item : input) {
      output->emplace_back(item.req_id, item.sub_id,
                           empty_embedding ? std::vector<float>{}
                                           : std::vector<float>{0.1f, 0.2f});
    }
    return result;
  }
  int calls = 0;
  int result = 0;
  bool empty_embedding = false;
};

template <typename Input, typename Output>
void CheckScaffoldExecution(const std::string& name, const std::string& model,
                            SessionContext* session) {
  auto node = NodeRegistry::Instance().Create(name);
  ASSERT_NE(node, nullptr);
  // 解析后的键与逻辑名不同：同时覆盖类型化绑定。
  ValidatedNodePlan plan;
  plan.ports = {{"input", "source", BlackboardTypeTraits<Input>::TypeName(),
                 "1:1", "preserve", "request", PortDirection::kInput},
                {"output", "result", BlackboardTypeTraits<Output>::TypeName(),
                 "1:1", "preserve", "request", PortDirection::kOutput}};
  const auto def = PipelineCatalog::FindNode(name);
  ASSERT_TRUE(def.has_value());
  nlohmann::json config = nlohmann::json::object();
  for (const auto& dep : def->model_dependencies) {
    config[dep.config_field] = model;
    plan.model_bindings.push_back(
        {dep.name, dep.model_type, dep.config_field, model});
  }
  // 与 Validator 相同：按 Definition 字段填入默认值。
  ASSERT_TRUE(ValidateAndNormalizeFields(def->config_fields, config,
                                         &plan.normalized_params, nullptr));
  ASSERT_TRUE(node->Init({&plan, session}));
  Input input;
  for (const auto& id :
       std::vector<std::pair<uint64_t, uint32_t>>{{7, 3}, {11, 9}, {7, 5}}) {
    input.emplace_back(id.first, id.second,
                       decltype(typename Input::value_type{}.data){});
  }
  AlgContext ctx;
  ctx.Publish("source", input);
  ASSERT_EQ(node->Process(&ctx), 0) << ctx.GetErrorMessage();
  const auto* output = ctx.Read<Output>("result");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    EXPECT_EQ((*output)[i].req_id, input[i].req_id);
    EXPECT_EQ((*output)[i].sub_id, input[i].sub_id);
  }
}

}  // namespace

TEST_F(NodeAuthoringExamplesTest,
       BatchStarterJoinsContextByRequestAndRetriesOnlyModelErrors) {
  for (int fault = 0; fault < 4; ++fault) {
    SCOPED_TRACE(fault);
    auto model = std::make_shared<test::PromptCaptureLlmModel>();
    model->response_prefix.clear();
    model->response_suffix.clear();
    model->fail_first_calls = fault == 1 ? 1 : 0;
    model->result = fault == 2 ? -99 : 0;
    model->wrong_request = fault == 3;
    NodeHarness harness("starter_batch");
    harness.Config({{"bind_model", "starter_llm"}, {"retry_once", true}});
    harness.BindModel("starter_llm", model);
    harness.TextInputWithBatch("questions",
                               TextBatch{{17, 4, "first"}, {29, 8, "second"}});
    harness.TextInputWithBatch(
        "context", TextBatch{{29, 0, "B"}, {17, 0, "A"}, {17, 1, "A2"}});
    auto result = harness.Run();
    if (fault < 2) {
      ASSERT_TRUE(result.ok()) << result.diagnostic();
      EXPECT_EQ(result.TextValues("output"),
                (std::vector<std::string>{"A\nA2\nfirst", "B\nsecond"}));
      const auto* output = result.Output<TextBatch>("output");
      ASSERT_NE(output, nullptr);
      EXPECT_EQ((*output)[0].req_id, 17u);
      EXPECT_EQ((*output)[0].sub_id, 4u);
      EXPECT_EQ((*output)[1].req_id, 29u);
      EXPECT_EQ((*output)[1].sub_id, 8u);
    } else {
      EXPECT_FALSE(result.ok());
      EXPECT_FALSE(result.init_failed()) << result.diagnostic();
      EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
    }
    EXPECT_EQ(model->calls, fault == 1 || fault == 2 ? 2 : 1);
  }
}

TEST_F(NodeAuthoringExamplesTest,
       MultiModelStarterExecutesBothCapabilitiesAndStopsOnFailure) {
  for (int fault = 0; fault < 4; ++fault) {
    SCOPED_TRACE(fault);
    auto llm = std::make_shared<test::PromptCaptureLlmModel>();
    llm->response_prefix.clear();
    llm->response_suffix.clear();
    auto embedding = std::make_shared<StarterEmbeddingModel>();
    embedding->result = fault == 1 ? -99 : 0;
    embedding->empty_embedding = fault == 2;
    llm->wrong_sub_id = fault == 3;
    NodeHarness harness("starter_multi_model");
    harness.Config(
        {{"bind_llm", "starter_llm"}, {"bind_embedding", "starter_embedding"}});
    harness.BindModel("starter_llm", llm);
    harness.BindModel("starter_embedding", embedding);
    harness.TextInputWithBatch("questions",
                               TextBatch{{17, 4, "first"}, {29, 8, "second"}});
    auto result = harness.Run();
    if (fault == 0) {
      ASSERT_TRUE(result.ok()) << result.diagnostic();
      EXPECT_EQ(result.TextValues("output"),
                (std::vector<std::string>{"first\nEmbedding dimensions: 2",
                                          "second\nEmbedding dimensions: 2"}));
      const auto* output = result.Output<TextBatch>("output");
      ASSERT_NE(output, nullptr);
      EXPECT_EQ((*output)[0].req_id, 17u);
      EXPECT_EQ((*output)[0].sub_id, 4u);
      EXPECT_EQ((*output)[1].req_id, 29u);
      EXPECT_EQ((*output)[1].sub_id, 8u);
    } else {
      EXPECT_FALSE(result.ok());
      EXPECT_FALSE(result.init_failed()) << result.diagnostic();
      EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
    }
    EXPECT_EQ(embedding->calls, 1);
    EXPECT_EQ(llm->calls, fault == 1 || fault == 2 ? 0 : 1);
  }
}

TEST_F(NodeAuthoringExamplesTest,
       PromptAndGeneratedLlmNodesFailWithoutPublishing) {
  auto model = std::make_shared<test::PromptCaptureLlmModel>();
  ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(),
                                "prompt_contract", model, "v1"));
  for (const char* name :
       {"prompt_guided_llm", "scaffold_model_llm", "scaffold_tutorial_llm"}) {
    SCOPED_TRACE(name);
    auto node = NodeRegistry::Instance().Create(name);
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(InitNodeForTest(*node, {{"bind_model", "prompt_contract"}},
                                session_ctx_.get()));
    for (int fault = 0; fault < 6; ++fault) {
      SCOPED_TRACE(fault);
      model->result = fault == 2 ? -99 : 0;
      model->wrong_count = fault == 3;
      model->wrong_request = fault == 4;
      model->wrong_sub_id = fault == 5;
      AlgContext ctx;
      if (fault == 1) ctx.Publish("input", Int32Batch{{1, 0, 123}});
      if (fault >= 2)
        ctx.Publish("input", TextBatch{{17, 4, "a"}, {29, 8, "b"}});
      EXPECT_NE(node->Process(&ctx), 0);
      EXPECT_FALSE(ctx.IsOk());
      EXPECT_FALSE(ctx.Has("output"));
    }
    const int before = model->calls;
    AlgContext empty;
    empty.Publish("input", TextBatch{});
    ASSERT_EQ(node->Process(&empty), 0);
    ASSERT_NE(empty.Read<TextBatch>("output"), nullptr);
    EXPECT_TRUE(empty.Read<TextBatch>("output")->empty());
    EXPECT_EQ(model->calls, before);
  }
}

TEST_F(NodeAuthoringExamplesTest,
       GeneratedLlmNodeReadsGenerationOptionsFromConfig) {
  auto model = std::make_shared<test::PromptCaptureLlmModel>();
  ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(),
                                "prompt_contract", model, "v1"));
  // 未配置时使用默认生成参数；配置后原样传给模型。
  const std::vector<std::tuple<nlohmann::json, int, float>> cases = {
      {nlohmann::json{{"bind_model", "prompt_contract"}}, 128, 0.7f},
      {nlohmann::json{{"bind_model", "prompt_contract"},
                      {"max_tokens", 2048},
                      {"temperature", 0.0}},
       2048, 0.0f}};
  for (const auto& [config, max_tokens, temperature] : cases) {
    SCOPED_TRACE(config.dump());
    auto node = NodeRegistry::Instance().Create("scaffold_model_llm");
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(InitNodeForTest(*node, config, session_ctx_.get()));
    AlgContext ctx;
    ctx.Publish("input", TextBatch{{17, 4, "a"}});
    ASSERT_EQ(node->Process(&ctx), 0) << ctx.GetErrorMessage();
    EXPECT_EQ(model->last_options.max_tokens, max_tokens);
    EXPECT_FLOAT_EQ(model->last_options.temperature, temperature);
  }
}

namespace {
// 与 custom_node_concepts.md 中"生成参数加自有配置"的示例保持一致。
struct LlmWithOwnParams {
  GenerateOptions generation;
  std::string prefix;
};
}  // namespace

TEST_F(NodeAuthoringExamplesTest,
       GenerateParametersCombinesOwnFieldsWithGeneration) {
  auto params = Parameters<LlmWithOwnParams>{
      Field("prefix", &LlmWithOwnParams::prefix).Default("")};
  params.Include(&LlmWithOwnParams::generation, GenerateParameters());
  std::vector<std::string> names;
  for (const auto& field : params.Fields()) names.push_back(field.name);
  EXPECT_EQ(names,
            (std::vector<std::string>{
                "prefix", "system_prompt", "temperature", "max_tokens", "top_k",
                "top_p", "repetition_penalty", "stop_words", "random_seed"}));

  std::string error;
  auto defaults = params.Parse(nlohmann::json::object(), &error);
  ASSERT_TRUE(defaults.has_value()) << error;
  EXPECT_EQ(defaults->prefix, "");
  EXPECT_EQ(defaults->generation.max_tokens, 128);
  EXPECT_FLOAT_EQ(defaults->generation.temperature, 0.7f);
  EXPECT_EQ(defaults->generation.top_k, 0);
  EXPECT_FLOAT_EQ(defaults->generation.top_p, 0.9f);
  EXPECT_FLOAT_EQ(defaults->generation.repetition_penalty, 1.0f);
  EXPECT_TRUE(defaults->generation.stop_words.empty());

  const nlohmann::json raw = {
      {"prefix", "P:"},       {"temperature", 0.3},
      {"max_tokens", 64},     {"top_k", 7},
      {"top_p", 0.8},         {"repetition_penalty", 1.25},
      {"stop_words", {"END"}}};
  auto configured = params.Parse(raw, &error);
  ASSERT_TRUE(configured.has_value()) << error;
  EXPECT_EQ(configured->prefix, "P:");
  EXPECT_EQ(configured->generation.max_tokens, 64);
  EXPECT_FLOAT_EQ(configured->generation.temperature, 0.3f);
  EXPECT_EQ(configured->generation.top_k, 7);
  EXPECT_FLOAT_EQ(configured->generation.top_p, 0.8f);
  EXPECT_FLOAT_EQ(configured->generation.repetition_penalty, 1.25f);
  EXPECT_EQ(configured->generation.stop_words, std::vector<std::string>{"END"});

  for (const auto& config : {nlohmann::json::object(), raw}) {
    SCOPED_TRACE(config.dump());
    nlohmann::json normalized;
    ASSERT_TRUE(ValidateAndNormalizeFields(params.Fields(), config, &normalized,
                                           nullptr));
    const auto from_raw = params.Parse(config, &error);
    ASSERT_TRUE(from_raw.has_value()) << error;
    const auto from_normalized = params.Parse(normalized, &error);
    ASSERT_TRUE(from_normalized.has_value()) << error;
    EXPECT_EQ(from_normalized->prefix, from_raw->prefix);
    EXPECT_EQ(from_normalized->generation.max_tokens,
              from_raw->generation.max_tokens);
    EXPECT_FLOAT_EQ(from_normalized->generation.temperature,
                    from_raw->generation.temperature);
    EXPECT_EQ(from_normalized->generation.top_k, from_raw->generation.top_k);
    EXPECT_FLOAT_EQ(from_normalized->generation.top_p,
                    from_raw->generation.top_p);
    EXPECT_FLOAT_EQ(from_normalized->generation.repetition_penalty,
                    from_raw->generation.repetition_penalty);
    EXPECT_EQ(from_normalized->generation.stop_words,
              from_raw->generation.stop_words);
  }

  for (const auto& bad :
       std::vector<nlohmann::json>{{{"prefix", 7}},
                                   {{"max_tokens", 0}},
                                   {{"max_tokens", 32769}},
                                   {{"temperature", -0.01}},
                                   {{"temperature", 2.01}},
                                   {{"top_k", -1}},
                                   {{"top_p", 0}},
                                   {{"top_p", 1.01}},
                                   {{"repetition_penalty", 0}},
                                   {{"repetition_penalty", 100.01}},
                                   {{"stop_words", {42}}},
                                   {{"stop_words", "END"}},
                                   {{"stop_words", {""}}},
                                   {{"unknown_field", true}}}) {
    SCOPED_TRACE(bad.dump());
    EXPECT_FALSE(params.Parse(bad, &error).has_value());
    EXPECT_FALSE(error.empty());
  }
}

TEST_F(NodeAuthoringExamplesTest, CustomAndGeneratedNodesUseStrictNativePlans) {
  for (const auto& [fixture, boundary] :
       std::vector<std::pair<std::string, PipelineIoBoundary>>{
           {"entity_extract",
            MakeTestBoundary(
                {{"input.sentence_text", "TextBatch"}},
                {{"parse_entities.document", "StructuredDocumentBatch"}})},
           {"doc_qa",
            MakeTestBoundary({{"input.doc_text", "TextBatch"},
                              {"input.query_text", "TextBatch"}},
                             {{"generate_answer.output", "TextBatch"},
                              {"match_intent.matches", "RuleMatchBatch"},
                              {"chunk_docs.chunk_counts", "Int32Batch"}})}}) {
    auto plan = PipelineValidator::ValidateAndPlan(
        LoadCorePipelineFixture("demo/fixtures/mock/pipeline_" + fixture +
                                "_custom.json"),
        boundary);
    ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  }
  ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(), "entity_llm",
                                std::make_shared<test::TestBizLlmModel>(
                                    std::make_shared<test::TestCausalLmSession>(
                                        "llm.fixture", BatchPolicy{2, 2})),
                                "v1"));
  for (const char* name : {"scaffold_compute", "scaffold_model_llm"}) {
    auto doc = LoadCorePipelineFixture(
        "demo/fixtures/mock/pipeline_entity_extract_custom.json");
    doc["pipeline"][0]["type"] = name;
    doc["pipeline"][0]["params"] =
        std::string(name) == "scaffold_compute"
            ? nlohmann::json::object()
            : nlohmann::json{{"bind_model", "entity_llm"}};
    if (std::string(name) == "scaffold_compute") doc.erase("models");
    auto plan = PipelineValidator::ValidateAndPlan(
        doc, MakeTestBoundary(
                 {{"input.sentence_text", "TextBatch"}},
                 {{"parse_entities.document", "StructuredDocumentBatch"}}));
    ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
    auto node = NodeRegistry::Instance().Create(name);
    // 使用真实的原生计划，包括归一化后的配置和键。
    const auto& node_plan = plan.node_plans.at("generate_entities");
    ASSERT_TRUE(node->Init({&node_plan, session_ctx_.get()}));
    AlgContext ctx;
    ctx.Publish("input.sentence_text", TextBatch{{31, 7, "实体"}});
    ASSERT_EQ(node->Process(&ctx), 0);
    ASSERT_NE(ctx.Read<TextBatch>("generate_entities.output"), nullptr);
    EXPECT_EQ(ctx.Read<TextBatch>("generate_entities.output")->front().req_id,
              31U);
  }
}

TEST_F(NodeAuthoringExamplesTest,
       StarterTextFunctionsFollowTheDocumentedExercise) {
  auto model = std::make_shared<test::PromptCaptureLlmModel>();
  model->response_prefix.clear();
  model->response_suffix = "\n\n";
  ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(), "entity_llm",
                                model, "v1"));
  auto document = LoadCorePipelineFixture(
      "demo/fixtures/mock/pipeline_entity_extract_custom.json");
  document["pipeline"][0]["type"] = "scaffold_tutorial_llm";
  document["pipeline"][0]["params"] = {{"bind_model", "entity_llm"}};
  const auto plan = PipelineValidator::ValidateAndPlan(
      document, MakeTestBoundary(
                    {{"input.sentence_text", "TextBatch"}},
                    {{"parse_entities.document", "StructuredDocumentBatch"}}));
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  auto node = NodeRegistry::Instance().Create("scaffold_tutorial_llm");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init(
      {&plan.node_plans.at("generate_entities"), session_ctx_.get()}));

  // 乱序的请求 ID 和非零 sub_id 经过两个文本函数后必须保持不变。
  const TextBatch inputs{{51, 8, "张三"}, {19, 3, "李四"}};
  AlgContext ctx;
  ctx.Publish("input.sentence_text", inputs);
  ASSERT_EQ(node->Process(&ctx), 0);
  ASSERT_EQ(model->prompts.size(), inputs.size());
  const auto* output = ctx.Read<TextBatch>("generate_entities.output");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), inputs.size());
  const auto* unchanged = ctx.Read<TextBatch>("input.sentence_text");
  ASSERT_NE(unchanged, nullptr);
  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto expected = "实体抽取：\n" + inputs[i].data;
    EXPECT_EQ(model->prompts[i].data, expected);
    EXPECT_EQ((*output)[i].data,
              expected);  // 去掉了 Model 输出末尾的换行。
    EXPECT_EQ((*output)[i].req_id, inputs[i].req_id);
    EXPECT_EQ((*output)[i].sub_id, inputs[i].sub_id);
    EXPECT_EQ((*unchanged)[i].data, inputs[i].data);
  }
}

TEST_F(NodeAuthoringExamplesTest,
       GeneratedCapabilityTemplatesCompileBindAndExecute) {
  CheckScaffoldExecution<TextBatch, TextBatch>("scaffold_compute", "",
                                               session_ctx_.get());
  CheckScaffoldExecution<TextBatch, TextBatch>("scaffold_model_llm",
                                               "llm_model", session_ctx_.get());
  CheckScaffoldExecution<TextBatch, EmbeddingBatch>(
      "scaffold_model_embedding", "embed_model", session_ctx_.get());
  CheckScaffoldExecution<AudioPcmBatch, TextBatch>(
      "scaffold_model_asr", "asr_model", session_ctx_.get());
  CheckScaffoldExecution<QueryCandidatesBatch, ScoreBatch>(
      "scaffold_model_rerank", "rerank_model", session_ctx_.get());
  CheckScaffoldExecution<ImageRefBatch, OcrDocumentBatch>(
      "scaffold_model_ocr", "ocr_model", session_ctx_.get());
  auto node = NodeRegistry::Instance().Create("scaffold_conversion");
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("input", TextBatch{{1, 0, "unimplemented"}});
  EXPECT_NE(node->Process(&ctx), 0);
  EXPECT_FALSE(ctx.Has("output"));
}

}  // namespace llm_edgeflow
