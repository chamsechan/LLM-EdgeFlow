#pragma once

#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "adapter/operator_biz_bridge.h"
#include "edgeflow/c_api.h"

namespace llm_edgeflow {

/**
 * @brief Operator 业务桥接注册表 (SSOT 与自注册中心)
 */
class OperatorBizBridgeRegistry {
 public:
  static OperatorBizBridgeRegistry& Instance();

  /**
   * @brief 注册业务桥接描述符 (严格审计并在 Init 后冻结)
   */
  bool RegisterBridge(OperatorBizBridgeDescriptor desc);

  /**
   * @brief 获取业务桥接描述符
   */
  const OperatorBizBridgeDescriptor* GetBridge(
      CompanyAlgBizType biz_type) const;

  /**
   * @brief 全局初始化与一致性原子审计 (返回 -6 若存在任何冲突或缺漏)
   */
  int GlobalInit(std::string* diagnostic = nullptr);

  /**
   * @brief 检查是否存在冲突
   */
  bool HasConflict() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_conflict_;
  }

  /**
   * @brief 辅助函数：将 C 字符串安全复制至池化 CompanyString
   */
  static int CopyToPooledString(const char* src, CompanyString* dest,
                                uint32_t capacity, const char* field_name,
                                std::string* err) noexcept;

  OperatorBizBridgeRegistry() = default;

 private:
  void RecordConflict(CompanyAlgBizType biz_type, std::string_view adapter_name,
                      std::initializer_list<std::string_view> reason) noexcept;
  int ReportConflict(std::string* diagnostic) const noexcept;

  mutable std::mutex mutex_;
  bool has_conflict_ = false;
  bool audited_ = false;
  std::string conflict_diagnostic_;
  std::unordered_map<int32_t, OperatorBizBridgeDescriptor> bridges_by_biz_type_;
};

}  // namespace llm_edgeflow
