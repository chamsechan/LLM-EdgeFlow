// expect-error: Batch Run must return NodeResult<OutputBatch>
#include "signature_fixture.h"
TextBatch Run(const Inputs&, const Options&, const Models&) { return {}; }
auto Spec() { return Batch(&Run); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
