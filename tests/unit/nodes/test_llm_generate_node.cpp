#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "contracts/diagnostic.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "engine/model_interface.h"
#include "nodes/generate_parameters.h"
#include "nodes/model_calls.h"
#include "nodes/node_base.h"
#include "nodes/node_error_codes.h"
#include "tests/support/model_registration.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/pipeline_test_utils.h"

namespace llm_edgeflow {

namespace {

class ContractLlmModel final : public ILlmModel {
 public:
  const std::string& ImplName() const noexcept override {
    static const std::string type = "contract_llm";
    return type;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string model_type = "llm";
    return model_type;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }

  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs,
               std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
    if (!outputs) return -1;
    if (fail_with_input_reason) {
      outputs->clear();
      SetDiagnosticNoexcept(diagnostic, prompts.empty() ? "backend failure"
                                                        : prompts.front().data);
      return -77;
    }
    ++infer_calls;
    prompt_batches.push_back(prompts);
    call_options.push_back(options);
    if (fail_on_call == infer_calls) {
      outputs->clear();
      SetDiagnosticNoexcept(diagnostic, "endpoint backend unavailable");
      return -77;
    }
    last_options = options;
    outputs->clear();
    for (const auto& prompt : prompts) {
      outputs->emplace_back(prompt.req_id, prompt.sub_id,
                            "generated:" + prompt.data);
    }
    if (return_wrong_count &&
        (fault_on_call == 0 || fault_on_call == infer_calls) &&
        !outputs->empty()) {
      outputs->pop_back();
    }
    if (corrupt_provenance &&
        (fault_on_call == 0 || fault_on_call == infer_calls) &&
        outputs->size() > 1) {
      ++(*outputs)[1].sub_id;
    }
    return 0;
  }

  int infer_calls = 0;
  GenerateOptions last_options;
  std::vector<TextBatch> prompt_batches;
  std::vector<GenerateOptions> call_options;
  int fail_on_call = 0;
  int fault_on_call = 0;
  bool return_wrong_count = false;
  bool corrupt_provenance = false;
  bool fail_with_input_reason = false;
};

}  // namespace

class LlmGenerateNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    session_ctx_ = std::make_unique<SessionContext>();

    model_ = std::make_shared<ContractLlmModel>();
    ASSERT_TRUE(RegisterTestModel(session_ctx_->GetModelManager(), "llm_model",
                                  model_, "test-v1"));
  }
  std::unique_ptr<SessionContext> session_ctx_;
  std::shared_ptr<ContractLlmModel> model_;
};

TEST_F(LlmGenerateNodeTest, ModelReasonReachesRequestErrorWithoutOutput) {
  model_->fail_with_input_reason = true;
  auto node = NodeRegistry::Instance().Create("llm_generate");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node,
                      {{"bind_model", "llm_model"},
                       {"endpoints", {{"answer", nlohmann::json::object()}}}},
                      session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("input", TextBatch{{0, 0, "backend context capacity exceeded"}});
  EXPECT_EQ(node->Process(&ctx), -77);
  EXPECT_NE(ctx.GetErrorMessage().find("backend context capacity exceeded"),
            std::string::npos);
  EXPECT_EQ(ctx.Read<TextBatch>("text"), nullptr);
  EXPECT_EQ(ctx.Read<StructuredDocumentBatch>("document"), nullptr);
}

TEST_F(LlmGenerateNodeTest, ConcurrentModelCallsKeepIndependentReasons) {
  model_->fail_with_input_reason = true;
  LlmCall call(model_);
  auto first = std::async(std::launch::async, [&] {
    return call.Generate(TextBatch{{0, 0, "backend failure A"}});
  });
  auto second = std::async(std::launch::async, [&] {
    return call.Generate(TextBatch{{1, 0, "backend failure B"}});
  });
  auto first_result = first.get();
  auto second_result = second.get();
  ASSERT_FALSE(first_result.ok());
  ASSERT_FALSE(second_result.ok());
  EXPECT_NE(first_result.failure().message.find("backend failure A"),
            std::string::npos);
  EXPECT_EQ(first_result.failure().message.find("backend failure B"),
            std::string::npos);
  EXPECT_NE(second_result.failure().message.find("backend failure B"),
            std::string::npos);
  EXPECT_EQ(second_result.failure().message.find("backend failure A"),
            std::string::npos);
}

TEST_F(LlmGenerateNodeTest, ProcessBatchPromptInference) {
  auto node = NodeRegistry::Instance().Create("llm_generate");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "llm_model"},
                        {"endpoints", {{"answer", nlohmann::json::object()}}},
                        {"temperature", 0.7},
                        {"max_tokens", 128},
                        {"top_k", 32},
                        {"top_p", 0.8},
                        {"repetition_penalty", 1.15},
                        {"stop_words", {"END"}}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch prompts;
  prompts.emplace_back(1, 0, "Explain quantum physics");
  prompts.emplace_back(2, 0, "Summarize article");
  ctx.Publish("input", prompts);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_FALSE((*out)[0].data.empty());
  EXPECT_FALSE((*out)[1].data.empty());
  EXPECT_EQ((*out)[0].req_id, 1U);
  EXPECT_EQ((*out)[1].req_id, 2U);
  EXPECT_FLOAT_EQ(model_->last_options.temperature, 0.7f);
  EXPECT_EQ(model_->last_options.max_tokens, 128);
  EXPECT_EQ(model_->last_options.top_k, 32);
  EXPECT_FLOAT_EQ(model_->last_options.top_p, 0.8f);
  EXPECT_FLOAT_EQ(model_->last_options.repetition_penalty, 1.15f);
  EXPECT_EQ(model_->last_options.stop_words, std::vector<std::string>({"END"}));
}

TEST_F(LlmGenerateNodeTest,
       RawAndNormalizedGenerationOptionsReachModelEqually) {
  const auto schema = GenerateParameters();
  const auto definition = PipelineCatalog::FindNode("llm_generate");
  ASSERT_TRUE(definition.has_value());
  const nlohmann::json override_options = {{"temperature", 0.25},
                                           {"max_tokens", 47},
                                           {"top_k", 17},
                                           {"top_p", 0.6},
                                           {"repetition_penalty", 1.3},
                                           {"stop_words", {"END", "STOP"}}};
  for (bool overridden : {false, true}) {
    SCOPED_TRACE(overridden);
    const auto raw_options =
        overridden ? override_options : nlohmann::json::object();
    std::string diagnostic;
    const auto expected = schema.Parse(raw_options, &diagnostic);
    ASSERT_TRUE(expected.has_value()) << diagnostic;
    EXPECT_EQ(expected->max_tokens, overridden ? 47 : 128);
    nlohmann::json raw = raw_options;
    raw["bind_model"] = "llm_model";
    raw["endpoints"] = {{"answer", nlohmann::json::object()}};
    nlohmann::json normalized;
    ASSERT_TRUE(ValidateAndNormalizeFields(definition->config_fields, raw,
                                           &normalized, nullptr));
    const TextBatch prompts{
        {31, 7, "first"}, {19, 3, "second"}, {31, 9, "third"}};
    for (const auto& config : {raw, normalized}) {
      SCOPED_TRACE(config.dump());
      auto node = NodeRegistry::Instance().Create("llm_generate");
      ASSERT_NE(node, nullptr);
      ASSERT_TRUE(
          InitNodeForTest(*node, config, session_ctx_.get(), &diagnostic))
          << diagnostic;
      const int before = model_->infer_calls;
      AlgContext context;
      context.Publish("input", prompts);
      ASSERT_EQ(node->Process(&context), 0) << context.GetErrorMessage();
      EXPECT_EQ(model_->infer_calls, before + 1);
      EXPECT_FLOAT_EQ(model_->last_options.temperature, expected->temperature);
      EXPECT_EQ(model_->last_options.max_tokens, expected->max_tokens);
      EXPECT_EQ(model_->last_options.top_k, expected->top_k);
      EXPECT_FLOAT_EQ(model_->last_options.top_p, expected->top_p);
      EXPECT_FLOAT_EQ(model_->last_options.repetition_penalty,
                      expected->repetition_penalty);
      EXPECT_EQ(model_->last_options.stop_words, expected->stop_words);
      const auto* output = context.Read<TextBatch>("text");
      ASSERT_NE(output, nullptr);
      ASSERT_EQ(output->size(), prompts.size());
      for (size_t i = 0; i < prompts.size(); ++i) {
        EXPECT_EQ((*output)[i].req_id, prompts[i].req_id);
        EXPECT_EQ((*output)[i].sub_id, prompts[i].sub_id);
        EXPECT_EQ((*output)[i].data, "generated:" + prompts[i].data);
      }
    }
  }
}

TEST_F(LlmGenerateNodeTest, GenerationOptionsAcceptInclusiveBounds) {
  const auto schema = GenerateParameters();
  const std::vector<nlohmann::json> valid = {
      {{"temperature", 0},
       {"max_tokens", 1},
       {"top_k", 0},
       {"top_p", 1.0e-9},
       {"repetition_penalty", 1.0e-9},
       {"stop_words", nlohmann::json::array()}},
      {{"temperature", 2},
       {"max_tokens", 32768},
       {"top_k", std::numeric_limits<int32_t>::max()},
       {"top_p", 1},
       {"repetition_penalty", 100},
       {"stop_words", {"END"}}}};
  for (auto config : valid) {
    SCOPED_TRACE(config.dump());
    std::string diagnostic;
    ASSERT_TRUE(schema.Parse(config, &diagnostic).has_value()) << diagnostic;
    config["bind_model"] = "llm_model";
    config["endpoints"] = {{"answer", nlohmann::json::object()}};
    auto node = NodeRegistry::Instance().Create("llm_generate");
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(InitNodeForTest(*node, config, session_ctx_.get(), &diagnostic))
        << diagnostic;
    AlgContext context;
    context.Publish("input", TextBatch{{71, 4, "boundary"}});
    ASSERT_EQ(node->Process(&context), 0) << context.GetErrorMessage();
    ASSERT_NE(context.Read<TextBatch>("text"), nullptr);
    EXPECT_EQ(context.Read<TextBatch>("text")->front().req_id, 71U);
    EXPECT_EQ(context.Read<TextBatch>("text")->front().sub_id, 4U);
  }
}

TEST_F(LlmGenerateNodeTest, ValidatorRejectsInvalidOptions) {
  const std::vector<nlohmann::json> invalid = {
      {{"stop_words", nlohmann::json::array({42})}},
      {{"stop_words", nlohmann::json::array({""})}},
      {{"stop_words", "END"}},
      {{"max_tokens", 32769}},
      {{"max_tokens", 0}},
      {{"max_tokens", uint64_t{1} << 32}},
      {{"top_k", -1}},
      {{"top_k", uint64_t{1} << 32}},
      {{"temperature", -0.01}},
      {{"temperature", 2.01}},
      {{"top_p", 1.0e-10}},
      {{"top_p", 1.01}},
      {{"repetition_penalty", 0.0}},
      {{"repetition_penalty", 100.01}}};
  // 检查 Node 的预检诊断无需构造 Model。
  for (auto config : invalid) {
    SCOPED_TRACE(config.dump());
    config["bind_model"] = "llm_model";
    config["endpoints"] = {{"answer", nlohmann::json::object()}};
    const nlohmann::json pipeline = {
        {"pipeline", nlohmann::json::array({{{"name", "generate"},
                                             {"type", "llm_generate"},
                                             {"params", config}}})}};
    const auto plan =
        PipelineValidator::ValidateAndPlan(pipeline, MakeTestBoundary());
    EXPECT_FALSE(plan.report.ok);
    bool config_rejected = false;
    for (const auto& diagnostic : plan.report.diagnostics) {
      if (diagnostic.path.rfind("/pipeline/0/params", 0) == 0 &&
          diagnostic.code != DiagnosticCode::kUnknownModelReference)
        config_rejected = true;
    }
    EXPECT_TRUE(config_rejected);
  }
  std::string diagnostic;
  EXPECT_NE(PrepareNodePlanForTest(
                "llm_generate",
                {{"bind_model", "llm_model"},
                 {"endpoints", {{"answer", nlohmann::json::object()}}},
                 {"stop_words", nlohmann::json::array({"END"})},
                 {"max_tokens", 32768},
                 {"top_p", 1.0e-9}},
                {}, "", "", &diagnostic),
            nullptr)
      << diagnostic;
}

TEST_F(LlmGenerateNodeTest, StopWordElementTypeErrorReportsExactConfigPath) {
  const nlohmann::json options = {{"stop_words", {"END", 42}}};
  auto config = options;
  config["bind_model"] = "llm_model";
  config["endpoints"] = {{"answer", nlohmann::json::object()}};
  const nlohmann::json pipeline = {
      {"pipeline", nlohmann::json::array({{{"name", "generate"},
                                           {"type", "llm_generate"},
                                           {"params", config}}})}};
  const auto plan =
      PipelineValidator::ValidateAndPlan(pipeline, MakeTestBoundary());
  bool exact_diagnostic = false;
  for (const auto& error : plan.report.diagnostics) {
    if (error.path == "/pipeline/0/params/stop_words/1" &&
        error.code == DiagnosticCode::kConfigFieldType) {
      exact_diagnostic = true;
    }
  }
  EXPECT_TRUE(exact_diagnostic) << plan.report.ToJson().dump(2);
  EXPECT_EQ(model_->infer_calls, 0);
}

TEST_F(LlmGenerateNodeTest, MissingInputFailsClosed) {
  auto node = NodeRegistry::Instance().Create("llm_generate");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node,
                      {{"bind_model", "llm_model"},
                       {"endpoints", {{"answer", nlohmann::json::object()}}}},
                      session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), node_error::author_node::kMissingInput);
}

TEST_F(LlmGenerateNodeTest, EmptyBatchSkipsInference) {
  auto node = NodeRegistry::Instance().Create("llm_generate");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node,
                      {{"bind_model", "llm_model"},
                       {"endpoints", {{"answer", nlohmann::json::object()}}}},
                      session_ctx_.get()));

  AlgContext ctx;
  ctx.Publish("input", TextBatch{});
  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<TextBatch>("text");
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->empty());
  const auto* document = ctx.Read<StructuredDocumentBatch>("document");
  ASSERT_NE(document, nullptr);
  EXPECT_TRUE(document->empty());
  EXPECT_EQ(model_->infer_calls, 0);
}

TEST_F(LlmGenerateNodeTest, InvalidModelOutputFailsClosed) {
  auto node = NodeRegistry::Instance().Create("llm_generate");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node,
                      {{"bind_model", "llm_model"},
                       {"endpoints", {{"answer", nlohmann::json::object()}}}},
                      session_ctx_.get()));

  TextBatch prompts = {{3, 0, "first"}, {3, 1, "second"}};

  model_->return_wrong_count = true;
  AlgContext count_ctx;
  count_ctx.Publish("input", prompts);
  EXPECT_EQ(node->Process(&count_ctx),
            node_error::author_node::kOutputCountMismatch);
  EXPECT_EQ(count_ctx.Read<TextBatch>("text"), nullptr);
  EXPECT_EQ(count_ctx.Read<StructuredDocumentBatch>("document"), nullptr);

  model_->return_wrong_count = false;
  model_->corrupt_provenance = true;
  AlgContext provenance_ctx;
  provenance_ctx.Publish("input", prompts);
  EXPECT_EQ(node->Process(&provenance_ctx),
            node_error::author_node::kOutputProvenanceMismatch);
  EXPECT_EQ(provenance_ctx.Read<TextBatch>("text"), nullptr);
  EXPECT_EQ(provenance_ctx.Read<StructuredDocumentBatch>("document"), nullptr);
}

TEST_F(LlmGenerateNodeTest, SingleEndpointPublishesRawAnswerAndNamedDocument) {
  auto node = NodeRegistry::Instance().Create("llm_generate");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node,
                      {{"bind_model", "llm_model"},
                       {"endpoints", {{"answer", nlohmann::json::object()}}}},
                      session_ctx_.get()));
  const TextBatch inputs{{31, 7, "first\n\"quoted\""},
                         {19, 3, "{\"opaque\":true}"},
                         {31, 9, "literal {{input}}"}};
  AlgContext context;
  context.Publish("input", inputs);
  ASSERT_EQ(node->Process(&context), 0) << context.GetErrorMessage();
  ASSERT_EQ(model_->prompt_batches.size(), 1U);
  const auto* text = context.Read<TextBatch>("text");
  const auto* document = context.Read<StructuredDocumentBatch>("document");
  ASSERT_NE(text, nullptr);
  ASSERT_NE(document, nullptr);
  ASSERT_EQ(text->size(), inputs.size());
  ASSERT_EQ(document->size(), inputs.size());
  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto answer = "generated:" + inputs[i].data;
    EXPECT_EQ((*text)[i].data, answer);
    EXPECT_EQ((*text)[i].req_id, inputs[i].req_id);
    EXPECT_EQ((*text)[i].sub_id, inputs[i].sub_id);
    EXPECT_EQ(model_->prompt_batches[0][i].data, inputs[i].data);
    EXPECT_EQ((*document)[i].req_id, inputs[i].req_id);
    EXPECT_EQ((*document)[i].sub_id, inputs[i].sub_id);
    EXPECT_TRUE((*document)[i].data.is_valid);
    const nlohmann::json expected = {{"answer", answer}};
    EXPECT_EQ((*document)[i].data.structured_data, expected);
    EXPECT_EQ(nlohmann::json::parse((*document)[i].data.json_payload),
              expected);
  }
}

TEST_F(LlmGenerateNodeTest, MultipleEndpointsCallWholeBatchesInNameOrder) {
  auto node = NodeRegistry::Instance().Create("llm_generate");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(
      *node,
      {{"bind_model", "llm_model"},
       {"temperature", 0.25},
       {"max_tokens", 47},
       {"system_prompt", "shared system"},
       {"endpoints",
        {{"z_last", {{"prompt", "Z:{{input}}"}}},
         {"a_first", {{"prompt", "A:{{ input }}|{{input}}"}}}}}},
      session_ctx_.get()));
  for (const auto& inputs :
       {TextBatch{{31, 7, "first"}, {19, 3, "second"}, {31, 9, "third"}},
        TextBatch{{54, 8, "next {{input}}"}}}) {
    const size_t before = model_->prompt_batches.size();
    AlgContext context;
    context.Publish("input", inputs);
    ASSERT_EQ(node->Process(&context), 0) << context.GetErrorMessage();
    ASSERT_EQ(model_->prompt_batches.size(), before + 2);
    ASSERT_EQ(model_->prompt_batches[before].size(), inputs.size());
    ASSERT_EQ(model_->prompt_batches[before + 1].size(), inputs.size());
    for (size_t call = before; call < before + 2; ++call) {
      EXPECT_FLOAT_EQ(model_->call_options[call].temperature, 0.25f);
      EXPECT_EQ(model_->call_options[call].max_tokens, 47);
      EXPECT_EQ(model_->call_options[call].system_prompt, "shared system");
    }
    const auto* text = context.Read<TextBatch>("text");
    const auto* document = context.Read<StructuredDocumentBatch>("document");
    ASSERT_NE(text, nullptr);
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(text->size(), inputs.size());
    ASSERT_EQ(document->size(), inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
      const auto first = "A:" + inputs[i].data + "|" + inputs[i].data;
      const auto last = "Z:" + inputs[i].data;
      EXPECT_EQ(model_->prompt_batches[before][i].data, first);
      EXPECT_EQ(model_->prompt_batches[before + 1][i].data, last);
      for (size_t call = before; call < before + 2; ++call) {
        EXPECT_EQ(model_->prompt_batches[call][i].req_id, inputs[i].req_id);
        EXPECT_EQ(model_->prompt_batches[call][i].sub_id, inputs[i].sub_id);
      }
      const nlohmann::json expected = {{"a_first", "generated:" + first},
                                       {"z_last", "generated:" + last}};
      EXPECT_EQ((*document)[i].data.structured_data, expected);
      EXPECT_EQ((*document)[i].data.json_payload, expected.dump());
      EXPECT_EQ((*text)[i].data, expected.dump());
      EXPECT_EQ((*text)[i].req_id, inputs[i].req_id);
      EXPECT_EQ((*text)[i].sub_id, inputs[i].sub_id);
      EXPECT_EQ((*document)[i].req_id, inputs[i].req_id);
      EXPECT_EQ((*document)[i].sub_id, inputs[i].sub_id);
    }
  }
}

TEST_F(LlmGenerateNodeTest,
       EndpointMapAndTemplatesAreValidatedBeforeInference) {
  const std::vector<nlohmann::json> invalid = {
      nlohmann::json::object(),
      {{"endpoints", nlohmann::json::object()}},
      {{"endpoints", nlohmann::json::array()}},
      {{"endpoints", {{"answer", 42}}}},
      {{"endpoints", {{"answer", {{"prompt", 42}}}}}},
      {{"endpoints", {{"answer", {{"prompt", "{{unknown}}"}}}}}},
      {{"endpoints", {{"answer", {{"prompt", "{{input"}}}}}}};
  for (auto params : invalid) {
    SCOPED_TRACE(params.dump());
    params["bind_model"] = "llm_model";
    auto node = NodeRegistry::Instance().Create("llm_generate");
    ASSERT_NE(node, nullptr);
    std::string diagnostic;
    EXPECT_FALSE(
        InitNodeForTest(*node, params, session_ctx_.get(), &diagnostic));
    EXPECT_FALSE(diagnostic.empty());
  }
  EXPECT_EQ(model_->infer_calls, 0);
}

TEST_F(LlmGenerateNodeTest, LaterEndpointFailuresPublishNeitherOutput) {
  for (int fault = 0; fault < 3; ++fault) {
    SCOPED_TRACE(fault);
    model_ = std::make_shared<ContractLlmModel>();
    SessionContext session;
    ASSERT_TRUE(RegisterTestModel(session.GetModelManager(), "llm_model",
                                  model_, "v1"));
    model_->fail_on_call = fault == 0 ? 2 : 0;
    model_->fault_on_call = 2;
    model_->return_wrong_count = fault == 1;
    model_->corrupt_provenance = fault == 2;
    auto node = NodeRegistry::Instance().Create("llm_generate");
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(
        InitNodeForTest(*node,
                        {{"bind_model", "llm_model"},
                         {"endpoints",
                          {{"a_success", nlohmann::json::object()},
                           {"b_failure", nlohmann::json::object()},
                           {"c_unreachable", nlohmann::json::object()}}}},
                        &session));
    AlgContext context;
    context.Publish("input", TextBatch{{43, 7, "first"}, {19, 8, "second"}});
    EXPECT_EQ(node->Process(&context),
              fault == 0 ? -77
              : fault == 1
                  ? node_error::author_node::kOutputCountMismatch
                  : node_error::author_node::kOutputProvenanceMismatch);
    EXPECT_EQ(model_->infer_calls, 2);
    EXPECT_NE(context.GetErrorMessage().find("b_failure"), std::string::npos);
    EXPECT_FALSE(context.Has("text"));
    EXPECT_FALSE(context.Has("document"));
  }
}

TEST_F(LlmGenerateNodeTest, UnreferencedOutputsMayBeOmittedFromRuntimePlan) {
  for (const char* referenced : {"text", "document", ""}) {
    SCOPED_TRACE(referenced);
    std::string diagnostic;
    auto plan = PrepareNodePlanForTest(
        "llm_generate",
        {{"bind_model", "llm_model"},
         {"endpoints", {{"answer", nlohmann::json::object()}}}},
        {}, "", "", &diagnostic);
    ASSERT_NE(plan, nullptr) << diagnostic;
    plan->ports.erase(std::remove_if(plan->ports.begin(), plan->ports.end(),
                                     [&](const auto& port) {
                                       return port.direction ==
                                                  PortDirection::kOutput &&
                                              port.logical_name != referenced;
                                     }),
                      plan->ports.end());
    auto node = NodeRegistry::Instance().Create("llm_generate");
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->Init({plan.get(), session_ctx_.get(), &diagnostic}))
        << diagnostic;
    AlgContext context;
    context.Publish("input", TextBatch{{43, 7, "payload"}});
    ASSERT_EQ(node->Process(&context), 0) << context.GetErrorMessage();
    EXPECT_EQ(context.Has("text"), std::string(referenced) == "text");
    EXPECT_EQ(context.Has("document"), std::string(referenced) == "document");
    EXPECT_FALSE(context.Has(""));
    if (std::string(referenced) == "text") {
      EXPECT_EQ(context.Read<TextBatch>("text")->front().data,
                "generated:payload");
    }
    if (std::string(referenced) == "document") {
      EXPECT_EQ(context.Read<StructuredDocumentBatch>("document")
                    ->front()
                    .data.structured_data,
                nlohmann::json({{"answer", "generated:payload"}}));
    }
  }
}

TEST_F(LlmGenerateNodeTest,
       SingleEndpointPreservesRawBytesWhenDocumentCannotEncodeUtf8) {
  auto node = NodeRegistry::Instance().Create("llm_generate");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node,
                      {{"bind_model", "llm_model"},
                       {"endpoints", {{"answer", nlohmann::json::object()}}}},
                      session_ctx_.get()));
  const std::string invalid = std::string("opaque ") + static_cast<char>(0xff);
  const TextBatch inputs{{43, 7, invalid}, {19, 8, "valid"}};
  AlgContext context;
  context.Publish("input", inputs);
  ASSERT_EQ(node->Process(&context), 0) << context.GetErrorMessage();
  EXPECT_EQ(model_->infer_calls, 1);
  const auto* text = context.Read<TextBatch>("text");
  const auto* document = context.Read<StructuredDocumentBatch>("document");
  ASSERT_NE(text, nullptr);
  ASSERT_NE(document, nullptr);
  ASSERT_EQ(text->size(), inputs.size());
  ASSERT_EQ(document->size(), inputs.size());
  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto answer = "generated:" + inputs[i].data;
    EXPECT_EQ((*text)[i].data, answer);
    EXPECT_EQ((*text)[i].req_id, inputs[i].req_id);
    EXPECT_EQ((*text)[i].sub_id, inputs[i].sub_id);
    EXPECT_EQ((*document)[i].req_id, inputs[i].req_id);
    EXPECT_EQ((*document)[i].sub_id, inputs[i].sub_id);
    EXPECT_EQ(
        (*document)[i].data.structured_data.at("answer").get<std::string>(),
        answer);
  }
  const auto& failed = document->front().data;
  EXPECT_FALSE(failed.is_valid);
  EXPECT_EQ(failed.parse_status, JsonParseStatus::kFailed);
  EXPECT_TRUE(failed.json_payload.empty());
  EXPECT_NE(failed.diagnostic.find("UTF-8"), std::string::npos);
  const auto& valid = document->back().data;
  EXPECT_TRUE(valid.is_valid);
  EXPECT_EQ(valid.parse_status, JsonParseStatus::kOk);
  EXPECT_TRUE(valid.diagnostic.empty());
  EXPECT_EQ(nlohmann::json::parse(valid.json_payload),
            nlohmann::json({{"answer", "generated:valid"}}));
}

TEST_F(LlmGenerateNodeTest,
       MultipleEndpointsRejectInvalidUtf8WithoutPublishingEitherOutput) {
  auto node = NodeRegistry::Instance().Create("llm_generate");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node,
                              {{"bind_model", "llm_model"},
                               {"endpoints",
                                {{"first", nlohmann::json::object()},
                                 {"second", nlohmann::json::object()}}}},
                              session_ctx_.get()));
  AlgContext context;
  context.Publish("input",
                  TextBatch{{43, 7, std::string(1, static_cast<char>(0xff))}});
  EXPECT_EQ(node->Process(&context),
            static_cast<int>(NodeRuntimeCode::kUnhandledException));
  EXPECT_EQ(model_->infer_calls, 2);
  EXPECT_NE(context.GetErrorMessage().find("UTF-8"), std::string::npos);
  EXPECT_FALSE(context.Has("text"));
  EXPECT_FALSE(context.Has("document"));
}

}  // namespace llm_edgeflow
