#pragma once

#include <memory>
#include <string>

#include "adapter/business_adapter_interface.h"
#include "company_alg_interface.h"
#include "core/pipeline.h"
#include "nlohmann/json.hpp"

namespace alg_framework {

/**
 * @brief 纯 C ABI 与 C++ 平台 Operator 门面共享的内部算法运行时句柄 (Layer 1
 * 内部)
 */
class SharedAlgorithmRuntime {
 public:
  SharedAlgorithmRuntime() = default;
  ~SharedAlgorithmRuntime() = default;

  // 禁止拷贝与移动赋值
  SharedAlgorithmRuntime(const SharedAlgorithmRuntime&) = delete;
  SharedAlgorithmRuntime& operator=(const SharedAlgorithmRuntime&) = delete;

  /**
   * @brief 全局资源初始化与注册表防腐冲突检查
   */
  static int GlobalInit() noexcept;

  /**
   * @brief 全局资源反初始化
   */
  static int GlobalDeinit() noexcept;

  /**
   * @brief 通过配置文件路径构建运行时
   */
  static int CreateFromConfigFile(
      const std::string& config_path, int device_id,
      const std::string& model_root_dir, CompanyAlgBizType biz_type,
      std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
      std::string* out_error = nullptr) noexcept;

  /**
   * @brief 通过内存中的 Pipeline JSON 配置对象直接构建运行时 (避免临时文件)
   */
  static int CreateFromPipelineJson(
      const nlohmann::json& pipeline_json, int device_id,
      const std::string& model_root_dir, CompanyAlgBizType biz_type,
      std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
      std::string* out_error = nullptr) noexcept;

  /**
   * @brief 批量计算通用流 (ValidateBatch -> Unpack -> Pipeline::Execute ->
   * Pack)
   */
  int ExecuteBatch(const void** inputs, int num_inputs, void** outputs,
                   int* num_outputs, std::string* out_error = nullptr) noexcept;

  /**
   * @brief 运行时动态控制指令下发
   */
  int ExecuteControl(int cmd, const std::string& json_param_str,
                     std::string* out_error = nullptr) noexcept;

  // Getters
  Pipeline* GetPipeline() { return pipeline_.get(); }
  const Pipeline* GetPipeline() const { return pipeline_.get(); }
  std::shared_ptr<IBusinessAdapter> GetAdapter() const { return adapter_; }
  CompanyAlgBizType GetBizType() const { return biz_type_; }
  int GetDeviceId() const { return device_id_; }

 private:
  std::unique_ptr<Pipeline> pipeline_;
  std::shared_ptr<IBusinessAdapter> adapter_;
  CompanyAlgBizType biz_type_ = ALG_BIZ_TYPE_UNKNOWN;
  int device_id_ = 0;
  std::string model_root_dir_;
  std::string config_file_path_;
};

}  // namespace alg_framework
