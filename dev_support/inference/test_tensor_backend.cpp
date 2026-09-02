#include "dev_support/inference/test_tensor_backend.h"

#include <utility>

namespace llm_edgeflow {
namespace test {

std::atomic<int> TestTensorBackend::requested_protocol_{-1};

TestTensorSession::TestTensorSession(std::string model_path)
    : model_path_(std::move(model_path)) {
  input_specs_ = {
      {"input_ids", ElementType::kInt64, {-1, -1}},
  };
  output_specs_ = {
      {"embeddings", ElementType::kFloat32, {-1, 384}},
  };
}

int TestTensorSession::Run(const TensorMap& inputs, TensorMap* outputs,
                           std::string* diagnostic) noexcept {
  try {
    (void)inputs;
    if (!outputs) {
      if (diagnostic) *diagnostic = "Output TensorMap pointer is null";
      return -1;
    }
    outputs->clear();
    Tensor out_t;
    TensorDesc desc{ElementType::kFloat32, {1, 384}};
    if (!CreateHostTensor(desc, &out_t, diagnostic)) return -2;
    (*outputs)["embeddings"] = std::move(out_t);
    return 0;
  } catch (...) {
    try {
      if (outputs) outputs->clear();
      if (diagnostic) *diagnostic = "Exception in TestTensorSession::Run";
    } catch (...) {
    }
    return -1;
  }
}

BackendDefinition TestTensorBackend::MakeDefinition() {
  BackendDefinition def;
  def.backend_type = kBackendType;
  def.description = "Test Tensor Backend Fixture";
  def.supported_protocols = {ExecutionProtocol::kTensorGraph};
  def.concurrency = InferenceConcurrency::kConcurrent;
  return def;
}

void TestTensorBackend::ResetRequestedProtocol() noexcept {
  requested_protocol_.store(-1, std::memory_order_relaxed);
}

std::optional<ExecutionProtocol>
TestTensorBackend::RequestedProtocol() noexcept {
  const int value = requested_protocol_.load(std::memory_order_relaxed);
  if (value < 0) return std::nullopt;
  return static_cast<ExecutionProtocol>(value);
}

std::shared_ptr<IBackendSession> TestTensorBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  (void)diagnostic;
  try {
    requested_protocol_.store(spec.requested_protocol
                                  ? static_cast<int>(*spec.requested_protocol)
                                  : -1,
                              std::memory_order_relaxed);
    return std::make_shared<TestTensorSession>(spec.model_path);
  } catch (...) {
    return nullptr;
  }
}

REGISTER_BACKEND_WITH_DEFINITION(TestTensorBackend,
                                 TestTensorBackend::MakeDefinition());

}  // namespace test
}  // namespace llm_edgeflow
