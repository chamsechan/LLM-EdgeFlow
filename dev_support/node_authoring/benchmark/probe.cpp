#include <atomic>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <new>
#include <string>

#include "baseline_starter.cpp"
#include "current_starter.cpp"
#include "nodes/authoring.h"

namespace {
thread_local bool count_allocations = false;
thread_local std::size_t allocations = 0, allocated_bytes = 0;
void* Allocate(std::size_t n) {
  if (void* p = std::malloc(n ? n : 1)) {
    if (count_allocations) {
      ++allocations;
      allocated_bytes += n;
    }
    return p;
  }
  throw std::bad_alloc();
}
}  // namespace
void* operator new(std::size_t n) { return Allocate(n); }
void* operator new[](std::size_t n) { return Allocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace llm_edgeflow;
class EchoModel final : public ILlmModel {
 public:
  std::size_t calls = 0;
  const std::string& ModelType() const noexcept override {
    static const std::string v = "probe_echo";
    return v;
  }
  const std::string& Capability() const noexcept override {
    static const std::string v = "llm";
    return v;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  std::size_t GetMaxBatchSize() const noexcept override { return 32; }
  int Generate(const TextBatch& in, const GenerateOptions&,
               TextBatch* out) noexcept override {
    ++calls;
    *out = in;
    return 0;
  }
};
class ExplicitMap final : public NodeBase {
 public:
  ExplicitMap() : NodeBase("ExplicitMap") {}

 protected:
  int ProcessNode(AlgContext& ctx) override {
    const auto* input = input_.Require(ctx, -1);
    if (!input) return -1;
    TextBatch output;
    output.reserve(input->size());
    for (const auto& item : *input)
      output.emplace_back(item.req_id, item.sub_id, std::string(item.data));
    output_.Set(ctx, std::move(output));
    return 0;
  }

 private:
  BoundInput<TextBatch> input_{"input"};
  BoundOutput<TextBatch> output_{"output"};
};
std::string Identity(const std::string& s) { return s; }
auto ProbeMapSpec() {
  return MakeMapSpec(Input<TextBatch>("input"), Output<TextBatch>("output"),
                     &Identity);
}
struct Inputs {
  const TextBatch* input{};
};
struct Models {
  LlmCall llm;
};
NodeResult<TextBatch> RunBatch(const Inputs& in, const NoParameters&,
                               const Models& models) {
  if (in.input->empty()) return TextBatch{};
  auto prompts = MapPayloads(*in.input, &Identity);
  auto answer = models.llm.Generate(prompts, GenerateOptions{});
  if (!answer.ok()) return answer;
  return MapPayloads(answer.value(), &Identity);
}
NodeResult<TextBatch> RunBatchInPlace(const Inputs& in, const NoParameters&,
                                      const Models& models) {
  if (in.input->empty()) return TextBatch{};
  auto prompts = MapPayloads(*in.input, &Identity);
  auto answer = models.llm.Generate(prompts, GenerateOptions{});
  if (!answer.ok()) return answer;
  auto output = std::move(answer).value();
  for (auto& item : output) item.data = Identity(item.data);
  return output;
}
auto ProbeBatchSpec() {
  return MakeBatchSpec(
      InputsOf<Inputs>({Required("input", &Inputs::input)}),
      PreservedOutput<TextBatch>("output", "input"),
      ModelsOf<Models>({Llm("llm", "bind_model", &Models::llm)}), &RunBatch);
}
auto ProbeBatchInPlaceSpec() {
  return MakeBatchSpec(
      InputsOf<Inputs>({Required("input", &Inputs::input)}),
      PreservedOutput<TextBatch>("output", "input"),
      ModelsOf<Models>({Llm("llm", "bind_model", &Models::llm)}),
      &RunBatchInPlace);
}
REGISTER_FUNCTION_NODE(ProbeBatchInPlace, ProbeBatchInPlaceSpec());
REGISTER_FUNCTION_NODE(ProbeMap, ProbeMapSpec());
REGISTER_FUNCTION_NODE(ProbeBatch, ProbeBatchSpec());
static double CpuNow() {
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}
int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "new_map";
  const int iterations = argc > 2 ? std::atoi(argv[2]) : 10000;
  const int batch_size = 32, length = 1024;
  SessionContext session;
  auto mock = std::make_shared<EchoModel>();
  if (!session.GetModelManager().RegisterModel("echo", mock, "probe-v1"))
    return 2;
  std::unique_ptr<INode> node;
  if (mode == "old_map")
    node = std::make_unique<ExplicitMap>();
  else
    node = NodeRegistry::Instance().Create(
        mode == "new_map"             ? "ProbeMap"
        : mode == "old_llm"           ? "BaselineStarterLlmNode"
        : mode == "new_llm"           ? "ProbeStarterLlmNode"
        : mode == "new_batch_inplace" ? "ProbeBatchInPlace"
                                      : "ProbeBatch");
  nlohmann::json config = nlohmann::json::object();
  if (mode != "old_map" && mode != "new_map") config["bind_model"] = "echo";
  NodeInitContext init;
  init.session_ctx = &session;
  init.config = &config;
  if (!node || !node->Init(init)) {
    std::cerr << "Init failed: " << mode << '\n';
    return 3;
  }
  TextBatch source;
  for (int i = 0; i < batch_size; ++i)
    source.emplace_back(100 + i, i + 3, std::string(length, 'a' + i % 26));
  std::size_t total_allocs = 0, total_bytes = 0, model_calls = 0;
  double seconds = 0;
  for (int i = -100; i < iterations; ++i) {
    AlgContext ctx;
    ctx.Publish("input", source);
    allocations = allocated_bytes = 0;
    const auto before_calls = mock->calls;
    count_allocations = i >= 0;
    const double start = CpuNow();
    const int rc = node->Process(&ctx);
    const double elapsed = CpuNow() - start;
    count_allocations = false;
    if (i >= 0) {
      total_allocs += allocations;
      total_bytes += allocated_bytes;
      seconds += elapsed;
      model_calls += mock->calls - before_calls;
    }
    auto* output = ctx.Read<TextBatch>("output");
    if (rc != 0 || !output || output->size() != source.size()) return 4;
    for (std::size_t j = 0; j < source.size(); ++j)
      if ((*output)[j].req_id != source[j].req_id ||
          (*output)[j].sub_id != source[j].sub_id ||
          (*output)[j].data != source[j].data)
        return 5;
  }
  const bool llm = mode != "old_map" && mode != "new_map";
  if (model_calls != (llm ? static_cast<std::size_t>(iterations) : 0)) return 6;
  std::cout << "{\"mode\":\"" << mode << "\",\"iterations\":" << iterations
            << ",\"batch_size\":" << batch_size << ",\"text_bytes\":" << length
            << ",\"cpu_us_per_process\":" << seconds * 1e6 / iterations
            << ",\"new_calls_per_process\":"
            << double(total_allocs) / iterations
            << ",\"new_bytes_per_process\":" << double(total_bytes) / iterations
            << ",\"model_calls_per_process\":"
            << double(model_calls) / iterations << ",\"outputs_equal\":true}\n";
}
