#pragma once
#include "nodes/authoring.h"
using namespace llm_edgeflow;
namespace signature_fixture {
struct Inputs {
  const TextBatch* input = nullptr;
};
struct Options {
  bool flag = false;
};
struct Models {
  LlmCall generator;
};
inline auto InputsContract() {
  return InputsOf<Inputs>({Required("input", &Inputs::input)});
}
inline auto ModelContract() {
  return ModelsOf<Models>({Llm("generator", "bind_model", &Models::generator)});
}
template <typename Fn>
auto Batch(Fn fn) {
  return MakeBatchSpec(InputsContract(),
                       PreservedOutput<TextBatch>("output", "input"),
                       Parameters<Options>{}, ModelContract(), fn);
}
inline std::string Text(const std::string& text) { return text; }
}  // namespace signature_fixture
using namespace signature_fixture;
