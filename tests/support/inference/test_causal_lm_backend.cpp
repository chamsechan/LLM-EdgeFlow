#include "tests/support/inference/test_causal_lm_backend.h"

#include <utility>

namespace alg_framework {
namespace test {

std::atomic<int> TestCausalLmBackend::load_count_{0};

TestCausalLmSession::TestCausalLmSession(std::string model_path)
    : model_path_(std::move(model_path)) {}

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
    // The fixture proves protocol composition and lifetime only. Business test
    // Models that bind it own their deterministic response semantics.
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
  def.supported_protocols = {ExecutionProtocol::kTextGeneration};
  def.concurrency = InferenceConcurrency::kSerialized;
  return def;
}

void TestCausalLmBackend::ResetLoadCount() noexcept { load_count_.store(0); }
int TestCausalLmBackend::LoadCount() noexcept { return load_count_.load(); }

std::shared_ptr<IBackendSession> TestCausalLmBackend::Load(
    const BackendLoadSpec& spec, std::string* diagnostic) noexcept {
  load_count_.fetch_add(1);
  if (spec.requested_protocol.has_value() &&
      *spec.requested_protocol != ExecutionProtocol::kTextGeneration) {
    if (diagnostic) *diagnostic = "Unsupported requested protocol";
    return nullptr;
  }
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
