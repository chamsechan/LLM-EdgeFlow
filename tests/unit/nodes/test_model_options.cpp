#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/models/generated_text_embedding/generated_text_embedding_model.h"
#include "nodes/authoring.h"
#include "tests/support/node_harness.h"

namespace llm_edgeflow {
namespace {

class OptionsLlm final : public ILlmModel {
 public:
  explicit OptionsLlm(bool supports_seed = false)
      : supports_seed_(supports_seed) {}

  const std::string& ImplName() const noexcept override {
    static const std::string name = "test_model_options_llm";
    return name;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string type = "llm";
    return type;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  bool SupportsRandomSeed() const noexcept override {
    ++seed_queries;
    return supports_seed_;
  }
  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs,
               std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
    if (!outputs) return -1;
    calls.push_back(options);
    outputs->clear();
    for (const auto& item : prompts) {
      outputs->emplace_back(item.req_id, item.sub_id,
                            options.system_prompt + "|" +
                                std::to_string(options.random_seed) + "|" +
                                item.data);
    }
    return 0;
  }

  mutable int seed_queries = 0;
  std::vector<GenerateOptions> calls;

 private:
  bool supports_seed_;
};

class OptionsAsr final : public IAsrModel {
 public:
  const std::string& ImplName() const noexcept override {
    static const std::string name = "test_model_options_asr";
    return name;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string type = "asr";
    return type;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  bool SupportsLanguage(std::string_view language) const noexcept override {
    ++language_queries;
    return language == "en";
  }
  int Transcribe(const AudioPcmBatch& audio, const TranscribeOptions& options,
                 TextBatch* outputs,
                 std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
    if (!outputs) return -1;
    languages.push_back(options.language);
    outputs->clear();
    for (const auto& item : audio)
      outputs->emplace_back(item.req_id, item.sub_id,
                            options.language + ":transcript");
    return 0;
  }

  mutable int language_queries = 0;
  std::vector<std::string> languages;
};

class FixedVectorSession final : public IGeneratedTokenEmbeddingSession {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string type = "test_model_options_embedding";
    return type;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kGeneratedTokenEmbedding;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return {1, 0}; }
  int GenerateEmbeddings(const std::string&, bool, int,
                         GeneratedTokenEmbeddings* output,
                         std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
    if (!output) return -1;
    ++calls;
    *output = GeneratedTokenEmbeddings{{7}, {{3.0f, 4.0f}}};
    return 0;
  }

  int calls = 0;
};

struct ValidationInputs {
  const TextBatch* text = nullptr;
};
struct ValidationParams {
  int count = 0;
  std::string derived;
};
struct ValidationModels {
  LlmCall generator;
};
struct ValidationObservation {
  int validations = 0;
  int runs = 0;
  int count = 0;
  std::string derived;
  std::string model_name;
  bool bound = false;
} validation;

auto ValidationSpec() {
  auto params = Parameters<ValidationParams>{
      Field("count", &ValidationParams::count).Default(2)};
  params.Prepare([](ValidationParams* params, std::string*) {
    params->count *= 2;
    params->derived = "prepared";
    return true;
  });
  return MakeNodeSpec(
             InputsOf<ValidationInputs>{
                 Required("text", &ValidationInputs::text)},
             PreservedOutput<TextBatch>("output", "text"), std::move(params),
             ModelsOf<ValidationModels>{Model("generator", "bind_model",
                                              &ValidationModels::generator)},
             [](const ValidationInputs& inputs, const ValidationParams& params,
                const ValidationModels& models) {
               ++validation.runs;
               auto prompts = MapPayloads(*inputs.text, [&](const auto& text) {
                 return std::to_string(params.count) + ":" + text;
               });
               return models.generator.Generate(prompts);
             })
      .ValidateModels([](const ValidationParams& params,
                         const ValidationModels& models, std::string* error) {
        ++validation.validations;
        validation.count = params.count;
        validation.derived = params.derived;
        validation.model_name = models.generator.ModelName();
        validation.bound = models.generator.IsBound();
        if (models.generator.SupportsRandomSeed()) return true;
        if (error) *error = "probe refuses configured model";
        return false;
      })
      .WithControls({ReplaceFields(39001, "replace_count", {"count"})});
}

REGISTER_FUNCTION_NODE(model_validation_options_probe, ValidationSpec());

}  // namespace

TEST(FunctionNodeTest, ValidateModelsReceivesPreparedParamsAndRunsOnlyAtInit) {
  validation = {};
  auto model = std::make_shared<OptionsLlm>(true);
  NodeHarness harness("model_validation_options_probe");
  harness.Config({{"bind_model", "actual_model"}, {"count", 2}})
      .BindModel("actual_model", model)
      .TextInput("text", {"payload"});
  ASSERT_TRUE(harness.EnsureInitialized());
  EXPECT_EQ(validation.validations, 1);
  EXPECT_EQ(validation.count, 4);
  EXPECT_EQ(validation.derived, "prepared");
  EXPECT_EQ(validation.model_name, "actual_model");
  EXPECT_TRUE(validation.bound);
  EXPECT_EQ(model->seed_queries, 1);
  for (int request = 0; request < 2; ++request) {
    auto result = harness.Run();
    ASSERT_TRUE(result.ok()) << result.diagnostic();
    EXPECT_EQ(result.TextValues("output"),
              (std::vector<std::string>{"|-1|4:payload"}));
  }
  const auto update = harness.Control(39001, nlohmann::json{{"count", 3}});
  ASSERT_EQ(update.status, NodeControlStatus::kHandled) << update.message;
  auto updated = harness.Run();
  ASSERT_TRUE(updated.ok()) << updated.diagnostic();
  EXPECT_EQ(updated.TextValues("output"),
            (std::vector<std::string>{"|-1|6:payload"}));
  EXPECT_EQ(validation.runs, 3);
  EXPECT_EQ(validation.validations, 1);
  EXPECT_EQ(model->seed_queries, 1);

  auto refused_model = std::make_shared<OptionsLlm>();
  NodeHarness refused("model_validation_options_probe");
  refused.Config({{"bind_model", "refused_model"}, {"count", 7}})
      .BindModel("refused_model", refused_model)
      .TextInput("text", {"must not run"});
  auto failure = refused.Run();
  EXPECT_FALSE(failure.ok());
  EXPECT_TRUE(failure.init_failed());
  EXPECT_NE(failure.diagnostic().find("probe refuses configured model"),
            std::string::npos);
  EXPECT_EQ(validation.validations, 2);
  EXPECT_EQ(validation.count, 14);
  EXPECT_EQ(validation.model_name, "refused_model");
  EXPECT_EQ(validation.runs, 3);
  EXPECT_EQ(refused_model->seed_queries, 1);
  EXPECT_TRUE(refused_model->calls.empty());
  EXPECT_EQ(failure.Output<TextBatch>("output"), nullptr);
}

TEST(FunctionNodeTest,
     ActualLlmNodesValidateExplicitSeedsOnceAtInitialization) {
  for (const char* type : {"llm_generate", "prompt_guided_llm"}) {
    SCOPED_TRACE(type);
    const char* input = "input";
    nlohmann::json config = {{"bind_model", "llm"}};
    if (std::string(type) == "llm_generate")
      config["endpoints"] = {{"answer", nlohmann::json::object()}};
    auto unsupported = std::make_shared<OptionsLlm>();
    NodeHarness defaults(type);
    defaults.Config(config)
        .BindModel("llm", unsupported)
        .TextInput(input, {"payload"});
    auto default_result = defaults.Run();
    ASSERT_TRUE(default_result.ok()) << default_result.diagnostic();
    ASSERT_EQ(unsupported->calls.size(), 1U);
    EXPECT_EQ(unsupported->calls.front().random_seed, -1);
    EXPECT_EQ(unsupported->seed_queries, 0);

    NodeHarness refused(type);
    config["random_seed"] = 0;
    refused.Config(config)
        .BindModel("llm", unsupported)
        .TextInput(input, {"must not generate"});
    auto refusal = refused.Run();
    EXPECT_FALSE(refusal.ok());
    EXPECT_TRUE(refusal.init_failed());
    EXPECT_NE(refusal.diagnostic().find("does not support random_seed"),
              std::string::npos);
    EXPECT_EQ(unsupported->seed_queries, 1);
    EXPECT_EQ(unsupported->calls.size(), 1U);

    auto supported = std::make_shared<OptionsLlm>(true);
    NodeHarness seeded(type);
    config["random_seed"] = 17;
    seeded.Config(config)
        .BindModel("llm", supported)
        .TextInput(input, {"payload"});
    ASSERT_TRUE(seeded.EnsureInitialized());
    EXPECT_EQ(supported->seed_queries, 1);
    for (int request = 0; request < 2; ++request) {
      auto result = seeded.Run();
      ASSERT_TRUE(result.ok()) << result.diagnostic();
    }
    ASSERT_EQ(supported->calls.size(), 2U);
    EXPECT_EQ(supported->calls[0].random_seed, 17);
    EXPECT_EQ(supported->calls[1].random_seed, 17);
    EXPECT_EQ(supported->seed_queries, 1);
  }
}

TEST(FunctionNodeTest, ActualLlmNodesKeepCallOptionsSeparateOnOneSharedModel) {
  auto model = std::make_shared<OptionsLlm>(true);
  NodeHarness first("llm_generate"), second("prompt_guided_llm");
  first
      .Config({{"bind_model", "shared"},
               {"endpoints", {{"answer", nlohmann::json::object()}}},
               {"system_prompt", "first system"},
               {"random_seed", 11}})
      .BindModel("shared", model)
      .TextInput("input", {"first input"});
  second
      .Config({{"bind_model", "shared"},
               {"system_prompt", "second system"},
               {"random_seed", 22},
               {"prompt_template", "B:{{input}}"}})
      .BindModel("shared", model)
      .TextInput("input", {"second input"});
  ASSERT_TRUE(first.EnsureInitialized());
  ASSERT_TRUE(second.EnsureInitialized());
  EXPECT_EQ(model->seed_queries, 2);
  for (int request = 0; request < 2; ++request) {
    auto first_result = first.Run();
    ASSERT_TRUE(first_result.ok()) << first_result.diagnostic();
    EXPECT_EQ(first_result.TextValues("text"),
              (std::vector<std::string>{"first system|11|first input"}));
    auto second_result = second.Run();
    ASSERT_TRUE(second_result.ok()) << second_result.diagnostic();
    EXPECT_EQ(second_result.TextValues("output"),
              (std::vector<std::string>{"second system|22|B:second input"}));
  }
  ASSERT_EQ(model->calls.size(), 4U);
  for (size_t call = 0; call < model->calls.size(); ++call) {
    EXPECT_EQ(model->calls[call].system_prompt,
              call % 2 == 0 ? "first system" : "second system");
    EXPECT_EQ(model->calls[call].random_seed, call % 2 == 0 ? 11 : 22);
  }
  EXPECT_EQ(model->seed_queries, 2);
}

TEST(FunctionNodeTest, ActualAsrNodeValidatesLanguageAtInitAndPassesItPerCall) {
  auto model = std::make_shared<OptionsAsr>();
  const AudioPcmBatch audio{{9, 3, AudioPcmPayload{{0.1f, 0.2f}, 16000}}};
  NodeHarness defaults("asr_transcribe");
  defaults.Config({{"bind_model", "asr"}})
      .BindModel("asr", model)
      .CustomInput("audio", audio);
  auto refused = defaults.Run();
  EXPECT_FALSE(refused.ok());
  EXPECT_TRUE(refused.init_failed());
  EXPECT_NE(refused.diagnostic().find("does not support language: zh"),
            std::string::npos);
  EXPECT_EQ(model->language_queries, 1);
  EXPECT_TRUE(model->languages.empty());

  NodeHarness english("asr_transcribe");
  english.Config({{"bind_model", "asr"}, {"language", "en"}})
      .BindModel("asr", model)
      .CustomInput("audio", audio);
  ASSERT_TRUE(english.EnsureInitialized());
  EXPECT_EQ(model->language_queries, 2);
  for (int request = 0; request < 2; ++request) {
    auto result = english.Run();
    ASSERT_TRUE(result.ok()) << result.diagnostic();
    EXPECT_EQ(result.TextValues("text"),
              (std::vector<std::string>{"en:transcript"}));
    const auto* output = result.Output<TextBatch>("text");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 1U);
    EXPECT_EQ(output->front().req_id, 9U);
    EXPECT_EQ(output->front().sub_id, 3U);
  }
  EXPECT_EQ(model->languages, (std::vector<std::string>{"en", "en"}));
  EXPECT_EQ(model->language_queries, 2);
}

TEST(FunctionNodeTest, ActualEmbeddingNodesUseSharedModelNormalization) {
  const auto definition =
      ModelRegistry::Instance().Find(GeneratedTextEmbeddingModel::kImplName);
  ASSERT_TRUE(definition.has_value());
  for (bool normalize : {false, true}) {
    SCOPED_TRACE(normalize);
    auto session = std::make_shared<FixedVectorSession>();
    ModelCreateContext context;
    context.backend_session = session;
    std::string diagnostic;
    ASSERT_TRUE(definition->params.Parse(
        {{"embedding_dim", 2}, {"normalize", normalize}}, &context.params,
        &diagnostic))
        << diagnostic;
    auto model = GeneratedTextEmbeddingModel::Create(context, &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    NodeHarness first("text_embedding"), second("text_embedding");
    const TextBatch input{{43, 7, "shared text"}};
    for (auto* harness : {&first, &second})
      harness->Config({{"bind_model", "shared_embedding"}})
          .BindModel("shared_embedding", model)
          .TextInputWithBatch("text", input);
    auto first_result = first.Run();
    auto second_result = second.Run();
    ASSERT_TRUE(first_result.ok()) << first_result.diagnostic();
    ASSERT_TRUE(second_result.ok()) << second_result.diagnostic();
    const auto* left = first_result.Output<EmbeddingBatch>("embedding");
    const auto* right = second_result.Output<EmbeddingBatch>("embedding");
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_EQ(left->size(), 1U);
    ASSERT_EQ(right->size(), 1U);
    EXPECT_EQ(left->front().data, right->front().data);
    for (const auto* output : {left, right}) {
      EXPECT_EQ(output->front().req_id, 43U);
      EXPECT_EQ(output->front().sub_id, 7U);
      const auto& vector = output->front().data;
      ASSERT_EQ(vector.size(), 2U);
      EXPECT_FLOAT_EQ(vector[0], normalize ? 0.6f : 3.0f);
      EXPECT_FLOAT_EQ(vector[1], normalize ? 0.8f : 4.0f);
      EXPECT_NEAR(std::hypot(vector[0], vector[1]), normalize ? 1.0f : 5.0f,
                  1.0e-6f);
    }
    EXPECT_EQ(session->calls, 2);
  }
}

}  // namespace llm_edgeflow
