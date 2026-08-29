#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/engine_interface.h"
#include "engine/model_interface.h"

namespace alg_framework {

/**
 * @brief 基于统一 Model/Backend 架构的旧版 IEmbeddingEngine 适配器
 *
 * 架构隔离性：
 * - 实现 Layer 4 的 IEmbeddingEngine 纯虚基类接口，仅供过渡期使用；
 * - 内部完全转调 OnnxRuntimeBackend 与 BgeEmbeddingModel，无自身 ONNX 逻辑；
 * - 完全不引入 onnxruntime_cxx_api.h，实现单点维护。
 */
class OnnxEmbeddingEngine : public IEmbeddingEngine {
 public:
  inline static constexpr char kEngineType[] = "onnx_embedding";

  OnnxEmbeddingEngine();
  ~OnnxEmbeddingEngine() override;

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
  std::string model_path_;
  int device_id_ = -1;
  size_t max_batch_size_ = 4;
  size_t embedding_dim_ = 384;
  bool is_loaded_ = false;

  std::shared_ptr<IEmbeddingModel> model_;
};

}  // namespace alg_framework
