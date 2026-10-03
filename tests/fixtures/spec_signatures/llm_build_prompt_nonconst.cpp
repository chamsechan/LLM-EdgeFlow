// expect-error: BuildPrompt must be callable as
#include "signature_fixture.h"
std::string Wrong(std::string&) { return {}; }
auto Spec() { return MakeLlmTextSpec(&Wrong, &Text); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
