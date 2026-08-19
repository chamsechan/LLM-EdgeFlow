#pragma once

#include "company_alg_interface.h"
#include "core/alg_context.h"

namespace alg_framework {

/**
 * @brief 业务适配器描述符 (Layer 1 机器可读元数据)
 */
struct AdapterDescriptor {
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  std::string biz_name;
  std::string abi_version = "2.0.0";
  std::string input_type_name;
  std::string output_type_name;
  int max_batch_size = 64;
};

/**
 * @brief 业务适配器抽象接口 (Layer 1 内部)
 *
 * 职责：
 * 1. 将业务专属的纯 C 结构体解包 (Unpack) 并转换为内部 DTO 存入 AlgContext
 * 2. 从 AlgContext 读取算法输出 DTO 并打包 (Pack) 回业务专属纯 C 结构体
 * 3. 校验批量输入输出指针有效性与数量
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
   * @brief 解包 C 结构体输入为内部 DTO
   * @param[in] inputs C 输入结构体指针数组
   * @param[in] num_inputs 输入样本数
   * @param[out] ctx 请求黑板上下文
   * @return 0 成功，非 0 失败错误码
   */
  virtual int Unpack(const void** inputs, int num_inputs, AlgContext* ctx) = 0;

  /**
   * @brief 打包内部 DTO 为 C 结构体输出
   * @param[in] ctx 请求黑板上下文
   * @param[out] outputs C 输出结构体指针数组
   * @param[in,out] num_outputs 输出样本数
   * @return 0 成功，非 0 失败错误码
   */
  virtual int Pack(AlgContext* ctx, void** outputs, int* num_outputs) = 0;
};

}  // namespace alg_framework
