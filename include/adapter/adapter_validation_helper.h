#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "adapter/adapter_status.h"
#include "company_alg_interface.h"
#include "company_alg_log.h"

namespace alg_framework {

/**
 * @brief 业务适配器通用输入输出批量契约校验工具类 (Layer 1 内部)
 *
 * 契约规则与安全解析工具集 (ADP-001, ADP-002, ADP-005, REV2-002, REV2-005)：
 * 1. ValidateBatchPreFlight (执行前严苛预检)：
 *    - 必须在 Pipeline Execute 前调用，杜绝容量不足时无效执行模型推理；
 *    - 检查 num_inputs > 0 且不超过 max_batch_size (超出确定性返回 -3)；
 *    - 检查每一个 inputs[i] 非空 (包含空槽位确定性返回 -3)；
 *    - 检查 *num_outputs 容量 >= required_count (不足时回填所需容量并确定性返回
 * -4)；
 *    - 检查每一个 outputs[i] 非空 (包含空槽位确定性返回 -4)。
 * 2. 字段级安全解析工具 (ADP-001, ADP-005, RECHECK-001, RECHECK-004)：
 *    - RequireNotNull: 非空指针检查与字段路径诊断；
 *    - RequireRange: 数值区间边界检查；
 *    - RequireEnum: 枚举值有效性与 Tagged Union 校验；
 *    - RequireBoundedString: 有界安全字符串扫描与长度校验 (防止无界内存扫描)；
 *    - CheckedMultiply: 乘法溢出与最大缓冲区字节限制；
 *    - CheckedStringCopy: 字符串安全拷贝，截断时返回 false 并记录
 * BufferTooSmall 诊断。
 */
class AdapterValidationHelper {
 public:
  static int ValidateBatchPreFlight(const void** inputs, int num_inputs,
                                    void** outputs, int* num_outputs,
                                    int max_batch_size, int required_count,
                                    const char* biz_name) {
    if (!inputs || num_inputs <= 0) {
      ALG_LOG_ERROR(
          "[AdapterValidation] %s PreFlight failed: Invalid inputs array or "
          "num_inputs <= 0 (%d)\n",
          biz_name ? biz_name : "Biz", num_inputs);
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }

    // REV2-005: 强制校验 max_batch_size Descriptor 契约
    if (max_batch_size > 0 && num_inputs > max_batch_size) {
      ALG_LOG_ERROR(
          "[AdapterValidation] %s PreFlight failed: num_inputs (%d) exceeds "
          "max_batch_size limit (%d)\n",
          biz_name ? biz_name : "Biz", num_inputs, max_batch_size);
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }

    for (int i = 0; i < num_inputs; ++i) {
      if (!inputs[i]) {
        ALG_LOG_ERROR(
            "[AdapterValidation] %s PreFlight failed: Null pointer at input "
            "index [%d]\n",
            biz_name ? biz_name : "Biz", i);
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
    }

    if (!num_outputs || *num_outputs < 0) {
      ALG_LOG_ERROR(
          "[AdapterValidation] %s PreFlight failed: Invalid num_outputs "
          "pointer or negative capacity\n",
          biz_name ? biz_name : "Biz");
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    // REV2-002: 提前拦截容量不足或空 outputs (标准容量预查)，回填所需容量
    int capacity = *num_outputs;
    if (capacity < required_count || !outputs) {
      ALG_LOG_ERROR(
          "[AdapterValidation] %s PreFlight: Output capacity (%d) "
          "insufficient or outputs array null for required count (%d)\n",
          biz_name ? biz_name : "Biz", capacity, required_count);
      *num_outputs = required_count;  // 报告所需容量
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    for (int i = 0; i < required_count; ++i) {
      if (!outputs[i]) {
        ALG_LOG_ERROR(
            "[AdapterValidation] %s PreFlight failed: Null pointer at output "
            "slot index [%d]\n",
            biz_name ? biz_name : "Biz", i);
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }

    return COMPANY_ALG_SUCCESS;
  }

  static int ValidateBatchInputs(const void** inputs, int num_inputs,
                                 int max_batch_size = 64,
                                 const char* biz_name = nullptr) {
    (void)biz_name;
    if (!inputs || num_inputs <= 0) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    if (max_batch_size > 0 && num_inputs > max_batch_size) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    for (int i = 0; i < num_inputs; ++i) {
      if (!inputs[i]) return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  static int ValidateBatchOutputs(void** outputs, int* num_outputs,
                                  int required_count,
                                  const char* biz_name = nullptr) {
    (void)biz_name;
    if (!num_outputs || *num_outputs < 0) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
    int capacity = *num_outputs;
    if (capacity < required_count || !outputs) {
      *num_outputs = required_count;
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
    for (int i = 0; i < required_count; ++i) {
      if (!outputs[i]) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
    return COMPANY_ALG_SUCCESS;
  }

  static int ReturnInvalidInput(AdapterStatus* out_status, std::string message,
                                std::string field_path, const char* biz_name,
                                int sample_index = -1) {
    if (out_status) {
      *out_status =
          AdapterStatus::InvalidInput(std::move(message), std::move(field_path),
                                      sample_index, biz_name ? biz_name : "");
    }
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  static int ReturnBufferTooSmall(AdapterStatus* out_status,
                                  std::string message, std::string field_path,
                                  const char* biz_name, int sample_index = -1) {
    if (out_status) {
      *out_status = AdapterStatus::BufferTooSmall(
          std::move(message), std::move(field_path), sample_index,
          biz_name ? biz_name : "");
    }
    return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
  }

  // =========================================================================
  // 结构化字段级安全解析工具集 (ADP-001, ADP-005, RECHECK-001, RECHECK-004)
  // =========================================================================

  /**
   * @brief 非空指针强校验
   */
  static bool RequireNotNull(const char* field_path, const void* ptr,
                             int sample_idx = -1,
                             const char* biz_name = nullptr,
                             AdapterStatus* out_status = nullptr) {
    if (!ptr) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "Required pointer field is null", field_path ? field_path : "",
            sample_idx, biz_name ? biz_name : "");
      }
      return false;
    }
    return true;
  }

  /**
   * @brief 数值范围强校验
   */
  static bool RequireRange(const char* field_path, int64_t val, int64_t min_val,
                           int64_t max_val, int sample_idx = -1,
                           const char* biz_name = nullptr,
                           AdapterStatus* out_status = nullptr) {
    if (val < min_val || val > max_val) {
      if (out_status) {
        std::string msg =
            "Field value out of range (val=" + std::to_string(val) +
            ", allowed=[" + std::to_string(min_val) + ", " +
            std::to_string(max_val) + "])";
        *out_status = AdapterStatus::InvalidInput(
            std::move(msg), field_path ? field_path : "", sample_idx,
            biz_name ? biz_name : "");
      }
      return false;
    }
    return true;
  }

  /**
   * @brief 枚举合法性校验 (Tagged Union 模式)
   */
  static bool RequireEnum(const char* field_path, int val,
                          const std::vector<int>& allowed_enums,
                          int sample_idx = -1, const char* biz_name = nullptr,
                          AdapterStatus* out_status = nullptr) {
    for (int allowed : allowed_enums) {
      if (val == allowed) return true;
    }
    if (out_status) {
      std::string msg = "Illegal enum value: " + std::to_string(val);
      *out_status = AdapterStatus::InvalidInput(
          std::move(msg), field_path ? field_path : "", sample_idx,
          biz_name ? biz_name : "");
    }
    return false;
  }

  /**
   * @brief 有界 C 字符串安全扫描与长度校验 (防止无界内存扫描, RECHECK-004)
   */
  static bool RequireBoundedString(const char* field_path, const char* str,
                                   size_t max_len, int sample_idx = -1,
                                   const char* biz_name = nullptr,
                                   AdapterStatus* out_status = nullptr) {
    if (!str) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "Required string field is null", field_path ? field_path : "",
            sample_idx, biz_name ? biz_name : "");
      }
      return false;
    }
    size_t len = ::strnlen(str, max_len + 1);
    if (len > max_len) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "String exceeds maximum allowed length limit (" +
                std::to_string(max_len) + ")",
            field_path ? field_path : "", sample_idx, biz_name ? biz_name : "");
      }
      return false;
    }
    return true;
  }

  /**
   * @brief 乘法溢出与缓冲区总大小安全检查
   */
  static bool CheckedMultiply(const char* field_path, size_t count,
                              size_t elem_size, size_t max_bytes,
                              int sample_idx = -1,
                              const char* biz_name = nullptr,
                              AdapterStatus* out_status = nullptr) {
    if (elem_size > 0 && count > SIZE_MAX / elem_size) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "Integer overflow detected in byte size calculation",
            field_path ? field_path : "", sample_idx, biz_name ? biz_name : "");
      }
      return false;
    }
    size_t total_bytes = count * elem_size;
    if (max_bytes > 0 && total_bytes > max_bytes) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "Total byte size (" + std::to_string(total_bytes) +
                ") exceeds max limit (" + std::to_string(max_bytes) + ")",
            field_path ? field_path : "", sample_idx, biz_name ? biz_name : "");
      }
      return false;
    }
    return true;
  }

  /**
   * @brief 字符串安全拷贝与截断检测 (RECHECK-001)
   * @return true 完整拷贝成功, false 目标容量不足产生截断并记录 BufferTooSmall
   */
  static bool CheckedStringCopy(char* dst, size_t dst_size, const char* src,
                                const char* field_path, int sample_idx = -1,
                                const char* biz_name = nullptr,
                                AdapterStatus* out_status = nullptr) {
    if (!dst || dst_size == 0) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Destination buffer pointer is null or zero size",
            field_path ? field_path : "", sample_idx, biz_name ? biz_name : "");
      }
      return false;
    }
    if (!src) {
      dst[0] = '\0';
      return true;
    }
    size_t src_len = std::strlen(src);
    if (src_len >= dst_size) {
      // 发生截断：写入安全终止符但返回 false，拒绝静默假装成功 (RECHECK-001)
      std::memcpy(dst, src, dst_size - 1);
      dst[dst_size - 1] = '\0';
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Output string truncated (src_len=" + std::to_string(src_len) +
                ", dst_capacity=" + std::to_string(dst_size) + ")",
            field_path ? field_path : "", sample_idx, biz_name ? biz_name : "");
      }
      return false;
    }
    std::memcpy(dst, src, src_len + 1);
    return true;
  }
};

}  // namespace alg_framework
