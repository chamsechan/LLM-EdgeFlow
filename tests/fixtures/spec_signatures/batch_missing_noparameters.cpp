// expect-error: Batch Run must be callable as one of
#include "signature_fixture.h"
NodeResult<TextBatch> Run(const Inputs&, const Models&) { return TextBatch{}; }
auto Spec() {
  return MakeBatchSpec(InputsContract(),
                       PreservedOutput<TextBatch>("output", "input"),
                       ModelContract(), &Run);
}
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
