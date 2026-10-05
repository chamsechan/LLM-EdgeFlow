#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/io_binding.h"
#include "adapter/io_converter.h"
#include "contracts/registry_conflicts.h"

namespace llm_edgeflow {

// 已声明的条目原样保留；未声明的 Converter 逻辑端口映射到同名键。
// 使用时计算，与注册顺序无关。
std::unordered_map<std::string, std::string> EffectivePortMapping(
    const std::unordered_map<std::string, std::string>& declared,
    const std::vector<NodePortDefinition>& logical_ports);

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

  const IoBindingDefinition* FindBinding(const std::string& binding_id) const;

  std::vector<IoBindingDefinition> AllBindings() const;

  // 一个 biz 通过其全部 binding 标识一份完整的外部契约。
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
  RegistryConflicts conflicts_;
};

}  // namespace llm_edgeflow
