// expect-error: FormatAnswer must be callable as
#include "signature_fixture.h"
std::string Wrong(std::string&) { return {}; }
auto Spec() { return MakeLlmTextSpec(&Text, &Wrong); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
