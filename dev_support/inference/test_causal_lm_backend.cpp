#include "dev_support/inference/test_causal_lm_backend.h"

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

std::atomic<int> TestCausalLmBackend::load_count_{0};

TestCausalLmSession::TestCausalLmSession(std::string model_file,
                                         BatchPolicy policy)
    : model_file_(std::move(model_file)), policy_(policy) {}

int TestCausalLmSession::Generate(const std::string& formatted_prompt,
                                  bool add_bos, const GenerateOptions& options,
                                  std::optional<uint64_t> seed,
                                  std::string* output,
                                  std::string* diagnostic) noexcept {
  (void)add_bos;
  (void)options;
  (void)seed;
  if (!output) {
    if (diagnostic) *diagnostic = "Output pointer is null";
    return -1;
  }
  try {
    output->clear();
    if (formatted_prompt.empty()) {
      if (diagnostic) *diagnostic = "Prompt is empty";
      return -1;
    }
    // 该夹具只验证协议组合与生命周期。
    // 绑定它的业务测试 Model 自行负责确定性的响应语义。
    *output = "test-generation";
    return 0;
  } catch (...) {
    try {
      output->clear();
    } catch (...) {
    }
    return -1;
  }
}

BackendDefinition TestCausalLmBackend::MakeDefinition() {
  BackendDefinition def;
  def.backend_type = kBackendType;
  def.description = "Test Text Generation Backend Fixture";
  def.supported_protocols = {ExecutionProtocol::kFixture};
  def.concurrency = InferenceConcurrency::kSerialized;
  def.params = ParamSpec();
  return def;
}

void TestCausalLmBackend::ResetLoadCount() noexcept { load_count_.store(0); }
int TestCausalLmBackend::LoadCount() noexcept { return load_count_.load(); }

std::shared_ptr<IBackendSession> TestCausalLmBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  load_count_.fetch_add(1);
  if (spec.requested_protocol != ExecutionProtocol::kFixture) {
    if (diagnostic) *diagnostic = "Unsupported requested protocol";
    return nullptr;
  }
  try {
    const auto& params = spec.Params<Params>();
    const size_t fixed = static_cast<size_t>(params.fixed_batch_size);
    return std::make_shared<TestCausalLmSession>(
        spec.model_file, BatchPolicy{fixed == 0 ? 1 : fixed, fixed});
  } catch (...) {
    return nullptr;
  }
}

REGISTER_BACKEND_WITH_DEFINITION(TestCausalLmBackend,
                                 TestCausalLmBackend::MakeDefinition());

}  // namespace test
}  // namespace llm_edgeflow
