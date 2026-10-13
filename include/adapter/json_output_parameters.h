#pragma once

#include <cstdint>

#include "adapter/converter_authoring.h"

namespace llm_edgeflow {

struct JsonOutputParams {
  int64_t result_json_max_bytes = 0;
};

inline Parameters<JsonOutputParams> JsonOutputParameters() {
  return Parameters<JsonOutputParams>({
      MaxBytes("result_json", &JsonOutputParams::result_json_max_bytes)
          .Default(8191),
  });
}

}  // namespace llm_edgeflow
