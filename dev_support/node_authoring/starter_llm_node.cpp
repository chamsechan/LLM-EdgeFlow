#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace {

// LLM starter: see doc/dev_guide/first_custom_node.md for the authoring
// walkthrough.
// Start here: these two functions work on text, not platform structures.
static std::string BuildPrompt(const std::string& text) { return text; }

static std::string FormatAnswer(const std::string& text) { return text; }

auto StarterLlmSpec() {
  return MakeLlmTextSpec(Input<TextBatch>("input"), Output<TextBatch>("output"),
                         &BuildPrompt, &FormatAnswer)
      .Description("LLM authoring starter");
}

REGISTER_FUNCTION_NODE(StarterLlmNode, StarterLlmSpec());

}  // namespace
}  // namespace custom_nodes
}  // namespace llm_edgeflow
