// expect-error: Node Run must be callable as
#include "signature_fixture.h"
struct Logic {
  NodeResult<TextBatch> Run(const Inputs&, const Options&,
                            const Models&) const {
    return TextBatch{};
  }
};
auto Spec() { return ParamsAndModels(&Logic::Run); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
