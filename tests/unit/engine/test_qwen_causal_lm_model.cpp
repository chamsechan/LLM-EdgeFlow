#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/models/qwen_causal_lm/qwen_causal_lm_model.h"
#include "tests/support/model_probe.h"

namespace llm_edgeflow {
namespace {

class TextSession final : public ITextGenerationSession {
 public:
  const std::string& BackendType() const noexcept override { return backend; }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kTextGeneration;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return {1, 0}; }
  bool SupportsRandomSeed() const noexcept override { return supports_seed; }
  int Generate(const std::string& prompt, bool add_bos,
               const GenerateOptions& options, std::optional<uint64_t> seed,
               std::string* output, std::string* diagnostic) noexcept override {
    ++calls;
    observed_prompt = prompt;
    observed_bos = add_bos;
    observed_options = options;
    observed_seed = seed;
    *output = response;
    *diagnostic = reason;
    return code;
  }

  const std::string backend = "neutral_text_probe";
  bool supports_seed = true;
  int calls = 0;
  int code = 0;
  std::string reason;
  std::string response = "answer";
  std::string observed_prompt;
  bool observed_bos = false;
  GenerateOptions observed_options;
  std::optional<uint64_t> observed_seed;
};

TEST(QwenCausalLmModelTest, ChatMlAndAllCallOptionsReachTheSession) {
  auto session = std::make_shared<TextSession>();
  std::string diagnostic;
  auto model = test_support::CreateModel<QwenCausalLmModel>(
      session, {{"add_bos", true}}, &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  GenerateOptions options;
  options.system_prompt = "S";
  options.max_tokens = 7;
  options.temperature = 0.25f;
  options.top_k = 3;
  options.top_p = 0.8f;
  options.repetition_penalty = 1.2f;
  options.stop_words = {"stop", "end"};
  options.random_seed = 0;
  TextBatch output;
  ASSERT_EQ(model->Generate({{1, 2, "Hi"}}, options, &output, &diagnostic), 0)
      << diagnostic;
  EXPECT_EQ(session->observed_prompt,
            "<|im_start|>system\nS<|im_end|>\n"
            "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n");
  EXPECT_TRUE(session->observed_bos);
  EXPECT_EQ(session->observed_options.system_prompt, "S");
  EXPECT_EQ(session->observed_options.max_tokens, 7);
  EXPECT_FLOAT_EQ(session->observed_options.temperature, 0.25f);
  EXPECT_EQ(session->observed_options.top_k, 3);
  EXPECT_FLOAT_EQ(session->observed_options.top_p, 0.8f);
  EXPECT_FLOAT_EQ(session->observed_options.repetition_penalty, 1.2f);
  EXPECT_EQ(session->observed_options.stop_words,
            (std::vector<std::string>{"stop", "end"}));
  EXPECT_EQ(session->observed_options.random_seed, 0);
  ASSERT_TRUE(session->observed_seed.has_value());
  EXPECT_EQ(*session->observed_seed, 14813675350809533578ULL);
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(output[0].data, "answer");
  session->response = "changed";
  EXPECT_EQ(output[0].data, "answer");
  EXPECT_TRUE(diagnostic.empty());
}

TEST(QwenCausalLmModelTest, DefaultBosNegativeSeedAndOptionalSystemPrompt) {
  auto session = std::make_shared<TextSession>();
  std::string diagnostic;
  auto model = test_support::CreateModel<QwenCausalLmModel>(
      session, nlohmann::json::object(), &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  GenerateOptions options;
  options.random_seed = -7;
  TextBatch output;
  ASSERT_EQ(model->Generate({{3, 5, "Hi"}}, options, &output, &diagnostic), 0);
  EXPECT_EQ(session->observed_prompt,
            "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n");
  EXPECT_FALSE(session->observed_bos);
  EXPECT_FALSE(session->observed_seed.has_value());
  EXPECT_TRUE(model->SupportsRandomSeed());
  session->supports_seed = false;
  EXPECT_FALSE(model->SupportsRandomSeed());
}

TEST(QwenCausalLmModelTest, OnlyIncompleteUtf8SuffixIsRemoved) {
  struct Case {
    const char* name;
    std::string response;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {"complete multibyte", "A你", "A你"},
      {"incomplete two trailing bytes", std::string("A\xe4\xb8", 3), "A"},
      {"entirely incomplete", std::string("\xe4", 1), ""},
      {"empty generated response", "", ""},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto session = std::make_shared<TextSession>();
    session->response = test.response;
    QwenCausalLmModel model(session, false);
    TextBatch output;
    std::string diagnostic;
    ASSERT_EQ(model.Generate({{0, 0, "Hi"}}, {}, &output, &diagnostic), 0);
    ASSERT_EQ(output.size(), 1U);
    EXPECT_EQ(output[0].data, test.expected);
    EXPECT_TRUE(diagnostic.empty());
  }
}

TEST(QwenCausalLmModelTest, EmptyPromptIsRejectedAndBackendFailureIsPreserved) {
  auto session = std::make_shared<TextSession>();
  QwenCausalLmModel model(session, false);
  TextBatch output;
  std::string diagnostic;
  EXPECT_EQ(model.Generate({{0, 0, ""}}, {}, &output, &diagnostic), -1);
  EXPECT_EQ(session->calls, 0);
  EXPECT_NE(diagnostic.find("non-empty prompt"), std::string::npos);
  session->code = -719;
  session->reason = "decoder exhausted its resource";
  EXPECT_EQ(model.Generate({{0, 0, "Hi"}}, {}, &output, &diagnostic), -719);
  EXPECT_EQ(session->calls, 1);
  EXPECT_EQ(diagnostic, "decoder exhausted its resource");
  EXPECT_TRUE(output.empty());
}

}  // namespace
}  // namespace llm_edgeflow
