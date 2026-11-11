#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "adapter/runtime_types.h"

namespace llm_edgeflow {

class Pipeline;
class OutputPoolState;
struct ValidatedIoPlan;
enum class ControlFailureStage;

// 接入层运行时，不包含协议 ABI 或具体载体类型。
class SharedAlgorithmRuntime {
 public:
  SharedAlgorithmRuntime();
  ~SharedAlgorithmRuntime();
  SharedAlgorithmRuntime(const SharedAlgorithmRuntime&) = delete;
  SharedAlgorithmRuntime& operator=(const SharedAlgorithmRuntime&) = delete;

  /**
   * @brief 审计注册冲突及 Model、Backend、Converter 的绑定关系，
   * 通过后初始化宿主类型注册表。
   * 任一审计失败即返回框架错误码，并汇总诊断；此处不创建模型实例。
   */
  static int GlobalInit(std::string* diagnostic = nullptr) noexcept;
  static int ResolveIoPlan(const RuntimeCreateOptions& options,
                           std::unique_ptr<ValidatedIoPlan>* out_plan,
                           std::string* out_error = nullptr) noexcept;
  static int Create(const RuntimeCreateOptions& options,
                    std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
                    std::string* out_error = nullptr,
                    RuntimeFailureStage* failure_stage = nullptr) noexcept;
  /**
   * @brief 消费已验证计划，构建 Pipeline 并预分配各输出槽位的对象池。
   * 输出池深度必须与计划预算一致；全部准备成功后才交付运行时，失败时输出为空。
   * failure_stage 区分配置准备失败与后续输出池创建失败。
   */
  static int CreateFromIoPlan(
      std::unique_ptr<ValidatedIoPlan> io_plan,
      const RuntimeCreateOptions& options,
      std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
      std::string* out_error = nullptr,
      RuntimeFailureStage* failure_stage = nullptr) noexcept;

  /**
   * @brief 串行完成输入解码、Pipeline 执行和所请求输出的编码。
   * 调用方须预置空输出槽位。
   * 全部编码成功后才发布租约；中途失败由守卫归还已检出的块，池满时会等待空闲块。
   * 输入校验与转换使用已验证计划的 binding，调用方传入的回调不参与执行。
   */
  int Process(const RuntimeInputBatch& inputs, RuntimeOutputBatch* outputs,
              std::string* out_error = nullptr,
              RuntimeFailureStage* failure_stage = nullptr) noexcept;
  int ExecuteControl(const ControlRequest& request,
                     std::string* out_error = nullptr,
                     ControlFailureStage* failure_stage = nullptr) noexcept;
  /**
   * @brief 即使报告仍有未归还的输出租约，也会关闭运行时并释放资源。
   */
  int Close(std::string* out_error = nullptr) noexcept;

  const ValidatedIoPlan* GetIoPlan() const { return io_plan_.get(); }
  size_t ProcessBatchLimit() const { return process_batch_limit_; }

 private:
  std::unique_ptr<ValidatedIoPlan> io_plan_;
  std::unique_ptr<Pipeline> pipeline_;
  std::vector<std::shared_ptr<OutputPoolState>> output_pools_;
  size_t process_batch_limit_ = 0;
  std::mutex mutex_;
};

}  // namespace llm_edgeflow
