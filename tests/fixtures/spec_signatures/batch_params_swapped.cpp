// expect-error: Batch Run must be callable as one of
#include "signature_fixture.h"
NodeResult<TextBatch> Run(const Inputs&, const Models&, const Options&) {
  return TextBatch{};
}
auto Spec() { return Batch(&Run); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
