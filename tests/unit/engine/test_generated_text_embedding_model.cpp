#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/models/generated_text_embedding/generated_text_embedding_model.h"
#include "tests/support/model_probe.h"

namespace llm_edgeflow {
namespace {

class TokenStateSession final : public IGeneratedTokenEmbeddingSession {
 public:
  const std::string& BackendType() const noexcept override { return backend; }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kGeneratedTokenEmbedding;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return {1, 0}; }
  int GenerateEmbeddings(const std::string& prompt, bool add_bos,
                         int max_tokens, GeneratedTokenEmbeddings* output,
                         std::string* diagnostic) noexcept override {
    ++calls;
    observed_prompt = prompt;
    observed_bos = add_bos;
    observed_limit = max_tokens;
    *output = response;
    *diagnostic = reason;
    return code;
  }

  const std::string backend = "neutral_token_state_probe";
  GeneratedTokenEmbeddings response{{8, 9}, {{3, 0}, {3, 8}}};
  int calls = 0;
  int code = 0;
  std::string reason;
  std::string observed_prompt;
  bool observed_bos = false;
  int observed_limit = 0;
};

TEST(GeneratedTextEmbeddingModelTest,
     PromptAndGeneratedStatePoolingUseParameters) {
  struct Case {
    const char* pooling;
    bool normalize;
    std::vector<float> expected;
  };
  const std::vector<Case> cases = {
      {"mean", true, {0.6f, 0.8f}},
      {"mean", false, {3, 4}},
      {"last", false, {3, 8}},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.pooling);
    SCOPED_TRACE(test.normalize);
    auto session = std::make_shared<TokenStateSession>();
    std::string diagnostic;
    auto model = test_support::CreateModel<GeneratedTextEmbeddingModel>(
        session,
        {{"embedding_dim", 2},
         {"max_tokens", 2},
         {"pooling", test.pooling},
         {"normalize", test.normalize},
         {"prompt_prefix", "["},
         {"prompt_suffix", "]"},
         {"add_bos", true}},
        &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    EmbeddingBatch output;
    ASSERT_EQ(model->Embed({{4, 7, "Hi"}}, &output, &diagnostic), 0)
        << diagnostic;
    EXPECT_EQ(session->observed_prompt, "[Hi]");
    EXPECT_TRUE(session->observed_bos);
    EXPECT_EQ(session->observed_limit, 2);
    ASSERT_EQ(output.size(), 1U);
    ASSERT_EQ(output[0].data.size(), 2U);
    EXPECT_NEAR(output[0].data[0], test.expected[0], 1e-6f);
    EXPECT_NEAR(output[0].data[1], test.expected[1], 1e-6f);
    session->response.values[1][0] = 999;
    EXPECT_NEAR(output[0].data[0], test.expected[0], 1e-6f);
    EXPECT_TRUE(diagnostic.empty());
  }
}

TEST(GeneratedTextEmbeddingModelTest,
     EveryGeneratedRowMustBeUsableEvenForLastPooling) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  struct Case {
    const char* name;
    GeneratedTokenEmbeddings response;
    const char* detail;
  };
  const std::vector<Case> cases = {
      {"immediate EOS", {}, "including immediate EOS"},
      {"more than requested",
       {{8, 9, 10}, {{3, 0}, {3, 8}, {1, 1}}},
       "invalid generated token vectors"},
      {"missing token identity",
       {{8}, {{3, 0}, {3, 8}}},
       "invalid generated token vectors"},
      {"extra token identity",
       {{8, 9, 10}, {{3, 0}, {3, 8}}},
       "invalid generated token vectors"},
      {"unselected wrong dimension",
       {{8, 9}, {{3}, {3, 8}}},
       "dimension mismatch"},
      {"selected wrong dimension",
       {{8, 9}, {{3, 0}, {3, 8, 9}}},
       "dimension mismatch"},
      {"unselected NaN", {{8, 9}, {{nan, 0}, {3, 8}}}, "NaN or Inf"},
      {"unselected infinity", {{8, 9}, {{3, inf}, {3, 8}}}, "NaN or Inf"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto session = std::make_shared<TokenStateSession>();
    session->response = test.response;
    std::string diagnostic;
    auto model = test_support::CreateModel<GeneratedTextEmbeddingModel>(
        session,
        {{"embedding_dim", 2},
         {"max_tokens", 2},
         {"pooling", "last"},
         {"normalize", false}},
        &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    EmbeddingBatch output;
    EXPECT_EQ(model->Embed({{0, 0, "Hi"}}, &output, &diagnostic), -1);
    EXPECT_EQ(session->calls, 1);
    EXPECT_TRUE(output.empty());
    EXPECT_NE(diagnostic.find(test.detail), std::string::npos) << diagnostic;
  }
}

TEST(GeneratedTextEmbeddingModelTest,
     EmptyTextNeverGeneratesAndSessionErrorKeepsCode) {
  auto session = std::make_shared<TokenStateSession>();
  std::string diagnostic;
  auto model = test_support::CreateModel<GeneratedTextEmbeddingModel>(
      session, {{"embedding_dim", 2}, {"max_tokens", 2}}, &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  EmbeddingBatch output;
  EXPECT_EQ(model->Embed({{0, 0, ""}}, &output, &diagnostic), -1);
  EXPECT_EQ(session->calls, 0);
  EXPECT_EQ(diagnostic, "Embedding text is empty");
  session->code = -719;
  session->reason = "hidden states unavailable";
  EXPECT_EQ(model->Embed({{0, 0, "Hi"}}, &output, &diagnostic), -719);
  EXPECT_EQ(diagnostic, "hidden states unavailable");
  EXPECT_TRUE(output.empty());
}

}  // namespace
}  // namespace llm_edgeflow
