#pragma once

#include <string>
#include <utility>

#include "adapter/adapter_status.h"
#include "contracts/error_codes.h"
#include "core/alg_context.h"

namespace llm_edgeflow {

// 接入层诊断返回与请求值发布工具。
class AdapterValidationHelper {
 public:
  static int ReturnInvalidInput(AdapterStatus* out_status, std::string message,
                                std::string field_path,
                                const char* adapter_name,
                                int sample_index = -1) {
    if (out_status) {
      *out_status = AdapterStatus::InvalidInput(
          std::move(message), std::move(field_path), sample_index,
          adapter_name ? adapter_name : "");
    }
    return error::kInvalidInput;
  }

  static int ReturnBufferTooSmall(AdapterStatus* out_status,
                                  std::string message, std::string field_path,
                                  const char* adapter_name,
                                  int sample_index = -1) {
    if (out_status) {
      *out_status = AdapterStatus::BufferTooSmall(
          std::move(message), std::move(field_path), sample_index,
          adapter_name ? adapter_name : "");
    }
    return error::kInvalidOutput;
  }

  template <typename T>
  static bool PublishContextValue(AlgContext& ctx, const std::string& key,
                                  T&& value, const char* adapter_name,
                                  AdapterStatus* out_status) {
    if (key.empty()) return true;
    if (ctx.Publish(key, std::forward<T>(value))) return true;
    ReturnInvalidInput(out_status, "Duplicate AlgContext publication: " + key,
                       key, adapter_name);
    return false;
  }
};

}  // namespace llm_edgeflow
