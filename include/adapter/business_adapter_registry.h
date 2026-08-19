#pragma once

#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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
   * @brief 注册业务适配器 (防止多团队业务 ID / 名称冲突覆盖)
   * @return true 注册成功，false 冲突或无效拒绝注册
   */
  bool RegisterAdapter(std::shared_ptr<IBusinessAdapter> adapter) {
    if (!adapter) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    CompanyAlgBizType biz_type = adapter->BizType();

    // 冲突检查 1: 业务 ID 重复冲突
    auto it = adapters_.find(biz_type);
    if (it != adapters_.end()) {
      std::cerr << "[BusinessAdapterRegistry ERROR] Conflict: BizType ["
                << biz_type << "] already registered by '"
                << it->second->BizName() << "'. Cannot register '"
                << adapter->BizName() << "'." << std::endl;
      return false;
    }

    // 冲突检查 2: 业务名称重复冲突
    for (const auto& kv : adapters_) {
      if (kv.second->BizName() == std::string(adapter->BizName())) {
        std::cerr << "[BusinessAdapterRegistry ERROR] Conflict: BizName '"
                  << adapter->BizName()
                  << "' already registered under BizType [" << kv.first << "]."
                  << std::endl;
        return false;
      }
    }

    adapters_[biz_type] = adapter;
    std::cout << "[BusinessAdapterRegistry] Registered adapter for BizType ["
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

  size_t AdapterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return adapters_.size();
  }

  void ClearForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    adapters_.clear();
  }

 private:
  BusinessAdapterRegistry() = default;
  mutable std::mutex mutex_;
  std::unordered_map<int, std::shared_ptr<IBusinessAdapter>> adapters_;
};

/**
 * @brief 自动注册宏 (零异常抛出保证)
 */
#define REGISTER_BUSINESS_ADAPTER(Class)                          \
  static bool _registered_adapter_##Class = []() noexcept {       \
    try {                                                         \
      auto adapter = std::make_shared<Class>();                   \
      return ::alg_framework::BusinessAdapterRegistry::Instance() \
          .RegisterAdapter(adapter);                              \
    } catch (...) {                                               \
      return false;                                               \
    }                                                             \
  }()

}  // namespace alg_framework
