#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/io_converter.h"

namespace llm_edgeflow {

class IoConverterRegistry {
 public:
  static IoConverterRegistry& Instance();

  bool RegisterInputConverter(const InputConverterDefinition& def);
  bool RegisterOutputConverter(const OutputConverterDefinition& def);

  const InputConverterDefinition* FindInputConverter(
      const std::string& converter_id) const;
  const OutputConverterDefinition* FindOutputConverter(
      const std::string& converter_id) const;

  std::vector<InputConverterDefinition> AllInputConverters() const;
  std::vector<OutputConverterDefinition> AllOutputConverters() const;

  bool HasConflict() const;
  std::vector<std::string> GetConflictErrors() const;

  void ClearForTesting();

 private:
  IoConverterRegistry() = default;
  ~IoConverterRegistry() = default;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, InputConverterDefinition> input_converters_;
  std::unordered_map<std::string, OutputConverterDefinition> output_converters_;
  std::vector<std::string> conflict_errors_;
};

}  // namespace llm_edgeflow
