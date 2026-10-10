#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "contracts/json_pointer.h"

namespace llm_edgeflow {
struct IoEntryConfig {
  std::string type;
  std::string name;
  nlohmann::json params = nlohmann::json::object();
  std::unordered_map<std::string, std::string> inputs;
};

struct PipelineDocumentSplit {
  std::vector<IoEntryConfig> inputs;
  std::vector<IoEntryConfig> outputs;
  nlohmann::json neutral_pipeline_json;
};

// Integration validates io; all remaining fields are validated by Core.
bool SplitPipelineDocument(const nlohmann::json& root,
                           PipelineDocumentSplit* out_split,
                           std::string* out_error,
                           std::string* out_error_path = nullptr);
}  // namespace llm_edgeflow
