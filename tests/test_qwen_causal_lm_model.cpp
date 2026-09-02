#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/backend_interface.h"
#include "engine/model_registry.h"
#include "engine/models/qwen_causal_lm/qwen_causal_lm_model.h"
#include "engine/text_generation/common_autoregressive_generator.h"

namespace alg_framework {
namespace {

class ScriptedGenerationSession final : public ITextGenerationSession {
 public:
  struct Call {
    std::string prompt;
    bool add_bos = false;
    GenerateOptions options;
    std::optional<uint64_t> seed;
  };

  const std::string& BackendType() const noexcept override {
    static const std::string type = "scripted_text_generation";
    return type;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kTextGeneration;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return policy; }

  int Generate(const std::string& prompt, bool add_bos,
               const GenerateOptions& options, std::optional<uint64_t> seed,
               std::string* output, std::string* diagnostic) noexcept override {
    if (!output) return -1;
    output->clear();
    try {
      calls.push_back({prompt, add_bos, options, seed});
      const size_t call_index = calls.size() - 1;
      if (call_index == fail_call) {
        if (diagnostic) *diagnostic = "scripted failure";
        return -1;
      }
      *output = call_index < scripted_outputs.size()
                    ? scripted_outputs[call_index]
                    : "generated";
      return 0;
    } catch (...) {
      output->clear();
      return -1;
    }
  }

  BatchPolicy policy{1, 0};
  std::vector<std::string> scripted_outputs;
  std::vector<Call> calls;
  size_t fail_call = std::numeric_limits<size_t>::max();
};

GenerateOptions GreedyOptions() {
  GenerateOptions options;
  options.max_tokens = 8;
  options.temperature = 0.0f;
  options.top_k = 0;
  options.top_p = 1.0f;
  options.repetition_penalty = 1.0f;
  return options;
}

TEST(QwenCausalLmModelTest, DefinitionAndCreationRequireTextGeneration) {
  const auto definition =
      ModelRegistry::Instance().Find(QwenCausalLmModel::kModelType);
  ASSERT_TRUE(definition.has_value());
  EXPECT_EQ(definition->capability, "llm");
  EXPECT_EQ(definition->required_protocol, ExecutionProtocol::kTextGeneration);
  EXPECT_EQ(definition->concurrency, InferenceConcurrency::kConcurrent);

  ModelCreateContext invalid;
  std::string diagnostic;
  EXPECT_EQ(QwenCausalLmModel::Create(invalid, &diagnostic), nullptr);
  EXPECT_FALSE(diagnostic.empty());

  auto session = std::make_shared<ScriptedGenerationSession>();
  ModelCreateContext valid;
  valid.backend_session = session;
  valid.model_config = {{"chat_template", "qwen_chatml"},
                        {"system_prompt", "You are concise."},
                        {"random_seed", 7}};
  auto model = QwenCausalLmModel::Create(valid, &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  EXPECT_EQ(model->ModelType(), "qwen_causal_lm");
  EXPECT_EQ(model->Concurrency(), InferenceConcurrency::kConcurrent);

  session->policy = {1, 1};
  EXPECT_EQ(QwenCausalLmModel::Create(valid, &diagnostic), nullptr);
}

TEST(QwenCausalLmModelTest,
     DelegatesFormattedPromptOptionsSeedAndPreservesProvenance) {
  auto session = std::make_shared<ScriptedGenerationSession>();
  session->scripted_outputs = {"first-answer", "second-answer"};
  QwenCausalLmModel model(session, "System", true, 11);

  GenerateOptions options = GreedyOptions();
  options.top_k = 17;
  options.top_p = 0.75f;
  options.repetition_penalty = 1.25f;
  options.stop_words = {"STOP"};
  TextBatch outputs;
  ASSERT_EQ(model.Generate({{101, 3, "first"}, {202, 7, "second"}}, options,
                           &outputs),
            0);
  ASSERT_EQ(outputs.size(), 2U);
  EXPECT_EQ(outputs[0].req_id, 101U);
  EXPECT_EQ(outputs[0].sub_id, 3U);
  EXPECT_EQ(outputs[0].data, "first-answer");
  EXPECT_EQ(outputs[1].req_id, 202U);
  EXPECT_EQ(outputs[1].sub_id, 7U);
  EXPECT_EQ(outputs[1].data, "second-answer");

  ASSERT_EQ(session->calls.size(), 2U);
  EXPECT_TRUE(session->calls[0].add_bos);
  EXPECT_EQ(session->calls[0].options.top_k, 17);
  EXPECT_FLOAT_EQ(session->calls[0].options.repetition_penalty, 1.25f);
  EXPECT_EQ(session->calls[0].options.stop_words,
            std::vector<std::string>({"STOP"}));
  EXPECT_NE(
      session->calls[0].prompt.find("<|im_start|>system\nSystem<|im_end|>\n"),
      std::string::npos);
  EXPECT_NE(session->calls[0].prompt.find("<|im_start|>user\nfirst<|im_end|>\n"
                                          "<|im_start|>assistant\n"),
            std::string::npos);
  ASSERT_TRUE(session->calls[0].seed.has_value());
  ASSERT_TRUE(session->calls[1].seed.has_value());
  EXPECT_NE(session->calls[0].seed, session->calls[1].seed);
}

TEST(QwenCausalLmModelTest, RandomSeedAndLaterFailureRollbackBatch) {
  auto session = std::make_shared<ScriptedGenerationSession>();
  session->fail_call = 1;
  QwenCausalLmModel model(session, "", false, -1);

  TextBatch outputs{{9, 9, "stale"}};
  EXPECT_NE(model.Generate({{1, 0, "first"}, {2, 0, "second"}}, GreedyOptions(),
                           &outputs),
            0);
  EXPECT_TRUE(outputs.empty());
  ASSERT_EQ(session->calls.size(), 2U);
  EXPECT_FALSE(session->calls[0].seed.has_value());
  EXPECT_FALSE(session->calls[1].seed.has_value());

  EXPECT_NE(model.Generate({{3, 0, ""}}, GreedyOptions(), &outputs), 0);
  EXPECT_TRUE(outputs.empty());
}

class ScriptedDecoder final : public text_generation::IAutoregressiveDecoder {
 public:
  int Encode(const std::string& text, bool add_bos,
             std::vector<int32_t>* tokens,
             std::string* diagnostic) noexcept override {
    (void)diagnostic;
    if (!tokens || fail_encode) return -1;
    encoded = text;
    tokens->clear();
    if (add_bos) tokens->push_back(1);
    tokens->insert(tokens->end(), prompt_tokens.begin(), prompt_tokens.end());
    return 0;
  }
  int DecodeToken(int32_t token, std::string* piece,
                  std::string* diagnostic) noexcept override {
    (void)diagnostic;
    if (!piece || fail_decode) return -1;
    const auto it = pieces.find(token);
    if (it == pieces.end()) return -1;
    *piece = it->second;
    return 0;
  }
  bool IsEndToken(int32_t token) const noexcept override {
    return token == eos_token;
  }
  size_t MaxContextTokens() const noexcept override { return max_context; }
  int Evaluate(const std::vector<int32_t>& tokens, std::vector<float>* logits,
               std::string* diagnostic) noexcept override {
    (void)diagnostic;
    if (!logits || tokens.empty() || fail_evaluate) return -1;
    history.push_back(tokens);
    if (emit_empty) {
      logits->clear();
      return 0;
    }
    if (step >= scripted_logits.size()) return -1;
    *logits = scripted_logits[step++];
    return 0;
  }

  std::vector<int32_t> prompt_tokens{42};
  std::unordered_map<int32_t, std::string> pieces;
  std::vector<std::vector<float>> scripted_logits;
  std::vector<std::vector<int32_t>> history;
  std::string encoded;
  size_t step = 0;
  size_t max_context = 32;
  int32_t eos_token = 2;
  bool fail_encode = false;
  bool fail_decode = false;
  bool fail_evaluate = false;
  bool emit_empty = false;
};

std::vector<float> Logits(size_t size,
                          std::initializer_list<std::pair<size_t, float>> set) {
  std::vector<float> logits(size, -20.0f);
  for (const auto& [index, value] : set) logits[index] = value;
  return logits;
}

TEST(CommonAutoregressiveGeneratorTest,
     GreedyEosStopWordsAndIncrementalEvaluation) {
  ScriptedDecoder decoder;
  decoder.pieces = {{10, "prefixST"}, {11, "OP"}};
  decoder.scripted_logits = {Logits(16, {{10, 20.0f}}),
                             Logits(16, {{11, 20.0f}}),
                             Logits(16, {{2, 20.0f}})};
  auto options = GreedyOptions();
  options.stop_words = {"STOP"};

  std::string output;
  std::string diagnostic;
  ASSERT_EQ(text_generation::CommonAutoregressiveGenerator::Generate(
                decoder, "formatted", false, options, 7, &output, &diagnostic),
            0)
      << diagnostic;
  EXPECT_EQ(output, "prefix");
  ASSERT_EQ(decoder.history.size(), 2U);
  EXPECT_EQ(decoder.history[0], std::vector<int32_t>({42}));
  EXPECT_EQ(decoder.history[1], std::vector<int32_t>({10}));
}

TEST(CommonAutoregressiveGeneratorTest,
     TopKTopPTemperatureAndFixedSeedAreDeterministic) {
  auto make_decoder = [] {
    ScriptedDecoder decoder;
    decoder.pieces = {{5, "A"}, {6, "B"}};
    decoder.scripted_logits = {Logits(8, {{5, 5.0f}, {6, 4.0f}}),
                               Logits(8, {{2, 8.0f}})};
    return decoder;
  };
  GenerateOptions options = GreedyOptions();
  options.temperature = 0.8f;
  options.top_k = 2;
  options.top_p = 0.9f;

  auto first_decoder = make_decoder();
  auto second_decoder = make_decoder();
  std::string first;
  std::string second;
  ASSERT_EQ(text_generation::CommonAutoregressiveGenerator::Generate(
                first_decoder, "prompt", false, options, 1234, &first),
            0);
  ASSERT_EQ(text_generation::CommonAutoregressiveGenerator::Generate(
                second_decoder, "prompt", false, options, 1234, &second),
            0);
  EXPECT_EQ(first, second);

  auto top_p_decoder = make_decoder();
  options.top_k = 0;
  options.top_p = 0.01f;
  ASSERT_EQ(text_generation::CommonAutoregressiveGenerator::Generate(
                top_p_decoder, "prompt", false, options, 99, &first),
            0);
  EXPECT_EQ(first, "A");

  auto top_one_decoder = make_decoder();
  options.top_k = 1;
  options.top_p = 1.0f;
  ASSERT_EQ(text_generation::CommonAutoregressiveGenerator::Generate(
                top_one_decoder, "prompt", false, options, 99, &first),
            0);
  EXPECT_EQ(first, "A");
}

TEST(CommonAutoregressiveGeneratorTest, RepetitionPenaltyChangesGreedyChoice) {
  ScriptedDecoder decoder;
  decoder.prompt_tokens = {5};
  decoder.pieces = {{5, "repeat"}, {6, "fresh"}};
  decoder.scripted_logits = {Logits(8, {{5, 10.0f}, {6, 6.0f}}),
                             Logits(8, {{2, 20.0f}})};
  auto options = GreedyOptions();
  options.repetition_penalty = 2.0f;
  std::string output;
  ASSERT_EQ(text_generation::CommonAutoregressiveGenerator::Generate(
                decoder, "prompt", false, options, 0, &output),
            0);
  EXPECT_EQ(output, "fresh");
}

TEST(CommonAutoregressiveGeneratorTest,
     RejectsInvalidOptionsContextAndExceptionalLogits) {
  ScriptedDecoder decoder;
  decoder.scripted_logits = {{0.0f, std::numeric_limits<float>::quiet_NaN()}};
  auto options = GreedyOptions();
  std::string output = "stale";
  EXPECT_NE(text_generation::CommonAutoregressiveGenerator::Generate(
                decoder, "prompt", false, options, 0, &output),
            0);
  EXPECT_TRUE(output.empty());

  ScriptedDecoder infinite_decoder;
  infinite_decoder.scripted_logits = {
      {0.0f, std::numeric_limits<float>::infinity()}};
  output = "stale";
  EXPECT_NE(text_generation::CommonAutoregressiveGenerator::Generate(
                infinite_decoder, "prompt", false, options, 0, &output),
            0);
  EXPECT_TRUE(output.empty());

  ScriptedDecoder context_decoder;
  context_decoder.max_context = 1;
  context_decoder.scripted_logits = {Logits(4, {{2, 1.0f}})};
  EXPECT_NE(text_generation::CommonAutoregressiveGenerator::Generate(
                context_decoder, "prompt", false, options, 0, &output),
            0);

  ScriptedDecoder option_decoder;
  option_decoder.scripted_logits = {Logits(4, {{2, 1.0f}})};
  options.top_k = -1;
  EXPECT_NE(text_generation::CommonAutoregressiveGenerator::Generate(
                option_decoder, "prompt", false, options, 0, &output),
            0);
  options = GreedyOptions();
  options.repetition_penalty = 0.0f;
  EXPECT_NE(text_generation::CommonAutoregressiveGenerator::Generate(
                option_decoder, "prompt", false, options, 0, &output),
            0);
}

TEST(CommonAutoregressiveGeneratorTest,
     DecoderFailuresAndEmptyLogitsRollbackOutput) {
  const auto expect_failure = [](ScriptedDecoder* decoder) {
    GenerateOptions options = GreedyOptions();
    std::string output = "stale";
    EXPECT_NE(text_generation::CommonAutoregressiveGenerator::Generate(
                  *decoder, "prompt", false, options, 0, &output),
              0);
    EXPECT_TRUE(output.empty());
  };

  ScriptedDecoder encode_failure;
  encode_failure.fail_encode = true;
  expect_failure(&encode_failure);

  ScriptedDecoder evaluate_failure;
  evaluate_failure.fail_evaluate = true;
  expect_failure(&evaluate_failure);

  ScriptedDecoder empty_logits;
  empty_logits.emit_empty = true;
  expect_failure(&empty_logits);

  ScriptedDecoder decode_failure;
  decode_failure.fail_decode = true;
  decode_failure.scripted_logits = {Logits(8, {{5, 10.0f}})};
  expect_failure(&decode_failure);
}

TEST(QwenCausalLmModelTest, Utf8SuffixCompatibilityHelper) {
  std::string incomplete = std::string("ok") + "\xE4\xB8";
  QwenCausalLmModel::StripIncompleteUtf8Suffix(&incomplete);
  EXPECT_EQ(incomplete, "ok");
  std::string complete = "中文";
  QwenCausalLmModel::StripIncompleteUtf8Suffix(&complete);
  EXPECT_EQ(complete, "中文");
  std::string dangling_continuation = std::string("ok") + "\x80";
  QwenCausalLmModel::StripIncompleteUtf8Suffix(&dangling_continuation);
  EXPECT_EQ(dangling_continuation, "ok");

  auto session = std::make_shared<ScriptedGenerationSession>();
  session->scripted_outputs = {std::string("managed") + "\xE4\xB8"};
  QwenCausalLmModel model(session, "", false, 0);
  TextBatch outputs;
  ASSERT_EQ(model.Generate({{1, 0, "prompt"}}, GreedyOptions(), &outputs), 0);
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs.front().data, "managed");
}

}  // namespace
}  // namespace alg_framework
