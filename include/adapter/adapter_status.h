#pragma once

#include <cstdint>
#include <sstream>
#include <string>

#include "platform_mock/error_codes.h"

namespace llm_edgeflow {

/**
 * @brief 结构化适配器状态与字段路径诊断 (ADP-001, ADP-005)
 */
class AdapterStatus {
 public:
  AdapterStatus() : code_(COMPANY_ALG_SUCCESS), sample_index_(-1) {}

  AdapterStatus(int code, std::string message, std::string field_path = "",
                int sample_index = -1, std::string adapter_name = "")
      : code_(code),
        message_(std::move(message)),
        field_path_(std::move(field_path)),
        sample_index_(sample_index),
        adapter_name_(std::move(adapter_name)) {}

  static AdapterStatus Ok() { return AdapterStatus(); }

  static AdapterStatus InvalidInput(std::string message,
                                    std::string field_path = "",
                                    int sample_index = -1,
                                    std::string adapter_name = "") {
    return AdapterStatus(COMPANY_ALG_ERR_INVALID_INPUT, std::move(message),
                         std::move(field_path), sample_index,
                         std::move(adapter_name));
  }

  static AdapterStatus BufferTooSmall(std::string message,
                                      std::string field_path = "",
                                      int sample_index = -1,
                                      std::string adapter_name = "") {
    return AdapterStatus(COMPANY_ALG_ERR_BUFFER_TOO_SMALL, std::move(message),
                         std::move(field_path), sample_index,
                         std::move(adapter_name));
  }

  static AdapterStatus UnsupportedBiz(std::string message,
                                      std::string adapter_name = "") {
    return AdapterStatus(COMPANY_ALG_ERR_UNSUPPORTED_BIZ, std::move(message),
                         "", -1, std::move(adapter_name));
  }

  bool IsOk() const { return code_ == COMPANY_ALG_SUCCESS; }
  int Code() const { return code_; }
  const std::string& Message() const { return message_; }
  const std::string& FieldPath() const { return field_path_; }
  int SampleIndex() const { return sample_index_; }
  const std::string& AdapterName() const { return adapter_name_; }

  std::string ToString() const {
    if (IsOk()) return "OK";
    std::ostringstream oss;
    oss << "[AdapterStatus] Error " << code_;
    if (!adapter_name_.empty()) oss << " in Adapter [" << adapter_name_ << "]";
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
  std::string adapter_name_;
};

}  // namespace llm_edgeflow
