#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "engine/backend_interface.h"
#include "engine/backend_registry.h"
#include "engine/inference_definition.h"

namespace llm_edgeflow {
namespace test {

// 测试专用 fixture 协议会话，保留文本生成接口供协议测试调用。
class TestCausalLmSession : public ITextGenerationSession {
 public:
  bool SupportsRandomSeed() const noexcept override { return true; }
  TestCausalLmSession(std::string model_file, BatchPolicy policy = {1, 0});
  ~TestCausalLmSession() override = default;

  const std::string& BackendType() const noexcept override {
    static const std::string type = "test_causal_lm_backend";
    return type;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kFixture;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return policy_; }

  int Generate(const std::string& formatted_prompt, bool add_bos,
               const GenerateOptions& options, std::optional<uint64_t> seed,
               std::string* output,
               std::string* diagnostic = nullptr) noexcept override;

 private:
  std::string model_file_;
  BatchPolicy policy_;
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
}  // namespace llm_edgeflow
