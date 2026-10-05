#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace {

// LLM 入门模板：编写流程见 doc/dev_guide/first_custom_node.md。
// 从这里开始：这两个函数处理文本，而非平台结构。
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
