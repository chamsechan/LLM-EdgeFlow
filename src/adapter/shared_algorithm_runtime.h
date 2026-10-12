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

  static int GlobalInit(std::string* diagnostic = nullptr) noexcept;
  static int ResolveIoPlan(const RuntimeCreateOptions& options,
                           std::unique_ptr<ValidatedIoPlan>* out_plan,
                           std::string* out_error = nullptr) noexcept;
  static int Create(const RuntimeCreateOptions& options,
                    std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
                    std::string* out_error = nullptr,
                    RuntimeFailureStage* failure_stage = nullptr) noexcept;
  static int CreateFromIoPlan(
      std::unique_ptr<ValidatedIoPlan> io_plan,
      const RuntimeCreateOptions& options,
      std::unique_ptr<SharedAlgorithmRuntime>* out_runtime,
      std::string* out_error = nullptr,
      RuntimeFailureStage* failure_stage = nullptr) noexcept;

  int Process(const RuntimeInputBatch& inputs, RuntimeOutputBatch* outputs,
              std::string* out_error = nullptr,
              RuntimeFailureStage* failure_stage = nullptr) noexcept;
  int ExecuteControl(const ControlRequest& request,
                     std::string* out_error = nullptr,
                     ControlFailureStage* failure_stage = nullptr) noexcept;
  // 即使报告仍有未归还的输出租约，也会关闭运行时并释放资源。
  int Close(std::string* out_error = nullptr) noexcept;

  const ValidatedIoPlan* GetIoPlan() const { return io_plan_.get(); }
  const Pipeline* GetPipeline() const { return pipeline_.get(); }
  size_t ProcessBatchLimit() const { return process_batch_limit_; }

 private:
  std::unique_ptr<ValidatedIoPlan> io_plan_;
  std::unique_ptr<Pipeline> pipeline_;
  std::vector<std::shared_ptr<OutputPoolState>> output_pools_;
  size_t process_batch_limit_ = 0;
  std::mutex mutex_;
};

}  // namespace llm_edgeflow
