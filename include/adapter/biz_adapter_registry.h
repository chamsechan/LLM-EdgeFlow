#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/biz_adapter_interface.h"
#include "edgeflow/log.h"

namespace llm_edgeflow {

/**
 * @brief 业务适配器全局注册中心 (接入适配层内部)
 */
class BizAdapterRegistry {
 public:
  static BizAdapterRegistry& Instance() {
    static BizAdapterRegistry instance;
    return instance;
  }

  /**
   * @brief 注册业务适配器 (防止多团队业务 ID / 名称冲突覆盖，REV2-003)
   * @return true 注册成功，false 冲突或无效拒绝注册并标记 conflict 状态
   */
  bool RegisterAdapter(std::shared_ptr<IBizAdapter> adapter);

  std::shared_ptr<IBizAdapter> GetAdapter(CompanyAlgBizType biz_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = adapters_.find(biz_type);
    if (it != adapters_.end()) {
      return it->second;
    }
    return nullptr;
  }

  /**
   * @brief 获取线程安全的已注册 Adapter 快照，供跨注册表完整性审计使用
   */
  std::vector<std::shared_ptr<IBizAdapter>> GetAdaptersSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<IBizAdapter>> snapshot;
    snapshot.reserve(adapters_.size());
    for (const auto& [biz_type, adapter] : adapters_) {
      (void)biz_type;
      snapshot.push_back(adapter);
    }
    return snapshot;
  }

  enum class AdapterLookupStatus {
    kSuccess = 0,
    kNotFound = 1,
    kAmbiguousMatch = 2,
  };

  std::shared_ptr<IBizAdapter> GetAdapterByPipelineName(
      const std::string& pipeline_name,
      AdapterLookupStatus* out_status = nullptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<IBizAdapter> matched = nullptr;
    size_t match_count = 0;
    for (const auto& kv : adapters_) {
      if (kv.second && kv.second->ValidatePipelineBinding(pipeline_name)) {
        matched = kv.second;
        match_count++;
      }
    }
    if (match_count == 1) {
      if (out_status) *out_status = AdapterLookupStatus::kSuccess;
      return matched;
    }
    if (match_count > 1) {
      if (out_status) *out_status = AdapterLookupStatus::kAmbiguousMatch;
      return nullptr;  // RECHECK-P1-2: 多个 Adapter 发生白名单冲突时
                       // fail-closed 拦截
    }
    if (out_status) *out_status = AdapterLookupStatus::kNotFound;
    return nullptr;
  }

  std::shared_ptr<IBizAdapter> GetAdapterByName(
      const std::string& adapter_name,
      AdapterLookupStatus* out_status = nullptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<IBizAdapter> matched = nullptr;
    size_t match_count = 0;
    for (const auto& kv : adapters_) {
      if (kv.second && kv.second->AdapterName() == adapter_name) {
        matched = kv.second;
        match_count++;
      }
    }
    if (match_count == 1) {
      if (out_status) *out_status = AdapterLookupStatus::kSuccess;
      return matched;
    }
    if (match_count > 1) {
      if (out_status) *out_status = AdapterLookupStatus::kAmbiguousMatch;
      return nullptr;
    }
    if (out_status) *out_status = AdapterLookupStatus::kNotFound;
    return nullptr;
  }

  size_t AdapterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return adapters_.size();
  }

  bool HasRegistrationConflict() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_conflict_;
  }

  std::vector<std::string> GetRegistrationErrors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registration_errors_;
  }

  void RecordRegistrationError(const std::string& error_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    has_conflict_ = true;
    registration_errors_.push_back(error_msg);
    ALG_LOG_ERROR("[BizAdapterRegistry] %s\n", error_msg.c_str());
  }

  void ClearForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    adapters_.clear();
    has_conflict_ = false;
    registration_errors_.clear();
  }

  void ResetConflictForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    has_conflict_ = false;
    registration_errors_.clear();
  }

 private:
  BizAdapterRegistry() = default;
  mutable std::mutex mutex_;
  bool has_conflict_ = false;
  std::vector<std::string> registration_errors_;
  std::unordered_map<int, std::shared_ptr<IBizAdapter>> adapters_;
};

/**
 * @brief 自动注册宏 (零异常抛出保证，若发生异常记录错误并标记冲突)
 */
#define REGISTER_BIZ_ADAPTER(Class)                                           \
  static bool _registered_adapter_##Class = []() noexcept {                   \
    try {                                                                     \
      auto adapter = std::make_shared<Class>();                               \
      return ::llm_edgeflow::BizAdapterRegistry::Instance().RegisterAdapter(  \
          adapter);                                                           \
    } catch (const std::exception& e) {                                       \
      ::llm_edgeflow::BizAdapterRegistry::Instance().RecordRegistrationError( \
          std::string("Exception registering " #Class ": ") + e.what());      \
      return false;                                                           \
    } catch (...) {                                                           \
      ::llm_edgeflow::BizAdapterRegistry::Instance().RecordRegistrationError( \
          "Unknown exception registering " #Class);                           \
      return false;                                                           \
    }                                                                         \
  }()

}  // namespace llm_edgeflow
