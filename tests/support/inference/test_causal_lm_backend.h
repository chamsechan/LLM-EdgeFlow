#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "engine/backend_interface.h"
#include "engine/backend_registry.h"
#include "engine/inference_definition.h"

namespace alg_framework {
namespace test {

class TestCausalLmCodec : public ITokenCodec {
 public:
  ~TestCausalLmCodec() override = default;

  int Encode(const std::string& text, bool add_bos,
             std::vector<int32_t>* tokens,
             std::string* diagnostic = nullptr) noexcept override;

  int DecodeToken(int32_t token, std::string* piece,
                  std::string* diagnostic = nullptr) noexcept override;

  bool IsEndToken(int32_t token) const noexcept override;
};

class TestCausalLmSequenceState : public ISequenceState {
 public:
  ~TestCausalLmSequenceState() override = default;
};

class TestCausalLmSession : public ICausalLmSession {
 public:
  explicit TestCausalLmSession(std::string model_path);
  ~TestCausalLmSession() override = default;

  const std::string& BackendType() const noexcept override {
    static const std::string type = "test_causal_lm_backend";
    return type;
  }

  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kCausalLm;
  }

  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }

  BatchPolicy GetBatchPolicy() const noexcept override {
    return BatchPolicy{1, 0};
  }

  ITokenCodec& TokenCodec() noexcept override { return codec_; }

  size_t MaxContextTokens() const noexcept override { return 2048; }

  std::unique_ptr<ISequenceState> CreateSequence(
      std::string* diagnostic = nullptr) noexcept override;

  int Evaluate(const std::vector<int32_t>& tokens, ISequenceState& state,
               std::vector<float>* logits,
               std::string* diagnostic = nullptr) noexcept override;

 private:
  std::string model_path_;
  TestCausalLmCodec codec_;
};

class TestCausalLmBackend : public IInferenceBackend {
 public:
  static constexpr const char* kBackendType = "test_causal_lm_backend";

  static BackendDefinition MakeDefinition();
  static void ResetLoadCount() noexcept;
  static int LoadCount() noexcept;

  ~TestCausalLmBackend() override = default;

  const std::string& BackendType() const noexcept override {
    static const std::string type = kBackendType;
    return type;
  }

  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;

 private:
  static std::atomic<int> load_count_;
};

}  // namespace test
}  // namespace alg_framework
