#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/io_binding.h"

namespace llm_edgeflow {

class IoBindingRegistry {
 public:
  static IoBindingRegistry& Instance();

  bool RegisterBinding(const IoBindingDefinition& def);
  bool RegisterExposure(const BizExposureDefinition& def);

  const IoBindingDefinition* FindBinding(const std::string& binding_id) const;
  const BizExposureDefinition* FindExposure(const std::string& biz_name) const;

  std::vector<IoBindingDefinition> AllBindings() const;
  std::vector<BizExposureDefinition> AllExposures() const;

  bool HasConflict() const;
  std::vector<std::string> GetConflictErrors() const;

  /**
   * @brief 全量接入审计 (验证绑定、转换器、业务契约完整性与生产暴露能力)
   */
  bool Audit(std::vector<std::string>* out_errors = nullptr) const;

  void ClearForTesting();

 private:
  IoBindingRegistry() = default;
  ~IoBindingRegistry() = default;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, IoBindingDefinition> bindings_;
  std::unordered_map<std::string, BizExposureDefinition> exposures_;
  std::vector<std::string> conflict_errors_;
};

}  // namespace llm_edgeflow
