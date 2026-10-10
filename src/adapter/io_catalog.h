#pragma once

#include <nlohmann/json.hpp>

#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
class IoCatalog {
 public:
  static nlohmann::json ToJson(const PipelineCatalogSnapshot& snapshot);
  static nlohmann::json ToJson();
};
}  // namespace llm_edgeflow
