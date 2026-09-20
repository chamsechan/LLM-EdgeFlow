#pragma once

#include <string>

#include "core/validated_node_plan.h"

namespace llm_edgeflow {

inline bool ResolveBoundModelId(const ValidatedNodePlan& plan,
                                const std::string& slot_name,
                                const std::string& capability,
                                std::string* model_id, std::string* error) {
  const auto* binding = plan.FindModelBinding(slot_name);
  if (!binding || binding->model_id.empty()) {
    if (error)
      *error =
          "Model binding for '" + slot_name + "' is missing or empty in plan";
    return false;
  }
  if (binding->capability != capability) {
    if (error)
      *error = "Model binding capability mismatch for '" + slot_name + "'";
    return false;
  }
  *model_id = binding->model_id;
  return true;
}

}  // namespace llm_edgeflow
