#pragma once

#include <iostream>

namespace alg_framework {

/**
 * @brief 业务适配器通用输入输出批量契约校验工具类 (Layer 1 内部)
 *
 * 契约规则：
 * 1. ValidateBatchPreFlight (执行前严苛预检，REV2-002 & REV2-005)：
 *    - 必须在 Pipeline Execute 前调用，杜绝容量不足时无效执行模型推理；
 *    - 检查 num_inputs > 0 且不超过 max_batch_size (超出确定性返回 -3)；
 *    - 检查每一个 inputs[i] 非空 (包含空槽位确定性返回 -3)；
 *    - 检查 *num_outputs 容量 >= required_count (不足时回填所需容量并确定性返回
 * -4)；
 *    - 检查每一个 outputs[i] 非空 (包含空槽位确定性返回 -4)。
 */
class AdapterValidationHelper {
 public:
  static int ValidateBatchPreFlight(const void** inputs, int num_inputs,
                                    void** outputs, int* num_outputs,
                                    int max_batch_size, int required_count,
                                    const char* biz_name) {
    if (!inputs || num_inputs <= 0) {
      std::cerr
          << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
          << " PreFlight failed: Invalid inputs array or num_inputs <= 0 ("
          << num_inputs << ")" << std::endl;
      return -3;
    }

    // REV2-005: 强制校验 max_batch_size Descriptor 契约
    if (max_batch_size > 0 && num_inputs > max_batch_size) {
      std::cerr << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
                << " PreFlight failed: num_inputs (" << num_inputs
                << ") exceeds max_batch_size limit (" << max_batch_size << ")"
                << std::endl;
      return -3;
    }

    for (int i = 0; i < num_inputs; ++i) {
      if (!inputs[i]) {
        std::cerr << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
                  << " PreFlight failed: Null pointer at input index [" << i
                  << "]" << std::endl;
        return -3;
      }
    }

    if (!num_outputs || *num_outputs < 0) {
      std::cerr
          << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
          << " PreFlight failed: Invalid num_outputs pointer or negative capacity"
          << std::endl;
      return -4;
    }

    // REV2-002: 提前拦截容量不足或空 outputs (标准容量预查)，回填所需容量
    int capacity = *num_outputs;
    if (capacity < required_count || !outputs) {
      std::cerr << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
                << " PreFlight: Output capacity (" << capacity
                << ") insufficient or outputs array null for required count ("
                << required_count << ")" << std::endl;
      *num_outputs = required_count;  // 报告所需容量
      return -4;
    }

    for (int i = 0; i < required_count; ++i) {
      if (!outputs[i]) {
        std::cerr << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
                  << " PreFlight failed: Null pointer at output slot index ["
                  << i << "]" << std::endl;
        return -4;
      }
    }

    return 0;
  }

  static int ValidateBatchInputs(const void** inputs, int num_inputs,
                                 const char* biz_name = nullptr) {
    return ValidateBatchInputs(inputs, num_inputs, 64, biz_name);
  }

  static int ValidateBatchInputs(const void** inputs, int num_inputs,
                                 int max_batch_size, const char* biz_name) {
    if (!inputs || num_inputs <= 0) {
      return -3;
    }
    if (max_batch_size > 0 && num_inputs > max_batch_size) {
      return -3;
    }
    for (int i = 0; i < num_inputs; ++i) {
      if (!inputs[i]) return -3;
    }
    return 0;
  }

  static int ValidateBatchOutputs(void** outputs, int* num_outputs,
                                  int required_count,
                                  const char* biz_name = nullptr) {
    if (!num_outputs || *num_outputs < 0) {
      return -4;
    }
    int capacity = *num_outputs;
    if (capacity < required_count || !outputs) {
      *num_outputs = required_count;
      return -4;
    }
    for (int i = 0; i < required_count; ++i) {
      if (!outputs[i]) return -4;
    }
    return 0;
  }
};

}  // namespace alg_framework
