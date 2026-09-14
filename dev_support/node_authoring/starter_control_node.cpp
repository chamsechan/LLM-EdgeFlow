#include <string>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace custom_nodes {
namespace StarterControlNode_impl {

struct StarterControlNodeParams {
  std::string prefix;
};

// Choose a stable, unused custom ID using the current Catalog.
inline constexpr int kUpdatePrefix = 1001;

// Business logic works on ordinary data, not platform structures.
static std::string ApplyPrefix(const std::string& input,
                               const StarterControlNodeParams& params) {
  return params.prefix + input;
}

auto StarterControlNodeSpec() {
  return MakeMapSpec(
             Input<TextBatch>("input"), Output<TextBatch>("output"),
             Parameters<StarterControlNodeParams>(
                 {
                     Field("prefix", &StarterControlNodeParams::prefix)
                         .Default("")
                         .Description(
                             "Text prepended to each input; at most 64 UTF-8 "
                             "bytes. Control replaces this initial value."),
                 })
                 .Validate([](const StarterControlNodeParams& params,
                              std::string* diagnostic) {
                   if (params.prefix.size() > 64) {
                     if (diagnostic) {
                       *diagnostic = "prefix exceeds 64 UTF-8 bytes";
                     }
                     return false;
                   }
                   return true;
                 }),
             &ApplyPrefix)
      .Description("Control authoring starter")
      .WithControls({
          ReplaceFields(kUpdatePrefix, "set_prefix", {"prefix"},
                        "Replace the text prefix (at most 64 bytes)"),
      });
}

REGISTER_FUNCTION_NODE(StarterControlNode, StarterControlNodeSpec());

}  // namespace StarterControlNode_impl
}  // namespace custom_nodes
}  // namespace llm_edgeflow
