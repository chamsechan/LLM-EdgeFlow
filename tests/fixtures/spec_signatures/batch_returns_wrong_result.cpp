// expect-error: Batch Run must return NodeResult<OutputBatch>
#include "signature_fixture.h"
NodeResult<Int32Batch> Run(const Inputs&, const Options&, const Models&) {
  return Int32Batch{};
}
auto Spec() { return Batch(&Run); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
