#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace StarterControlNode_impl {

struct Inputs {
  const TextBatch* input = nullptr;
};

struct Params {
  std::string prefix;
};

// 根据当前 Catalog 选择一个稳定且未被占用的自定义 ID。
inline constexpr int kUpdatePrefix = 1001;

// 业务逻辑处理普通数据，而非平台结构。
NodeResult<TextBatch> Run(const Inputs& inputs, const Params& params) {
  return MapPayloads(*inputs.input, [&params](const std::string& text) {
    return params.prefix + text;
  });
}

auto Spec() {
  return MakeNodeSpec(
             InputsOf<Inputs>{Required("input", &Inputs::input)},
             PreservedOutput<TextBatch>("output", "input"),
             Parameters<Params>(
                 {
                     Field("prefix", &Params::prefix)
                         .Default("")
                         .Description(
                             "Text prepended to each input; at most 64 UTF-8 "
                             "bytes. Control replaces this initial value."),
                 })
                 .Validate([](const Params& params, std::string* diagnostic) {
                   if (params.prefix.size() > 64) {
                     if (diagnostic) {
                       *diagnostic = "prefix exceeds 64 UTF-8 bytes";
                     }
                     return false;
                   }
                   return true;
                 }),
             &Run)
      .Description("Control authoring starter")
      .WithControls({
          ReplaceFields(kUpdatePrefix, "set_prefix", {"prefix"},
                        "Replace the text prefix (at most 64 bytes)"),
      });
}

REGISTER_FUNCTION_NODE(StarterControlNode, Spec());

}  // namespace StarterControlNode_impl
}  // namespace custom_nodes
}  // namespace llm_edgeflow
