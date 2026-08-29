#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/backend_interface.h"
#include "engine/backend_registry.h"
#include "engine/inference_definition.h"

namespace alg_framework {
namespace test {

class TestTensorSession : public ITensorGraphSession {
 public:
  explicit TestTensorSession(std::string model_path);
  ~TestTensorSession() override = default;

  const std::string& BackendType() const noexcept override {
    static const std::string type = "test_tensor_backend";
    return type;
  }

  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kTensorGraph;
  }

  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }

  BatchPolicy GetBatchPolicy() const noexcept override {
    return BatchPolicy{16, 0};
  }

  const std::vector<TensorSpec>& Inputs() const noexcept override {
    return input_specs_;
  }

  const std::vector<TensorSpec>& Outputs() const noexcept override {
    return output_specs_;
  }

  int Run(const TensorMap& inputs, TensorMap* outputs,
          std::string* diagnostic = nullptr) noexcept override;

 private:
  std::string model_path_;
  std::vector<TensorSpec> input_specs_;
  std::vector<TensorSpec> output_specs_;
};

class TestTensorBackend : public IInferenceBackend {
 public:
  static constexpr const char* kBackendType = "test_tensor_backend";

  static BackendDefinition MakeDefinition();

  ~TestTensorBackend() override = default;

  const std::string& BackendType() const noexcept override {
    static const std::string type = kBackendType;
    return type;
  }

  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec,
      std::string* diagnostic = nullptr) noexcept override;
};

}  // namespace test
}  // namespace alg_framework
