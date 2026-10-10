#include "dev_support/inference/test_tensor_backend.h"

#include <utility>

#include "contracts/parameters.h"

namespace llm_edgeflow {
namespace test {
namespace {

struct Params {
  int fixed_batch_size = 0;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>(
      {Field("fixed_batch_size", &Params::fixed_batch_size)
           .Default(0)
           .Range(0, 16)
           .Description("固定批大小，0 表示动态批处理")});
}

}  // namespace

std::atomic<int> TestTensorBackend::requested_protocol_{-1};

TestTensorSession::TestTensorSession(std::string model_file, BatchPolicy policy)
    : model_file_(std::move(model_file)), policy_(policy) {
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
  def.supported_protocols = {ExecutionProtocol::kFixture};
  def.concurrency = InferenceConcurrency::kConcurrent;
  def.params = ParamSpec();
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
  try {
    const auto& params = spec.Params<Params>();
    requested_protocol_.store(static_cast<int>(spec.requested_protocol),
                              std::memory_order_relaxed);
    if (spec.requested_protocol != ExecutionProtocol::kFixture) {
      if (diagnostic) *diagnostic = "Unsupported requested protocol";
      return nullptr;
    }
    const size_t fixed = static_cast<size_t>(params.fixed_batch_size);
    return std::make_shared<TestTensorSession>(
        spec.model_file, BatchPolicy{fixed == 0 ? 16 : fixed, fixed});
  } catch (...) {
    return nullptr;
  }
}

REGISTER_BACKEND_WITH_DEFINITION(TestTensorBackend,
                                 TestTensorBackend::MakeDefinition());

}  // namespace test
}  // namespace llm_edgeflow
