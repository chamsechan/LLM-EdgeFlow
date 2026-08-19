#pragma once

#include <iostream>
#include <memory>
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

  void RegisterAdapter(CompanyAlgBizType biz_type,
                       std::shared_ptr<IBusinessAdapter> adapter) {
    if (adapter) {
      adapters_[biz_type] = adapter;
      std::cout << "[BusinessAdapterRegistry] Registered adapter for BizType ["
                << biz_type << "]: " << adapter->BizName() << std::endl;
    }
  }

  std::shared_ptr<IBusinessAdapter> GetAdapter(
      CompanyAlgBizType biz_type) const {
    auto it = adapters_.find(biz_type);
    if (it != adapters_.end()) {
      return it->second;
    }
    return nullptr;
  }

 private:
  BusinessAdapterRegistry() = default;
  std::unordered_map<int, std::shared_ptr<IBusinessAdapter>> adapters_;
};

/**
 * @brief 自动注册宏
 */
#define REGISTER_BUSINESS_ADAPTER(Class)                                  \
  static bool _registered_adapter_##Class = []() {                        \
    auto adapter = std::make_shared<Class>();                             \
    ::alg_framework::BusinessAdapterRegistry::Instance().RegisterAdapter( \
        adapter->BizType(), adapter);                                     \
    return true;                                                          \
  }()

}  // namespace alg_framework
