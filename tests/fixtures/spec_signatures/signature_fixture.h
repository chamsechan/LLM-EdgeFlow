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
  return ModelsOf<Models>(
      {Model("generator", "bind_model", &Models::generator)});
}
template <typename Fn>
auto ParamsAndModels(Fn fn) {
  return MakeNodeSpec(InputsContract(),
                      PreservedOutput<TextBatch>("output", "input"),
                      Parameters<Options>{}, ModelContract(), fn);
}
}  // namespace signature_fixture
using namespace signature_fixture;
