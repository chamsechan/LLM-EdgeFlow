// expect-error: Node Run must be callable as
#include "signature_fixture.h"
NodeResult<TextBatch> Run(const Inputs&, const NoParameters&, const Models&) {
  return TextBatch{};
}
auto Spec() {
  return MakeNodeSpec(InputsContract(),
                      PreservedOutput<TextBatch>("output", "input"),
                      ModelContract(), &Run);
}
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
