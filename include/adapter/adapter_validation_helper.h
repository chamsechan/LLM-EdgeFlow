#pragma once

#include <iostream>

namespace alg_framework {

/**
 * @brief 业务适配器通用输入输出批量契约校验工具类 (Layer 1 内部)
 *
 * 契约规则：
 * 1. ValidateBatchInputs:
 *    - inputs 必须非空且 num_inputs > 0
 *    - 每一个 inputs[i] 指针必须非空，杜绝静默跳过空槽位
 * 2. ValidateBatchOutputs:
 *    - outputs 与 num_outputs 指针必须非空
 *    - *num_outputs 传入值作为容积 (Capacity)。若 capacity < required_count，
 *      必须将 *num_outputs 回填为 required_count (告知所需容量) 并确定性返回 -4
 * (缓冲区不足)
 *    - 每一个 outputs[i] (i < required_count) 指针必须非空，杜绝空指针写入
 */
class AdapterValidationHelper {
 public:
  static int ValidateBatchInputs(const void** inputs, int num_inputs,
                                 const char* biz_name) {
    if (!inputs || num_inputs <= 0) {
      std::cerr << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
                << " Unpack failed: Invalid input array or num_inputs <= 0 ("
                << num_inputs << ")" << std::endl;
      return -3;
    }
    for (int i = 0; i < num_inputs; ++i) {
      if (!inputs[i]) {
        std::cerr << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
                  << " Unpack failed: Null pointer at input index [" << i << "]"
                  << std::endl;
        return -3;
      }
    }
    return 0;
  }

  static int ValidateBatchOutputs(void** outputs, int* num_outputs,
                                  int required_count, const char* biz_name) {
    if (!outputs || !num_outputs) {
      std::cerr << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
                << " Pack failed: Null outputs or num_outputs pointer"
                << std::endl;
      return -4;
    }

    int capacity = *num_outputs;
    if (capacity < required_count) {
      std::cerr << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
                << " Pack failed: Output capacity (" << capacity
                << ") is smaller than required results count ("
                << required_count << ")" << std::endl;
      *num_outputs = required_count;  // 报告所需容量
      return -4;
    }

    for (int i = 0; i < required_count; ++i) {
      if (!outputs[i]) {
        std::cerr << "[AdapterValidation] " << (biz_name ? biz_name : "Biz")
                  << " Pack failed: Null pointer at output slot index [" << i
                  << "]" << std::endl;
        return -4;
      }
    }
    return 0;
  }
};

}  // namespace alg_framework
