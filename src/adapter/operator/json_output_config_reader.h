#pragma once

#include "adapter/operator_output_config.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {

// Borrows one output object only for the resolver's Create-time work. Returned
// strings own their storage; the document never enters the output pools.
class JsonOutputConfigReader final : public OutputConfigReader {
 public:
  explicit JsonOutputConfigReader(const nlohmann::json& config)
      : config_(config) {}
  JsonOutputConfigReader(nlohmann::json&&) = delete;

  bool Read(OutputConfigField field, std::string* text,
            std::string* error) const noexcept override;

 private:
  const nlohmann::json& config_;
};

}  // namespace llm_edgeflow
