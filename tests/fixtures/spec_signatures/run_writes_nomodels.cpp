// expect-error: Node Run must be callable as
#include "signature_fixture.h"
NodeResult<TextBatch> Run(const Inputs&, const Options&, const NoModels&) {
  return TextBatch{};
}
auto Spec() {
  return MakeNodeSpec(InputsContract(),
                      PreservedOutput<TextBatch>("output", "input"),
                      Parameters<Options>{}, &Run);
}
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
