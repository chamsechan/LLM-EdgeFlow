#ifndef COMPANY_ALG_INTERFACE_H_
#define COMPANY_ALG_INTERFACE_H_

#include <stddef.h>
#include <stdint.h>

#include "edgeflow/export.h"
#include "edgeflow/version.h"
#include "platform_mock/alg_types.h"
#include "platform_mock/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define COMPANY_ALG_NOEXCEPT noexcept
#else
#define COMPANY_ALG_NOEXCEPT
#endif

// -------------------------------------------------------------
// 本项目实现的六个 C ABI 入口，当前使用本地平台模拟参数类型。
// -------------------------------------------------------------

/**
 * @brief 全局资源初始化 (进程级)
 */
COMPANY_ALG_API int Alg_Init(void) COMPANY_ALG_NOEXCEPT;

/**
 * @brief 创建算法处理句柄实例 (会话级)
 * @param[out] hndl 返回的句柄指针
 * @param[in] param_create 创建参数
 */
COMPANY_ALG_API int Alg_Create(void** hndl,
                               const CompanyAlgParamCreate* param_create)
    COMPANY_ALG_NOEXCEPT;

/**
 * @brief 批量执行算法计算 (纯 C 标准批处理接口)
 * @param[in] hndl 算法句柄
 * @param[in] inputs 多个输入结构体指针数组
 * @param[in] num_inputs 输入样本数量
 * @param[out] outputs 多个输出结构体指针数组
 * @param[in,out] num_outputs 输入为 outputs 容量，输出为实际填充的样本数量
 * @note 同一 hndl 上的 Alg_Process 与 Alg_Control
 * 由实现串行执行；不同句柄可并行。
 */
COMPANY_ALG_API int Alg_Process(void* hndl, const void** inputs, int num_inputs,
                                void** outputs,
                                int* num_outputs) COMPANY_ALG_NOEXCEPT;

/**
 * @brief 运行时动态控制或参数调整
 * @note 与同一 hndl 上的 Alg_Process/Alg_Control 串行执行。
 */
COMPANY_ALG_API int Alg_Control(void* hndl,
                                const CompanyAlgParamControl* param_control)
    COMPANY_ALG_NOEXCEPT;

/**
 * @brief 销毁算法句柄实例
 * @pre 调用方必须先停止向 hndl 提交新调用，并等待该 hndl 上已有的
 * Alg_Process/Alg_Control 全部返回。
 * @post 返回后 hndl 永久失效，不得再次传给任何 Alg_* 函数。
 */
COMPANY_ALG_API int Alg_Destroy(void* hndl) COMPANY_ALG_NOEXCEPT;

/**
 * @brief 全局资源释放 (进程级)
 */
COMPANY_ALG_API int Alg_DeInit(void) COMPANY_ALG_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif  // COMPANY_ALG_INTERFACE_H_
