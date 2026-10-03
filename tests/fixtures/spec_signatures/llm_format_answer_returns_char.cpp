// expect-error: FormatAnswer must return
#include "signature_fixture.h"
char Wrong(const std::string&) { return 'x'; }
auto Spec() { return MakeLlmTextSpec(&Text, &Wrong); }
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
