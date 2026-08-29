#pragma once

#include "engine/engine_interface.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

/**
 * @brief 模拟 NPU Cross-Encoder Reranker 精排模型引擎
 *
 * 硬件特征：
 * - 固定 Max Batch Size = 4
 * - 输入为 (Query, Passage) 对，输出 0.0 ~ 1.0 的相关度/风险匹配得分
 */
class MockNpuRerankEngine : public IRerankEngine {
 public:
  inline static constexpr char kEngineType[] = "mock_npu_rerank";

  MockNpuRerankEngine();
  ~MockNpuRerankEngine() override = default;

  bool Load(const std::string& model_path,
            const nlohmann::json& engine_config) override;
  size_t GetMaxBatchSize() const override { return max_batch_size_; }
  const std::string& EngineType() const override;
  const std::string& GetLoadedModelPath() const override { return model_path_; }
  int GetDeviceId() const override { return device_id_; }

  int ScoreTraceableBatch(
      const std::vector<TraceableItem<PairInput>>& input_pairs,
      std::vector<TraceableItem<float>>* output_scores) override;

 private:
  int RawNpuHardwareRerankInfer(const std::vector<PairInput>& batch_inputs,
                                std::vector<float>* batch_outputs);

  float ComputePairScore(const PairInput& pair);

 private:
  std::string model_path_;
  int device_id_ = -1;
  size_t max_batch_size_ = 4;
  bool is_loaded_ = false;
};

}  // namespace alg_framework
