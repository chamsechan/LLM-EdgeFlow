// expect-ok
#include "signature_fixture.h"
struct Outputs {
  TextBatch texts;
  Int32Batch scores;
};
auto Ports() {
  return std::make_pair(InputsContract(),
                        PreservedOutput<TextBatch>("output", "input"));
}
NodeResult<TextBatch> RunInputs(const Inputs& inputs) { return *inputs.input; }
NodeResult<TextBatch> RunSession(const Inputs& inputs,
                                 const SessionResources&) {
  return *inputs.input;
}
NodeResult<TextBatch> RunParams(const Inputs& inputs, const Options&) {
  return *inputs.input;
}
NodeResult<TextBatch> RunModels(const Inputs& inputs, const Models&) {
  return *inputs.input;
}
NodeResult<TextBatch> RunAll(const Inputs& inputs, const Options&,
                             const Models&) {
  return *inputs.input;
}
NodeResult<TextBatch> RunAllSession(const Inputs& inputs, const Options&,
                                    const Models&, const SessionResources&) {
  return *inputs.input;
}
auto InputsSpec() {
  auto [inputs, output] = Ports();
  return MakeNodeSpec(std::move(inputs), std::move(output), &RunInputs);
}
REGISTER_FUNCTION_NODE(ValidInputsNode, InputsSpec());
auto SessionSpec() {
  auto [inputs, output] = Ports();
  return MakeNodeSpec(std::move(inputs), std::move(output), &RunSession);
}
REGISTER_FUNCTION_NODE(ValidSessionNode, SessionSpec());
auto ParamsSpec() {
  auto [inputs, output] = Ports();
  return MakeNodeSpec(std::move(inputs), std::move(output),
                      Parameters<Options>{}, &RunParams);
}
REGISTER_FUNCTION_NODE(ValidParamsNode, ParamsSpec());
auto ModelsSpec() {
  auto [inputs, output] = Ports();
  return MakeNodeSpec(std::move(inputs), std::move(output), ModelContract(),
                      &RunModels);
}
REGISTER_FUNCTION_NODE(ValidModelsNode, ModelsSpec());
REGISTER_FUNCTION_NODE(ValidAllNode, ParamsAndModels(&RunAll));
REGISTER_FUNCTION_NODE(ValidAllSessionNode, ParamsAndModels(&RunAllSession));
auto LambdaSpec() {
  return ParamsAndModels([](const Inputs& inputs, const Options&,
                            const Models&) -> NodeResult<TextBatch> {
    return MapPayloads(*inputs.input,
                       [](const std::string& text) { return text; });
  });
}
REGISTER_FUNCTION_NODE(ValidLambdaNode, LambdaSpec());
auto ValidMultiOutputSpec() {
  return MakeNodeSpec(
      InputsContract(),
      OutputsOf<Outputs>({Produced("texts", &Outputs::texts),
                          Produced("scores", &Outputs::scores)}),
      [](const Inputs&) -> NodeResult<Outputs> { return Outputs{}; });
}
REGISTER_FUNCTION_NODE(ValidMultiOutputNode, ValidMultiOutputSpec());
