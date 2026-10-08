#pragma once

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "adapter/io_converter.h"
#include "contracts/registry_conflicts.h"

namespace llm_edgeflow {

// converter 按（结构体 type，业务 name）登记，方向各一张表。
class IoConverterRegistry {
 public:
  static IoConverterRegistry& Instance();

  bool RegisterInputConverter(const InputConverterDefinition& def);
  bool RegisterOutputConverter(const OutputConverterDefinition& def);

  const InputConverterDefinition* FindInputConverter(
      const std::string& type, const std::string& name) const;
  const OutputConverterDefinition* FindOutputConverter(
      const std::string& type, const std::string& name) const;

  // 某个 type 下已登记的业务名（字典序）；type 未登记时为空。
  std::vector<std::string> InputNamesOfType(const std::string& type) const;
  std::vector<std::string> OutputNamesOfType(const std::string& type) const;
  // 已登记的 type（字典序、去重），用于未知 type 时给出相近建议。
  std::vector<std::string> InputTypes() const;
  std::vector<std::string> OutputTypes() const;

  std::vector<InputConverterDefinition> AllInputConverters() const;
  std::vector<OutputConverterDefinition> AllOutputConverters() const;

  bool HasConflict() const;
  std::vector<std::string> GetConflictErrors() const;

  /**
   * @brief 全量注册审计：所有登记的冲突、槽与平台结构的一致性、参数声明、
   * 尺寸参数完整性和逻辑端口。
   */
  bool Audit(std::vector<std::string>* out_errors = nullptr) const;

  void ClearForTesting();
  void ResetConflictForTesting();

 private:
  IoConverterRegistry() = default;
  ~IoConverterRegistry() = default;

  using Key = std::pair<std::string, std::string>;  // (type, name)

  mutable std::mutex mutex_;
  std::map<Key, InputConverterDefinition> input_converters_;
  std::map<Key, OutputConverterDefinition> output_converters_;
  RegistryConflicts conflicts_;
};

}  // namespace llm_edgeflow
