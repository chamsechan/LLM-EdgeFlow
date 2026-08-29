#include "tests/support/inference/test_tensor_backend.h"

#include <utility>

namespace alg_framework {
namespace test {

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

std::shared_ptr<IBackendSession> TestTensorBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  (void)diagnostic;
  try {
    return std::make_shared<TestTensorSession>(spec.model_path);
  } catch (...) {
    return nullptr;
  }
}

REGISTER_BACKEND_WITH_DEFINITION(TestTensorBackend,
                                 TestTensorBackend::MakeDefinition());

}  // namespace test
}  // namespace alg_framework
