// expect-error: BuildPrompt must be callable as
#include "signature_fixture.h"

auto Spec() {
  return MakeLlmTextSpec(
      [count = 0](const std::string& text) mutable {
        ++count;
        return text;
      },
      &Text);
}
REGISTER_FUNCTION_NODE(SignatureProbeNode, Spec());
