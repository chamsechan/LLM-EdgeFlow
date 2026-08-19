#pragma once

#include "engine/engine_interface.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

/**
 * @brief 模拟公司优化部门提供的 NPU 固定 Max Batch LLM 模型引擎
 *
 * 硬件特征：
 * - 编译时固定 Max Batch Size = 2
 * - 生成智能回答与意图摘要
 */
class MockNpuLlmEngine : public ILlmEngine {
 public:
  MockNpuLlmEngine();
  ~MockNpuLlmEngine() override = default;

  bool Load(const std::string& model_path,
            const nlohmann::json& engine_config) override;
  size_t GetMaxBatchSize() const override { return max_batch_size_; }
  const std::string& EngineType() const override;
  const std::string& GetLoadedModelPath() const override { return model_path_; }
  int GetDeviceId() const override { return device_id_; }

  int Generate(const std::string& prompt, const GenerateOption& opt,
               std::string* output_text) override;

  int InferTraceableBatch(
      const std::vector<TraceableItem<std::string>>& input_prompts,
      const GenerateOption& opt,
      std::vector<TraceableItem<std::string>>* output_texts) override;

 private:
  int RawNpuHardwareLlmInfer(const std::vector<std::string>& batch_prompts,
                             std::vector<std::string>* batch_outputs);

  std::string GenerateSingleResponse(const std::string& prompt);

 private:
  std::string model_path_;
  int device_id_ = -1;
  size_t max_batch_size_ = 2;
  int max_seq_len_ = 1024;
  bool is_loaded_ = false;
};

}  // namespace alg_framework
