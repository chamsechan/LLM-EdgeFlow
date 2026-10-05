// expect-error: Node Run must be callable as
#include "signature_fixture.h"
NodeResult<TextBatch> Run(Inputs&, const Options&, const Models&) {
  return TextBatch{};
}
auto Spec() { return ParamsAndModels(&Run); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
