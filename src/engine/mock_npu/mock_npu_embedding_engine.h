#pragma once

#include "engine/engine_interface.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

/**
 * @brief 模拟公司优化部门提供的 NPU 固定 Max Batch Embedding 模型引擎
 *
 * 硬件特征：
 * - 编译时固定 Max Batch Size = 4
 * - 输入不足 4 时必须 Pad，超过 4 时必须分批
 * - 向量维度 = 128 (L2 正规化)
 */
class MockNpuEmbeddingEngine : public IEmbeddingEngine {
 public:
  MockNpuEmbeddingEngine();
  ~MockNpuEmbeddingEngine() override = default;

  bool Load(const std::string& model_path,
            const nlohmann::json& engine_config) override;
  size_t GetMaxBatchSize() const override { return max_batch_size_; }
  const std::string& EngineType() const override;
  const std::string& GetLoadedModelPath() const override { return model_path_; }
  int GetDeviceId() const override { return device_id_; }

  int InferTraceableBatch(
      const std::vector<TraceableItem<std::string>>& input_texts,
      std::vector<TraceableItem<std::vector<float>>>* output_embeddings)
      override;

 private:
  // 模拟底层优化部门导出的固定 batch 硬件推理 API
  int RawNpuHardwareInfer(const std::vector<std::string>& batch_inputs,
                          std::vector<std::vector<float>>* batch_outputs);

  // 内部计算确定性的语义向量 (便于验证相似度计算)
  std::vector<float> ComputeDeterministicEmbedding(const std::string& text);

 private:
  std::string model_path_;
  int device_id_ = -1;
  size_t max_batch_size_ = 4;
  size_t embedding_dim_ = 128;
  bool is_loaded_ = false;
};

}  // namespace alg_framework
