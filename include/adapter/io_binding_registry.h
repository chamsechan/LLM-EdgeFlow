#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/io_binding.h"
#include "adapter/io_converter.h"

namespace llm_edgeflow {

// Declared entries are kept as written; converter logical ports without an
// entry map to the same key. Computed on use, independent of registration
// order.
std::unordered_map<std::string, std::string> EffectivePortMapping(
    const std::unordered_map<std::string, std::string>& declared,
    const std::vector<NodePortDefinition>& logical_ports);

// Output slots with no capacity_fields inherit the string capacity fields of
// their registered ValueType, in lexicographic order. Input slots and unknown
// ValueTypes return the declared list unchanged.
std::vector<std::string> EffectiveCapacityFields(
    const ExternalSlotDefinition& slot);

// Smallest positive limit among the binding and its converters; zero when none
// of them declares one.
size_t EffectiveMaxBatchSize(const IoBindingDefinition& binding,
                             const InputConverterDefinition& input,
                             const OutputConverterDefinition& output);

class IoBindingRegistry {
 public:
  static IoBindingRegistry& Instance();

  bool RegisterBinding(const IoBindingDefinition& def);

  const IoBindingDefinition* FindBinding(const std::string& binding_id) const;

  std::vector<IoBindingDefinition> AllBindings() const;

  // A biz identifies one complete external contract across its bindings.
  bool ValidateBizContract(const std::string& biz_name,
                           std::string* error = nullptr) const;

  bool HasConflict() const;
  std::vector<std::string> GetConflictErrors() const;

  /**
   * @brief 全量接入审计 (验证绑定、转换器、业务契约与批次上限的完整性)
   */
  bool Audit(std::vector<std::string>* out_errors = nullptr) const;

  void ClearForTesting();
  void ResetConflictForTesting();

 private:
  IoBindingRegistry() = default;
  ~IoBindingRegistry() = default;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, IoBindingDefinition> bindings_;
  std::vector<std::string> conflict_errors_;
};

}  // namespace llm_edgeflow
