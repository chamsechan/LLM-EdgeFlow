#pragma once

#include <string>

namespace llm_edgeflow {

// Select a field of one output's configuration. These are framework fields,
// distinct from the structure-specific enums inside kParameters.
enum class OutputConfigField {
  kType,
  kAllocator,
  kParameters,
  kCapacities,
  kMetadataCount,
  kMetadataTypeId
};

// Creation-time Integration component, independent of Pipeline execution.
// The JSON implementation returns serialized JSON values (including quotes for
// strings); other configuration carriers can implement the same text boundary.
// Structure allocators receive only the resulting parameter text.
class OutputConfigReader {
 public:
  virtual ~OutputConfigReader() = default;
  virtual bool Read(OutputConfigField field, std::string* text,
                    std::string* error) const noexcept = 0;
};

}  // namespace llm_edgeflow
