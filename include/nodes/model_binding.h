#pragma once

#include <string>

#include "core/validated_node_plan.h"

namespace llm_edgeflow {

inline bool ResolveBoundModelName(const ValidatedNodePlan& plan,
                                  const std::string& slot_name,
                                  const std::string& model_type,
                                  std::string* model_name, std::string* error) {
  const auto* binding = plan.FindModelBinding(slot_name);
  if (!binding || binding->model_name.empty()) {
    if (error)
      *error =
          "Model binding for '" + slot_name + "' is missing or empty in plan";
    return false;
  }
  if (binding->model_type != model_type) {
    if (error)
      *error = "Model binding model_type mismatch for '" + slot_name + "'";
    return false;
  }
  *model_name = binding->model_name;
  return true;
}

}  // namespace llm_edgeflow
