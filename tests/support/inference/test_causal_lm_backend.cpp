#include "tests/support/inference/test_causal_lm_backend.h"

#include <utility>

namespace alg_framework {
namespace test {

std::atomic<int> TestCausalLmBackend::load_count_{0};

int TestCausalLmCodec::Encode(const std::string& text, bool add_bos,
                              std::vector<int32_t>* tokens,
                              std::string* diagnostic) noexcept {
  try {
    (void)diagnostic;
    if (!tokens) return -1;
    tokens->clear();
    if (add_bos) tokens->push_back(1);
    for (char c : text) {
      tokens->push_back(static_cast<int32_t>(static_cast<unsigned char>(c)));
    }
    return 0;
  } catch (...) {
    try {
      if (tokens) tokens->clear();
    } catch (...) {
    }
    return -2;
  }
}

int TestCausalLmCodec::DecodeToken(int32_t token, std::string* piece,
                                   std::string* diagnostic) noexcept {
  try {
    (void)diagnostic;
    if (!piece) return -1;
    piece->assign(1, static_cast<char>(token));
    return 0;
  } catch (...) {
    return -2;
  }
}

bool TestCausalLmCodec::IsEndToken(int32_t token) const noexcept {
  return token == 2 || token == 0;
}

TestCausalLmSession::TestCausalLmSession(std::string model_path)
    : model_path_(std::move(model_path)) {}

int TestCausalLmSequence::Evaluate(const std::vector<int32_t>& tokens,
                                   std::vector<float>* logits,
                                   std::string* diagnostic) noexcept {
  try {
    (void)tokens;
    (void)diagnostic;
    if (!logits) return -1;
    logits->assign(32000, 0.0f);
    (*logits)[2] = 1.0f;
    return 0;
  } catch (...) {
    try {
      if (logits) logits->clear();
    } catch (...) {
    }
    return -2;
  }
}

std::unique_ptr<ICausalLmSequence> TestCausalLmSession::CreateSequence(
    std::string* diagnostic) noexcept {
  (void)diagnostic;
  try {
    return std::make_unique<TestCausalLmSequence>();
  } catch (...) {
    return nullptr;
  }
}

BackendDefinition TestCausalLmBackend::MakeDefinition() {
  BackendDefinition def;
  def.backend_type = kBackendType;
  def.description = "Test Causal LM Backend Fixture";
  def.supported_protocols = {ExecutionProtocol::kCausalLm};
  def.concurrency = InferenceConcurrency::kSerialized;
  return def;
}

void TestCausalLmBackend::ResetLoadCount() noexcept { load_count_.store(0); }

int TestCausalLmBackend::LoadCount() noexcept { return load_count_.load(); }

std::shared_ptr<IBackendSession> TestCausalLmBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  (void)diagnostic;
  load_count_.fetch_add(1);
  try {
    return std::make_shared<TestCausalLmSession>(spec.model_path);
  } catch (...) {
    return nullptr;
  }
}

REGISTER_BACKEND_WITH_DEFINITION(TestCausalLmBackend,
                                 TestCausalLmBackend::MakeDefinition());

}  // namespace test
}  // namespace alg_framework
