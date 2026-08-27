#pragma once

#include <string>

#include "operator/operator_interface.h"

namespace alg_framework {

/**
 * @brief Operator 控制指令解析与参数适配器 (将 Operator 枚举与参数分发到
 * Pipeline::Control)
 */
class OperatorControlRegistry {
 public:
  /**
   * @brief 解析 Operator 控制参数为标准 JSON 控制字符串
   * @param[in] command 控制命令枚举
   * @param[in] control_param 控制参数指针
   * @param[out] out_cmd_id 映射后的内部控制指令 ID
   * @param[out] out_json_str 映射后的 JSON 参数字符串
   * @param[out] error_msg 结构化诊断错误信息
   * @return 0 成功, 非 0 错误码 (-2 参数非法)
   */
  static int ResolveControlParam(
      llm_edgeflow::operator_api::ControlCommand command, void* control_param,
      int* out_cmd_id, std::string* out_json_str,
      std::string* error_msg) noexcept;
};

}  // namespace alg_framework
