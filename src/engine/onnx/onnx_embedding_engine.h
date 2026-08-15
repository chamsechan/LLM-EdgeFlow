#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/engine_interface.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

/**
 * @brief 基于微软开源 ONNX Runtime 的特征向量提取引擎
 *
 * 架构隔离性：
 * - 实现 Layer 4 的 IEmbeddingEngine 纯虚基类接口；
 * - 仅在内部使用 ONNX Runtime C/C++ API，上层算子和调度层无感知；
 * - 结合 FixedBatchExecutor 满足硬件/固定 Batch 调度。
 */
class OnnxEmbeddingEngine : public IEmbeddingEngine {
 public:
  OnnxEmbeddingEngine();
  ~OnnxEmbeddingEngine() override;

  bool Load(const std::string& model_path,
            const nlohmann::json& engine_config) override;
  size_t GetMaxBatchSize() const override { return max_batch_size_; }
  const std::string& EngineType() const override;

  int InferTraceableBatch(
      const std::vector<TraceableItem<std::string>>& input_texts,
      std::vector<TraceableItem<std::vector<float>>>* output_embeddings)
      override;

 private:
  int RawOnnxHardwareInfer(const std::vector<std::string>& batch_inputs,
                           std::vector<std::vector<float>>* batch_outputs);

  std::vector<float> GenerateEmbedding(const std::string& text);

 private:
  std::string model_path_;
  size_t max_batch_size_ = 4;
  size_t embedding_dim_ = 128;
  bool is_loaded_ = false;

  // 内部 PIMPL / 引擎私有状态指针（完全屏蔽内部三方头文件泄露）
  struct Impl;
  std::unique_ptr<Impl> pimpl_;
};

}  // namespace alg_framework
