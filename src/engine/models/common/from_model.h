#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "edgeflow/log.h"

namespace llm_edgeflow {

inline bool ResolveFromModel(const char* name,
                             std::optional<int64_t> configured,
                             std::optional<int64_t> detected,
                             std::optional<int64_t> fallback, int64_t* value,
                             std::string* diagnostic) {
  if (configured && detected && configured != detected) {
    if (diagnostic) {
      *diagnostic = std::string("Configured '") + name + "' value " +
                    std::to_string(*configured) +
                    " does not match model value " + std::to_string(*detected);
    }
    return false;
  }
  const auto resolved =
      configured ? configured : (detected ? detected : fallback);
  if (!resolved) {
    if (diagnostic) {
      *diagnostic = std::string("Cannot read '") + name +
                    "' from model; specify it in model_config";
    }
    return false;
  }
  *value = *resolved;
  ALG_LOG_INFO("[Model] %s=%lld\n", name, static_cast<long long>(*value));
  return true;
}

}  // namespace llm_edgeflow
