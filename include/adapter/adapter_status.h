#pragma once

#include <cstdint>
#include <sstream>
#include <string>

#include "company_alg_interface.h"

namespace llm_edgeflow {

/**
 * @brief 输入内存所有权策略契约 (ADP-002)
 */
enum class OwnershipPolicy {
  kCopyIn = 0,  // 默认策略：Unpack 将外部数据完整复制到内部 DTO/Buffer
  kBorrowDuringProcess =
      1,  // 借用策略：仅在本次同步 Alg_Process 期间借用指针，调用返回后绝不留存
  kRetainWithCallback = 2  // 跨调用异步持有 (暂未开放)
};

/**
 * @brief 适配器线程与状态模型契约 (ADP-003)
 */
enum class ThreadModel {
  kStatelessThreadSafe =
      0,  // 强制无状态：Adapter 为单例并在所有句柄与线程间安全并发共享
  kStatefulHandleIsolated = 1  // 有状态：每个句柄独立拥有 Adapter 实例
};

/**
 * @brief 输出基数契约 (ADP-008)
 */
enum class OutputCardinality {
  kOneToOne = 0,      // 1:1 输出 (输出数量精确等于输入数量)
  kOneToMany = 1,     // 1:N 输出
  kManyToOne = 2,     // N:1 聚合输出
  kDataDependent = 3  // 数据依赖型变长输出
};

/**
 * @brief 结构化适配器状态与字段路径诊断 (ADP-001, ADP-005)
 */
class AdapterStatus {
 public:
  AdapterStatus() : code_(COMPANY_ALG_SUCCESS), sample_index_(-1) {}

  AdapterStatus(int code, std::string message, std::string field_path = "",
                int sample_index = -1, std::string biz_name = "")
      : code_(code),
        message_(std::move(message)),
        field_path_(std::move(field_path)),
        sample_index_(sample_index),
        biz_name_(std::move(biz_name)) {}

  static AdapterStatus Ok() { return AdapterStatus(); }

  static AdapterStatus InvalidInput(std::string message,
                                    std::string field_path = "",
                                    int sample_index = -1,
                                    std::string biz_name = "") {
    return AdapterStatus(COMPANY_ALG_ERR_INVALID_INPUT, std::move(message),
                         std::move(field_path), sample_index,
                         std::move(biz_name));
  }

  static AdapterStatus BufferTooSmall(std::string message,
                                      std::string field_path = "",
                                      int sample_index = -1,
                                      std::string biz_name = "") {
    return AdapterStatus(COMPANY_ALG_ERR_BUFFER_TOO_SMALL, std::move(message),
                         std::move(field_path), sample_index,
                         std::move(biz_name));
  }

  static AdapterStatus UnsupportedBiz(std::string message,
                                      std::string biz_name = "") {
    return AdapterStatus(COMPANY_ALG_ERR_UNSUPPORTED_BIZ, std::move(message),
                         "", -1, std::move(biz_name));
  }

  bool IsOk() const { return code_ == COMPANY_ALG_SUCCESS; }
  int Code() const { return code_; }
  const std::string& Message() const { return message_; }
  const std::string& FieldPath() const { return field_path_; }
  int SampleIndex() const { return sample_index_; }
  const std::string& BizName() const { return biz_name_; }

  std::string ToString() const {
    if (IsOk()) return "OK";
    std::ostringstream oss;
    oss << "[AdapterStatus] Error " << code_;
    if (!biz_name_.empty()) oss << " in Biz [" << biz_name_ << "]";
    if (sample_index_ >= 0) oss << " at sample [" << sample_index_ << "]";
    if (!field_path_.empty()) oss << " field `" << field_path_ << "`";
    if (!message_.empty()) oss << ": " << message_;
    return oss.str();
  }

 private:
  int code_;
  std::string message_;
  std::string field_path_;
  int sample_index_;
  std::string biz_name_;
};

}  // namespace llm_edgeflow
