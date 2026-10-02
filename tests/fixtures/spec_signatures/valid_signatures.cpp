// expect-ok
#include <string_view>

#include "signature_fixture.h"
struct ImplicitText {
  operator std::string() const { return "text"; }
};
struct Outputs {
  TextBatch texts;
  Int32Batch scores;
};
NodeResult<TextBatch> Run2(const Inputs& inputs, const NoParameters&) {
  return *inputs.input;
}
NodeResult<TextBatch> Run3(const Inputs& inputs, const NoParameters&,
                           const NoModels&) {
  return *inputs.input;
}
NodeResult<TextBatch> Run4(const Inputs& inputs, const NoParameters&,
                           const NoModels&, const SessionResources&) {
  return *inputs.input;
}
struct Logic {
  NodeResult<TextBatch> Run2(const Inputs& i, const NoParameters&) {
    return *i.input;
  }
  NodeResult<TextBatch> Run3(const Inputs& i, const NoParameters&,
                             const NoModels&) {
    return *i.input;
  }
  NodeResult<TextBatch> Run4(const Inputs& i, const NoParameters&,
                             const NoModels&, const SessionResources&) {
    return *i.input;
  }
  NodeResult<TextBatch> Const2(const Inputs& i, const NoParameters&) const {
    return *i.input;
  }
  NodeResult<TextBatch> Const3(const Inputs& i, const NoParameters&,
                               const NoModels&) const {
    return *i.input;
  }
  NodeResult<TextBatch> Const4(const Inputs& i, const NoParameters&,
                               const NoModels&, const SessionResources&) const {
    return *i.input;
  }
};
template <typename Fn>
auto NoParamsBatch(Fn fn) {
  return MakeBatchSpec(InputsContract(),
                       PreservedOutput<TextBatch>("output", "input"), fn);
}
REGISTER_FUNCTION_NODE(ValidFree2Node, NoParamsBatch(&Run2));
REGISTER_FUNCTION_NODE(ValidFree3Node, NoParamsBatch(&Run3));
REGISTER_FUNCTION_NODE(ValidFree4Node, NoParamsBatch(&Run4));
REGISTER_FUNCTION_NODE(ValidMember2Node, NoParamsBatch(&Logic::Run2));
REGISTER_FUNCTION_NODE(ValidMember3Node, NoParamsBatch(&Logic::Run3));
REGISTER_FUNCTION_NODE(ValidMember4Node, NoParamsBatch(&Logic::Run4));
REGISTER_FUNCTION_NODE(ValidConstMember2Node, NoParamsBatch(&Logic::Const2));
REGISTER_FUNCTION_NODE(ValidConstMember3Node, NoParamsBatch(&Logic::Const3));
REGISTER_FUNCTION_NODE(ValidConstMember4Node, NoParamsBatch(&Logic::Const4));
auto ValidMapSpec() {
  return MakeMapSpec(Input<TextBatch>("input"), Output<TextBatch>("output"),
                     &Text);
}
REGISTER_FUNCTION_NODE(ValidMapNode, ValidMapSpec());
auto ValidMultiOutputSpec() {
  return MakeBatchSpec(
      InputsContract(),
      OutputsOf<Outputs>({Produced("texts", &Outputs::texts),
                          Produced("scores", &Outputs::scores)}),
      [](const Inputs&, const NoParameters&) -> NodeResult<Outputs> {
        return Outputs{};
      });
}
REGISTER_FUNCTION_NODE(ValidMultiOutputNode, ValidMultiOutputSpec());
std::string HookString(const std::string& text) { return text; }
std::string ParameterHookString(const std::string& text, const Options&) {
  return text;
}
auto LlmStringSpec() { return MakeLlmTextSpec(&HookString, &HookString); }
REGISTER_FUNCTION_NODE(ValidLlmStringNode, LlmStringSpec());
auto ParameterLlmStringSpec() {
  return MakeLlmTextSpec(Parameters<Options>{}, &ParameterHookString,
                         &ParameterHookString);
}
REGISTER_FUNCTION_NODE(ValidParameterLlmStringNode, ParameterLlmStringSpec());
const char* HookPointer(const std::string& text) { return "text"; }
const char* ParameterHookPointer(const std::string& text, const Options&) {
  return "text";
}
auto LlmPointerSpec() { return MakeLlmTextSpec(&HookPointer, &HookPointer); }
REGISTER_FUNCTION_NODE(ValidLlmPointerNode, LlmPointerSpec());
auto ParameterLlmPointerSpec() {
  return MakeLlmTextSpec(Parameters<Options>{}, &ParameterHookPointer,
                         &ParameterHookPointer);
}
REGISTER_FUNCTION_NODE(ValidParameterLlmPointerNode, ParameterLlmPointerSpec());
std::string_view HookView(const std::string& text) { return text; }
std::string_view ParameterHookView(const std::string& text, const Options&) {
  return text;
}
auto LlmViewSpec() { return MakeLlmTextSpec(&HookView, &HookView); }
REGISTER_FUNCTION_NODE(ValidLlmViewNode, LlmViewSpec());
auto ParameterLlmViewSpec() {
  return MakeLlmTextSpec(Parameters<Options>{}, &ParameterHookView,
                         &ParameterHookView);
}
REGISTER_FUNCTION_NODE(ValidParameterLlmViewNode, ParameterLlmViewSpec());
NodeResult<std::string> HookResultString(const std::string& text) {
  return text;
}
NodeResult<std::string> ParameterHookResultString(const std::string& text,
                                                  const Options&) {
  return text;
}
auto LlmResultStringSpec() {
  return MakeLlmTextSpec(&HookResultString, &HookResultString);
}
REGISTER_FUNCTION_NODE(ValidLlmResultStringNode, LlmResultStringSpec());
auto ParameterLlmResultStringSpec() {
  return MakeLlmTextSpec(Parameters<Options>{}, &ParameterHookResultString,
                         &ParameterHookResultString);
}
REGISTER_FUNCTION_NODE(ValidParameterLlmResultStringNode,
                       ParameterLlmResultStringSpec());
NodeResult<const char*> HookResultPointer(const std::string& text) {
  return "text";
}
NodeResult<const char*> ParameterHookResultPointer(const std::string& text,
                                                   const Options&) {
  return "text";
}
auto LlmResultPointerSpec() {
  return MakeLlmTextSpec(&HookResultPointer, &HookResultPointer);
}
REGISTER_FUNCTION_NODE(ValidLlmResultPointerNode, LlmResultPointerSpec());
auto ParameterLlmResultPointerSpec() {
  return MakeLlmTextSpec(Parameters<Options>{}, &ParameterHookResultPointer,
                         &ParameterHookResultPointer);
}
REGISTER_FUNCTION_NODE(ValidParameterLlmResultPointerNode,
                       ParameterLlmResultPointerSpec());
NodeResult<std::string_view> HookResultView(const std::string& text) {
  return std::string_view(text);
}
NodeResult<std::string_view> ParameterHookResultView(const std::string& text,
                                                     const Options&) {
  return std::string_view(text);
}
auto LlmResultViewSpec() {
  return MakeLlmTextSpec(&HookResultView, &HookResultView);
}
REGISTER_FUNCTION_NODE(ValidLlmResultViewNode, LlmResultViewSpec());
auto ParameterLlmResultViewSpec() {
  return MakeLlmTextSpec(Parameters<Options>{}, &ParameterHookResultView,
                         &ParameterHookResultView);
}
REGISTER_FUNCTION_NODE(ValidParameterLlmResultViewNode,
                       ParameterLlmResultViewSpec());
ImplicitText HookImplicit(const std::string& text) { return {}; }
ImplicitText ParameterHookImplicit(const std::string& text, const Options&) {
  return {};
}
auto LlmImplicitSpec() { return MakeLlmTextSpec(&HookImplicit, &HookImplicit); }
REGISTER_FUNCTION_NODE(ValidLlmImplicitNode, LlmImplicitSpec());
auto ParameterLlmImplicitSpec() {
  return MakeLlmTextSpec(Parameters<Options>{}, &ParameterHookImplicit,
                         &ParameterHookImplicit);
}
REGISTER_FUNCTION_NODE(ValidParameterLlmImplicitNode,
                       ParameterLlmImplicitSpec());
NodeResult<ImplicitText> HookResultImplicit(const std::string& text) {
  return ImplicitText{};
}
NodeResult<ImplicitText> ParameterHookResultImplicit(const std::string& text,
                                                     const Options&) {
  return ImplicitText{};
}
auto LlmResultImplicitSpec() {
  return MakeLlmTextSpec(&HookResultImplicit, &HookResultImplicit);
}
REGISTER_FUNCTION_NODE(ValidLlmResultImplicitNode, LlmResultImplicitSpec());
auto ParameterLlmResultImplicitSpec() {
  return MakeLlmTextSpec(Parameters<Options>{}, &ParameterHookResultImplicit,
                         &ParameterHookResultImplicit);
}
REGISTER_FUNCTION_NODE(ValidParameterLlmResultImplicitNode,
                       ParameterLlmResultImplicitSpec());
auto ValidByValueSpec() {
  return MakeLlmTextSpec([](std::string text) { return text; },
                         [](std::string text) { return text; });
}
REGISTER_FUNCTION_NODE(ValidByValueNode, ValidByValueSpec());
