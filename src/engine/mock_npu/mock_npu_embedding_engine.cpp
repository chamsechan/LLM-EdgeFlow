#include "engine/mock_npu/mock_npu_embedding_engine.h"

#include <cmath>
#include <numeric>

#include "company_alg_log.h"
#include "engine/engine_registry.h"

namespace alg_framework {

MockNpuEmbeddingEngine::MockNpuEmbeddingEngine() = default;

bool MockNpuEmbeddingEngine::Load(const std::string& model_path,
                                  const nlohmann::json& engine_config) {
  model_path_ = model_path;
  max_batch_size_ = engine_config.value("max_batch_size", 4);
  embedding_dim_ = engine_config.value("embedding_dim", 128);
  device_id_ = engine_config.value("device_id", -1);
  is_loaded_ = true;
  ALG_LOG_INFO(
      "[MockNpuEmbeddingEngine] Loaded model from: %s, Fixed MaxBatchSize: "
      "%zu, Dim: %zu, Device: %d\n",
      model_path.c_str(), max_batch_size_, embedding_dim_, device_id_);
  return true;
}

const std::string& MockNpuEmbeddingEngine::EngineType() const {
  static const std::string type = kEngineType;
  return type;
}

int MockNpuEmbeddingEngine::InferTraceableBatch(
    const std::vector<TraceableItem<std::string>>& input_texts,
    std::vector<TraceableItem<std::vector<float>>>* output_embeddings) {
  if (!is_loaded_) return -1001;

  std::string dummy_pad = "<PAD_TEXT>";

  // 使用 FixedBatchExecutor 自动完成分批与填充
  return FixedBatchExecutor::Execute<std::string, std::vector<float>>(
      input_texts, max_batch_size_, dummy_pad,
      [this](const std::vector<std::string>& batch_in,
             std::vector<std::vector<float>>* batch_out) {
        return this->RawNpuHardwareInfer(batch_in, batch_out);
      },
      output_embeddings);
}

int MockNpuEmbeddingEngine::RawNpuHardwareInfer(
    const std::vector<std::string>& batch_inputs,
    std::vector<std::vector<float>>* batch_outputs) {
  // 严格校验底层 NPU 输入是否恰好为固定 max_batch_size
  if (batch_inputs.size() != max_batch_size_) {
    ALG_LOG_ERROR(
        "[MockNpuEmbeddingEngine] HARDWARE ERROR: Batch size %zu != Fixed "
        "MaxBatch %zu\n",
        batch_inputs.size(), max_batch_size_);
    return -1002;
  }

  ALG_LOG_DEBUG(
      "  [NPU Hardware] Executing NPU Embedding kernel with batch=%zu\n",
      max_batch_size_);
  batch_outputs->resize(max_batch_size_);

  for (size_t i = 0; i < max_batch_size_; ++i) {
    (*batch_outputs)[i] = ComputeDeterministicEmbedding(batch_inputs[i]);
  }

  return 0;
}

std::vector<float> MockNpuEmbeddingEngine::ComputeDeterministicEmbedding(
    const std::string& text) {
  std::vector<float> vec(embedding_dim_, 0.0f);
  if (text == "<PAD_TEXT>" || text.empty()) {
    return vec;
  }

  // 产生一个基于文本内容与字符分布的归一化稠密向量
  for (size_t i = 0; i < text.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    size_t idx = (c * 31 + i * 17) % embedding_dim_;
    vec[idx] += 1.0f + static_cast<float>(c % 7);
  }

  // L2 归一化
  float norm_sq = 0.0f;
  for (float val : vec) norm_sq += val * val;
  float norm = std::sqrt(norm_sq);
  if (norm > 1e-6f) {
    for (float& val : vec) val /= norm;
  }

  return vec;
}

EngineDefinition MakeMockNpuEmbeddingDefinition() {
  EngineDefinition def;
  def.engine_type = MockNpuEmbeddingEngine::kEngineType;
  def.capability = "embedding";
  def.description = "Mock NPU embedding engine";
  def.config_fields = {
      ConfigFieldDefinition{"max_batch_size", ConfigValueKind::kInteger, false,
                            4, 1.0, 4096.0},
      ConfigFieldDefinition{"embedding_dim", ConfigValueKind::kInteger, false,
                            128, 1.0, 65536.0},
      ConfigFieldDefinition{"device_id", ConfigValueKind::kInteger, false, -1,
                            -1.0, 1024.0}};
  def.thread_model = EngineThreadModel::kSerialized;
  return def;
}

REGISTER_ENGINE_WITH_DEFINITION(MockNpuEmbeddingEngine,
                                MakeMockNpuEmbeddingDefinition());

}  // namespace alg_framework
