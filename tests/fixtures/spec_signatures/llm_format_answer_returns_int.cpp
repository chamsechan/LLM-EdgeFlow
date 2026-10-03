// expect-error: FormatAnswer must return
#include "signature_fixture.h"
int Wrong(const std::string&) { return 42; }
auto Spec() { return MakeLlmTextSpec(&Text, &Wrong); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
