#pragma once

#include <vector>

#include "company_alg_interface.h"

#ifdef __cplusplus

/**
 * @brief C++ 便捷重载包装 (供 C++ 客户端与测试套件调用)
 *
 * 将 C++ std::vector<void*> 自动适配并调用底层纯 C 指针数组接口。
 */
inline int Alg_Process(void* hndl, const std::vector<void*>& inputs,
                       std::vector<void*>& outputs) {
  if (!hndl) return COMPANY_ALG_ERR_INVALID_HANDLE;
  if (inputs.empty()) return COMPANY_ALG_ERR_INVALID_PARAM;
  int num_outputs = static_cast<int>(outputs.size());
  return Alg_Process(hndl, const_cast<const void**>(inputs.data()),
                     static_cast<int>(inputs.size()),
                     outputs.empty() ? nullptr : outputs.data(), &num_outputs);
}

#endif  // __cplusplus
