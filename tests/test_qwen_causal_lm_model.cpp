#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/backend_interface.h"
#include "engine/model_registry.h"
#include "engine/models/qwen_causal_lm/qwen_causal_lm_model.h"

namespace alg_framework {
namespace {

class ScriptedCodec final : public ITokenCodec {
 public:
  int Encode(const std::string& text, bool add_bos,
             std::vector<int32_t>* tokens,
             std::string* diagnostic) noexcept override {
    (void)diagnostic;
    if (!tokens || fail_encode) return -1;
    tokens->clear();
    if (add_bos) tokens->push_back(1);
    tokens->push_back(42);
    encoded_texts.push_back(text);
    return 0;
  }

  int DecodeToken(int32_t token, std::string* piece,
                  std::string* diagnostic) noexcept override {
    (void)diagnostic;
    if (!piece || fail_decode) return -1;
    piece->clear();
    const auto it = pieces.find(token);
    if (it == pieces.end()) return -1;
    *piece = it->second;
    return 0;
  }

  bool IsEndToken(int32_t token) const noexcept override { return token == 2; }

  bool fail_encode = false;
  bool fail_decode = false;
  std::unordered_map<int32_t, std::string> pieces;
  std::vector<std::string> encoded_texts;
};

class ScriptedSession;

class ScriptedSequence final : public ICausalLmSequence {
 public:
  ScriptedSequence(ScriptedSession* owner, size_t state_id)
      : owner_(owner), id(state_id) {}

  int Evaluate(const std::vector<int32_t>& tokens, std::vector<float>* logits,
               std::string* diagnostic) noexcept override;

  ScriptedSession* owner_ = nullptr;
  size_t id = 0;
  size_t step = 0;
};

class ScriptedSession final : public ICausalLmSession {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string type = "scripted_causal";
    return type;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kCausalLm;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return policy; }
  ITokenCodec& TokenCodec() noexcept override { return codec; }
  size_t MaxContextTokens() const noexcept override { return max_context; }

  std::unique_ptr<ICausalLmSequence> CreateSequence(
      std::string* diagnostic) noexcept override {
    (void)diagnostic;
    if (fail_create) return nullptr;
    try {
      const size_t id = next_state_id++;
      histories.resize(next_state_id);
      return std::make_unique<ScriptedSequence>(this, id);
    } catch (...) {
      return nullptr;
    }
  }

  ScriptedCodec codec;
  BatchPolicy policy{1, 0};
  size_t max_context = 32;
  std::vector<int32_t> scripted_tokens{2};
  std::vector<std::vector<std::vector<int32_t>>> histories;
  size_t next_state_id = 0;
  size_t fail_state_id = std::numeric_limits<size_t>::max();
  bool fail_create = false;
  bool emit_empty_logits = false;
  bool emit_nan_logits = false;
};

int ScriptedSequence::Evaluate(const std::vector<int32_t>& tokens,
                               std::vector<float>* logits,
                               std::string* diagnostic) noexcept {
  (void)diagnostic;
  if (!owner_ || !logits || tokens.empty() || id == owner_->fail_state_id) {
    if (logits) logits->clear();
    return -1;
  }
  try {
    owner_->histories[id].push_back(tokens);
    if (owner_->emit_empty_logits) {
      logits->clear();
      return 0;
    }
    const int32_t token = step < owner_->scripted_tokens.size()
                              ? owner_->scripted_tokens[step]
                              : 2;
    ++step;
    logits->assign(128, -20.0f);
    if (owner_->emit_nan_logits) {
      (*logits)[0] = std::numeric_limits<float>::quiet_NaN();
    } else if (token >= 0 && static_cast<size_t>(token) < logits->size()) {
      (*logits)[static_cast<size_t>(token)] = 20.0f;
    } else {
      return -1;
    }
    return 0;
  } catch (...) {
    logits->clear();
    return -1;
  }
}

GenerateOptions GreedyOptions() {
  GenerateOptions options;
  options.max_tokens = 8;
  options.temperature = 0.0f;
  options.top_p = 1.0f;
  return options;
}

TEST(QwenCausalLmModelTest, DefinitionAndCreationRequireCausalProtocol) {
  const auto definition =
      ModelRegistry::Instance().Find(QwenCausalLmModel::kModelType);
  ASSERT_TRUE(definition.has_value());
  EXPECT_EQ(definition->capability, "llm");
  EXPECT_EQ(definition->required_protocol, ExecutionProtocol::kCausalLm);
  EXPECT_EQ(definition->concurrency, InferenceConcurrency::kConcurrent);

  ModelCreateContext invalid;
  std::string diagnostic;
  EXPECT_EQ(QwenCausalLmModel::Create(invalid, &diagnostic), nullptr);
  EXPECT_FALSE(diagnostic.empty());

  auto session = std::make_shared<ScriptedSession>();
  ModelCreateContext valid;
  valid.backend_session = session;
  valid.model_config = {{"chat_template", "qwen_chatml"},
                        {"system_prompt", "You are concise."},
                        {"random_seed", 7}};
  auto model = QwenCausalLmModel::Create(valid, &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  EXPECT_EQ(model->ModelType(), "qwen_causal_lm");
  EXPECT_EQ(model->Concurrency(), InferenceConcurrency::kConcurrent);
}

TEST(QwenCausalLmModelTest,
     GreedyGenerationPreservesProvenanceAndSequenceState) {
  auto session = std::make_shared<ScriptedSession>();
  session->scripted_tokens = {65, 66, 2};
  session->codec.pieces = {{65, "A"}, {66, "B"}};
  QwenCausalLmModel model(session, "System", false, 11);

  TextBatch prompts{{101, 3, "first"}, {202, 7, "second"}};
  TextBatch outputs;
  ASSERT_EQ(model.Generate(prompts, GreedyOptions(), &outputs), 0);
  ASSERT_EQ(outputs.size(), 2U);
  EXPECT_EQ(outputs[0].req_id, 101U);
  EXPECT_EQ(outputs[0].sub_id, 3U);
  EXPECT_EQ(outputs[0].data, "AB");
  EXPECT_EQ(outputs[1].req_id, 202U);
  EXPECT_EQ(outputs[1].sub_id, 7U);
  EXPECT_EQ(outputs[1].data, "AB");

  ASSERT_EQ(session->histories.size(), 2U);
  for (const auto& history : session->histories) {
    ASSERT_EQ(history.size(), 3U);
    EXPECT_EQ(history[1], std::vector<int32_t>({65}));
    EXPECT_EQ(history[2], std::vector<int32_t>({66}));
  }
  ASSERT_EQ(session->codec.encoded_texts.size(), 2U);
  EXPECT_NE(session->codec.encoded_texts[0].find(
                "<|im_start|>system\nSystem<|im_end|>\n"),
            std::string::npos);
  EXPECT_NE(
      session->codec.encoded_texts[0].find("<|im_start|>user\nfirst<|im_end|>\n"
                                           "<|im_start|>assistant\n"),
      std::string::npos);
}

TEST(QwenCausalLmModelTest, StopWordMaySpanMultipleTokenPieces) {
  auto session = std::make_shared<ScriptedSession>();
  session->scripted_tokens = {10, 11, 12, 2};
  session->codec.pieces = {{10, "prefixST"}, {11, "OP"}, {12, "ignored"}};
  QwenCausalLmModel model(session, "", false, 0);

  auto options = GreedyOptions();
  options.stop_words = {"STOP"};
  TextBatch outputs;
  ASSERT_EQ(model.Generate({{1, 0, "prompt"}}, options, &outputs), 0);
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].data, "prefix");
  ASSERT_EQ(session->histories.size(), 1U);
  EXPECT_EQ(session->histories[0].size(), 2U);
}

TEST(QwenCausalLmModelTest, TopPSamplingPathIsDeterministicWithFixedSeed) {
  auto session = std::make_shared<ScriptedSession>();
  session->scripted_tokens = {65, 2};
  session->codec.pieces = {{65, "A"}};
  QwenCausalLmModel model(session, "", false, 1234);

  auto options = GreedyOptions();
  options.temperature = 0.7f;
  options.top_p = 0.9f;
  TextBatch first;
  ASSERT_EQ(model.Generate({{7, 9, "prompt"}}, options, &first), 0);
  ASSERT_EQ(first.size(), 1U);
  EXPECT_EQ(first[0].data, "A");
}

TEST(QwenCausalLmModelTest, InvalidLogitsAndLaterRequestFailureRollbackBatch) {
  auto session = std::make_shared<ScriptedSession>();
  session->scripted_tokens = {65, 2};
  session->codec.pieces = {{65, "A"}};
  session->fail_state_id = 1;
  QwenCausalLmModel model(session, "", false, 0);

  TextBatch outputs{{9, 9, "stale"}};
  EXPECT_NE(model.Generate({{1, 0, "first"}, {2, 0, "second"}}, GreedyOptions(),
                           &outputs),
            0);
  EXPECT_TRUE(outputs.empty());

  session->fail_state_id = std::numeric_limits<size_t>::max();
  session->emit_nan_logits = true;
  EXPECT_NE(model.Generate({{3, 0, "third"}}, GreedyOptions(), &outputs), 0);
  EXPECT_TRUE(outputs.empty());
}

TEST(QwenCausalLmModelTest, ValidatesOptionsContextAndUtf8Suffix) {
  auto session = std::make_shared<ScriptedSession>();
  session->codec.pieces = {{65, "A"}};
  QwenCausalLmModel model(session, "", false, 0);
  TextBatch outputs;

  auto invalid = GreedyOptions();
  invalid.top_p = 0.0f;
  EXPECT_NE(model.Generate({{1, 0, "prompt"}}, invalid, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  session->max_context = 1;
  EXPECT_NE(model.Generate({{1, 0, "prompt"}}, GreedyOptions(), &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  std::string incomplete = std::string("ok") + "\xE4\xB8";
  QwenCausalLmModel::StripIncompleteUtf8Suffix(&incomplete);
  EXPECT_EQ(incomplete, "ok");
  std::string complete = "中文";
  QwenCausalLmModel::StripIncompleteUtf8Suffix(&complete);
  EXPECT_EQ(complete, "中文");
  std::string dangling_continuation = std::string("ok") + "\x80";
  QwenCausalLmModel::StripIncompleteUtf8Suffix(&dangling_continuation);
  EXPECT_EQ(dangling_continuation, "ok");
}

}  // namespace
}  // namespace alg_framework
