#pragma once

#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/business_adapter_interface.h"

namespace alg_framework {

/**
 * @brief 业务适配器全局注册中心 (Layer 1 内部)
 */
class BusinessAdapterRegistry {
 public:
  static BusinessAdapterRegistry& Instance() {
    static BusinessAdapterRegistry instance;
    return instance;
  }

  /**
   * @brief 注册业务适配器 (防止多团队业务 ID / 名称冲突覆盖，REV2-003)
   * @return true 注册成功，false 冲突或无效拒绝注册并标记 conflict 状态
   */
  bool RegisterAdapter(std::shared_ptr<IBusinessAdapter> adapter) {
    if (!adapter) {
      std::lock_guard<std::mutex> lock(mutex_);
      has_conflict_ = true;
      registration_errors_.push_back("Null adapter pointer passed to registry");
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    CompanyAlgBizType biz_type = adapter->BizType();

    if (biz_type == ALG_BIZ_TYPE_UNKNOWN) {
      has_conflict_ = true;
      std::string err = "Cannot register adapter '" +
                        std::string(adapter->BizName()) +
                        "' with ALG_BIZ_TYPE_UNKNOWN";
      registration_errors_.push_back(err);
      std::cerr << "[BusinessAdapterRegistry ERROR] " << err << std::endl;
      return false;
    }

    // 冲突与契约检查 0: 必须满足框架当前可执行的 Descriptor 策略 (RECHECK-003,
    // ADP-008)
    const auto& desc = adapter->GetDescriptor();
    if (desc.ownership_policy != OwnershipPolicy::kCopyIn) {
      has_conflict_ = true;
      std::string err = "Unsupported OwnershipPolicy in adapter '" +
                        std::string(adapter->BizName()) +
                        "': only kCopyIn is currently supported";
      registration_errors_.push_back(err);
      std::cerr << "[BusinessAdapterRegistry ERROR] " << err << std::endl;
      return false;
    }
    if (desc.thread_model != ThreadModel::kStatelessThreadSafe) {
      has_conflict_ = true;
      std::string err = "Unsupported ThreadModel in adapter '" +
                        std::string(adapter->BizName()) +
                        "': only kStatelessThreadSafe is currently supported";
      registration_errors_.push_back(err);
      std::cerr << "[BusinessAdapterRegistry ERROR] " << err << std::endl;
      return false;
    }
    if (desc.cardinality != OutputCardinality::kOneToOne) {
      has_conflict_ = true;
      std::string err = "Unsupported OutputCardinality in adapter '" +
                        std::string(adapter->BizName()) +
                        "': only kOneToOne is currently supported";
      registration_errors_.push_back(err);
      std::cerr << "[BusinessAdapterRegistry ERROR] " << err << std::endl;
      return false;
    }
    if (desc.biz_type != biz_type || desc.biz_name != adapter->BizName()) {
      has_conflict_ = true;
      std::string err =
          "Descriptor inconsistency for adapter '" +
          std::string(adapter->BizName()) +
          "': BizType/BizName mismatch between methods and descriptor";
      registration_errors_.push_back(err);
      std::cerr << "[BusinessAdapterRegistry ERROR] " << err << std::endl;
      return false;
    }

    // 冲突检查 1: 业务 ID 重复冲突
    auto it = adapters_.find(biz_type);
    if (it != adapters_.end()) {
      has_conflict_ = true;
      std::string err = "Conflict: BizType [" + std::to_string(biz_type) +
                        "] already registered by '" + it->second->BizName() +
                        "'. Cannot register '" + adapter->BizName() + "'.";
      registration_errors_.push_back(err);
      std::cerr << "[BusinessAdapterRegistry ERROR] " << err << std::endl;
      return false;
    }

    // 冲突检查 2: 业务名称重复冲突
    for (const auto& kv : adapters_) {
      if (kv.second->BizName() == std::string(adapter->BizName())) {
        has_conflict_ = true;
        std::string err = "Conflict: BizName '" +
                          std::string(adapter->BizName()) +
                          "' already registered under BizType [" +
                          std::to_string(kv.first) + "].";
        registration_errors_.push_back(err);
        std::cerr << "[BusinessAdapterRegistry ERROR] " << err << std::endl;
        return false;
      }
    }

    // 冲突检查 3: 业务 Pipeline 契约必须声明且在 Catalog 中无冲突
    if (desc.pipelines.empty()) {
      has_conflict_ = true;
      std::string err =
          "Adapter '" + std::string(adapter->BizName()) +
          "' must declare at least one BusinessDefinition in pipelines";
      registration_errors_.push_back(err);
      std::cerr << "[BusinessAdapterRegistry ERROR] " << err << std::endl;
      return false;
    }

    for (const auto& pipeline_def : desc.pipelines) {
      if (pipeline_def.business_name.empty()) {
        has_conflict_ = true;
        std::string err = "Adapter '" + std::string(adapter->BizName()) +
                          "' has a pipeline with empty business_name";
        registration_errors_.push_back(err);
        return false;
      }
    }
    if (!PipelineCatalog::RegisterBusinessDefinitions(desc.pipelines)) {
      has_conflict_ = true;
      std::string err =
          "Conflict: one or more pipeline business names in adapter '" +
          std::string(adapter->BizName()) +
          "' are invalid, duplicated, or already registered";
      registration_errors_.push_back(err);
      std::cerr << "[BusinessAdapterRegistry ERROR] " << err << std::endl;
      return false;
    }

    adapters_[biz_type] = adapter;
    // Keep stdout machine-readable for tools such as alg_pipeline_tool.
    std::cerr << "[BusinessAdapterRegistry] Registered adapter for BizType ["
              << biz_type << "]: " << adapter->BizName()
              << " (ABI: " << adapter->GetDescriptor().abi_version << ")"
              << std::endl;
    return true;
  }

  std::shared_ptr<IBusinessAdapter> GetAdapter(
      CompanyAlgBizType biz_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = adapters_.find(biz_type);
    if (it != adapters_.end()) {
      return it->second;
    }
    return nullptr;
  }

  enum class AdapterLookupStatus {
    kSuccess = 0,
    kNotFound = 1,
    kAmbiguousMatch = 2,
  };

  std::shared_ptr<IBusinessAdapter> GetAdapterByPipelineName(
      const std::string& pipeline_name,
      AdapterLookupStatus* out_status = nullptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<IBusinessAdapter> matched = nullptr;
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

  std::shared_ptr<IBusinessAdapter> GetAdapterByBusinessName(
      const std::string& biz_name,
      AdapterLookupStatus* out_status = nullptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<IBusinessAdapter> matched = nullptr;
    size_t match_count = 0;
    for (const auto& kv : adapters_) {
      if (kv.second && kv.second->BizName() == biz_name) {
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
    std::cerr << "[BusinessAdapterRegistry ERROR] " << error_msg << std::endl;
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
  BusinessAdapterRegistry() = default;
  mutable std::mutex mutex_;
  bool has_conflict_ = false;
  std::vector<std::string> registration_errors_;
  std::unordered_map<int, std::shared_ptr<IBusinessAdapter>> adapters_;
};

/**
 * @brief 自动注册宏 (零异常抛出保证，若发生异常记录错误并标记冲突)
 */
#define REGISTER_BUSINESS_ADAPTER(Class)                                     \
  static bool _registered_adapter_##Class = []() noexcept {                  \
    try {                                                                    \
      auto adapter = std::make_shared<Class>();                              \
      return ::alg_framework::BusinessAdapterRegistry::Instance()            \
          .RegisterAdapter(adapter);                                         \
    } catch (const std::exception& e) {                                      \
      ::alg_framework::BusinessAdapterRegistry::Instance()                   \
          .RecordRegistrationError(                                          \
              std::string("Exception registering " #Class ": ") + e.what()); \
      return false;                                                          \
    } catch (...) {                                                          \
      ::alg_framework::BusinessAdapterRegistry::Instance()                   \
          .RecordRegistrationError("Unknown exception registering " #Class); \
      return false;                                                          \
    }                                                                        \
  }()

}  // namespace alg_framework
