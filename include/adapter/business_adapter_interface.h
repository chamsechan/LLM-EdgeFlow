#pragma once

#include <string>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "company_alg_interface.h"
#include "core/alg_context.h"

namespace alg_framework {

/**
 * @brief 业务适配器描述符 (Layer 1 机器可读元数据与契约声明, ADP-002, ADP-003,
 * ADP-008)
 */
struct AdapterDescriptor {
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  std::string biz_name;
  std::string abi_version = "2.0.0";
  std::string input_type_name;
  std::string output_type_name;
  int max_batch_size = 64;
  OwnershipPolicy ownership_policy = OwnershipPolicy::kCopyIn;
  ThreadModel thread_model = ThreadModel::kStatelessThreadSafe;
  OutputCardinality cardinality = OutputCardinality::kOneToOne;
};

/**
 * @brief 业务适配器抽象接口 (Layer 1 内部)
 *
 * 职责与契约 (ADP-001 ~ ADP-008)：
 * 1. ValidateBatch: 在执行 Pipeline 之前预检批大小、输入输出槽位及缓冲区容量
 * (REV2-002, REV2-005)
 * 2. Unpack: 将业务专属的纯 C 结构体解包并深拷贝到内部 DTO 存入 AlgContext
 * (强制 const，无状态共享)
 * 3. Pack: 从 AlgContext 读取算法输出 DTO 并打包回业务专属纯 C 结构体 (强制
 * const，无状态共享)
 */
class IBusinessAdapter {
 public:
  virtual ~IBusinessAdapter() = default;

  /**
   * @brief 绑定的业务类型
   */
  virtual CompanyAlgBizType BizType() const = 0;

  /**
   * @brief 业务名称
   */
  virtual const char* BizName() const = 0;

  /**
   * @brief 获取业务适配器机器可读元数据描述符
   */
  virtual const AdapterDescriptor& GetDescriptor() const = 0;

  /**
   * @brief 预估所需输出数量 (默认 1:1 批处理模式)
   */
  virtual int EstimateRequiredOutputs(int num_inputs) const {
    return num_inputs;
  }

  /**
   * @brief 校验 Pipeline 配置中的 business_name 与 Adapter 是否匹配绑定
   * (ADP-006)
   * @return true 匹配, false 业务绑定不一致拒绝创建
   */
  virtual bool ValidatePipelineBinding(
      const std::string& pipeline_biz_name) const {
    return true;
  }

  /**
   * @brief Pipeline 执行前完整批处理契约预检 (REV2-002, REV2-005)
   * @return 0 校验通过, -3 输入非法或超限, -4 输出缓冲区容量不足或空指针
   */
  virtual int ValidateBatch(const void** inputs, int num_inputs, void** outputs,
                            int* num_outputs) const {
    int required = EstimateRequiredOutputs(num_inputs);
    return AdapterValidationHelper::ValidateBatchPreFlight(
        inputs, num_inputs, outputs, num_outputs,
        GetDescriptor().max_batch_size, required, BizName());
  }

  /**
   * @brief 解包 C 结构体输入为内部 DTO (强制 const
   * 方法以保证跨线程无状态安全性, ADP-003)
   * @param[in] inputs C 输入结构体指针数组
   * @param[in] num_inputs 输入样本数
   * @param[out] ctx 请求黑板上下文
   * @return 0 成功，非 0 失败错误码
   */
  virtual int Unpack(const void** inputs, int num_inputs,
                     AlgContext* ctx) const = 0;

  /**
   * @brief 打包内部 DTO 为 C 结构体输出 (强制 const
   * 方法以保证跨线程无状态安全性, ADP-003)
   * @param[in] ctx 请求黑板上下文
   * @param[out] outputs C 输出结构体指针数组
   * @param[in,out] num_outputs 输出样本数
   * @return 0 成功，非 0 失败错误码
   */
  virtual int Pack(AlgContext* ctx, void** outputs, int* num_outputs) const = 0;
};

}  // namespace alg_framework
