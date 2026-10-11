#pragma once

#include <nlohmann/json.hpp>

#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow::test_support {

inline nlohmann::json ParseJsonOutput(const CompanyOperatorOutput* output) {
  return nlohmann::json::parse(
      output->result_json->data,
      output->result_json->data + output->result_json->length);
}

}  // namespace llm_edgeflow::test_support
