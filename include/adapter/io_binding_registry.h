#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/io_binding.h"
#include "adapter/io_converter.h"
#include "contracts/registry_conflicts.h"

namespace llm_edgeflow {

// 输出槽位按字典序返回其已注册 ValueType 的字符串容量字段；
// 输入槽位和未知 ValueType 返回空列表。
std::vector<std::string> EffectiveCapacityFields(
    const ExternalSlotDefinition& slot);

// binding 及其 Converter 中最小的正上限；均未声明时为 0。
size_t EffectiveMaxBatchSize(const IoBindingDefinition& binding,
                             const InputConverterDefinition& input,
                             const OutputConverterDefinition& output);

class IoBindingRegistry {
 public:
  static IoBindingRegistry& Instance();

  bool RegisterBinding(const IoBindingDefinition& def);

  // Pipeline 的 deployment.io.io_binding 填写业务名，按其查找唯一的 binding。
  const IoBindingDefinition* FindBinding(const std::string& biz_name) const;

  std::vector<IoBindingDefinition> AllBindings() const;

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
  std::unordered_map<std::string, IoBindingDefinition>
      bindings_;  // 按 biz_name
  RegistryConflicts conflicts_;
};

}  // namespace llm_edgeflow
