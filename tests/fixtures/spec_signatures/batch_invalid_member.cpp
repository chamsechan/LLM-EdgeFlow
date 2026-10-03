// expect-error: Batch Run must be callable as one of
#include "signature_fixture.h"
struct Logic {
  NodeResult<TextBatch> Run(Inputs&, const Options&) const {
    return TextBatch{};
  }
};
auto Spec() { return Batch(&Logic::Run); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
