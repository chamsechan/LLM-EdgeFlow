#pragma once

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "adapter/io_converter.h"
#include "contracts/registry_conflicts.h"

namespace llm_edgeflow {

struct OperatorValueTypeBinding;

// Size fields inherit the platform hard maximum for validation and tools.
std::vector<ConfigFieldDefinition> OutputConverterParameterFields(
    const OutputConverterDefinition& definition,
    const OperatorValueTypeBinding* allocator = nullptr);

class IoConverterRegistry {
 public:
  static IoConverterRegistry& Instance();

  bool RegisterInputConverter(const InputConverterDefinition& def);
  bool RegisterOutputConverter(const OutputConverterDefinition& def);

  const InputConverterDefinition* FindInputConverter(
      const std::string& type, const std::string& name) const;
  const OutputConverterDefinition* FindOutputConverter(
      const std::string& type, const std::string& name) const;

  std::vector<InputConverterDefinition> AllInputConverters() const;
  std::vector<OutputConverterDefinition> AllOutputConverters() const;

  bool HasConflict() const;
  std::vector<std::string> GetConflictErrors() const;

  bool Audit(std::vector<std::string>* out_errors = nullptr) const;
  std::shared_ptr<const OutputAllocationParameters> OutputParameters(
      const std::string& type, const std::string& name) const;

  void ClearForTesting();
  void ResetConflictForTesting();

 private:
  IoConverterRegistry() = default;
  ~IoConverterRegistry() = default;

  mutable std::mutex mutex_;
  mutable std::mutex audit_mutex_;
  using Key = std::pair<std::string, std::string>;
  std::map<Key, InputConverterDefinition> input_converters_;
  std::map<Key, OutputConverterDefinition> output_converters_;
  mutable std::map<Key, std::shared_ptr<const OutputAllocationParameters>>
      output_parameters_;
  RegistryConflicts conflicts_;
};

}  // namespace llm_edgeflow
