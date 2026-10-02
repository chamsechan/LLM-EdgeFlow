// expect-error: BuildPrompt must return
#include "signature_fixture.h"
int Wrong(const std::string&) { return 42; }
auto Spec() { return MakeLlmTextSpec(&Wrong, &Text); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
