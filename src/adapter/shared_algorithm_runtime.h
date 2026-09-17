#pragma once

#include <memory>
#include <string>

#include "adapter/io_binding_resolver.h"
#include "adapter/io_converter.h"
#include "core/pipeline.h"
#include "core/session_context.h"
#include "platform_mock/error_codes.h"

namespace llm_edgeflow {

/**
 * @brief 平台 Operator 门面使用的内部算法运行时句柄 (接入适配层内部)
 */
class SharedAlgorithmRuntime {
 public:
  SharedAlgorithmRuntime() = default;
  ~SharedAlgorithmRuntime() = default;

  // 禁止拷贝与移动赋值
  SharedAlgorithmRuntime(const SharedAlgorithmRuntime&) = delete;
  SharedAlgorithmRuntime& operator=(const SharedAlgorithmRuntime&) = delete;

  /**
   * @brief 全局资源初始化与全量注册表防腐冲突检查 (Fail-Closed)
   */
  static int GlobalInit() noexcept;

  /**
   * @brief 全局资源反初始化
   */
  static int GlobalDeinit() noexcept;

  /**
   * @brief 通过已验证的 ValidatedIoPlan 与 RuntimeOptions 构建运行时
   */
  static int CreateFromIoPlan(
      std::unique_ptr<ValidatedIoPlan> io_plan, int device_id,
      const RuntimeOptions* extra_runtime_options,
      std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
      std::string* out_error = nullptr) noexcept;

  /**
   * @brief 运行时动态控制指令下发
   */
  int ExecuteControl(int cmd, const std::string& json_param_str,
                     std::string* out_error = nullptr) noexcept;

  // Getters
  Pipeline* GetPipeline() { return pipeline_.get(); }
  const Pipeline* GetPipeline() const { return pipeline_.get(); }
  const ValidatedIoPlan* GetIoPlan() const { return io_plan_.get(); }
  int GetDeviceId() const {
    return pipeline_
               ? pipeline_->GetSessionContext().GetRuntimeOptions().device_id
               : -1;
  }

 private:
  std::unique_ptr<ValidatedIoPlan> io_plan_;
  std::unique_ptr<Pipeline> pipeline_;
};

}  // namespace llm_edgeflow
