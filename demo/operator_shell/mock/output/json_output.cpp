#include "demo/common/demo_io_registry.h"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {
namespace {

void ShowJsonResult(const void* output, const nlohmann::json&,
                    nlohmann::json* sample_output) {
  const auto& out = *static_cast<const MockOutput*>(output);
  const auto* text = out.text;
  if (text && text->length > 0) {
    *sample_output =
        nlohmann::json::parse(text->value, text->value + text->length);
  }
}

REGISTER_DEMO_OUTPUT("MockOutput", ShowJsonResult);

}  // namespace
}  // namespace alg_demo
