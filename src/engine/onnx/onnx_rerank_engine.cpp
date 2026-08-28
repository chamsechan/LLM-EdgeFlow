#include "engine/onnx/onnx_rerank_engine.h"

#include <cmath>
#include <vector>

#include "company_alg_log.h"
#include "engine/engine_registry.h"

namespace alg_framework {

OnnxRerankEngine::OnnxRerankEngine() = default;

bool OnnxRerankEngine::Load(const std::string& model_path,
                            const nlohmann::json& engine_config) {
  model_path_ = model_path;
  max_batch_size_ = engine_config.value("max_batch_size", 4);
  device_id_ = engine_config.value("device_id", -1);
  is_loaded_ = true;
  ALG_LOG_INFO(
      "[OnnxRerankEngine] Loaded ONNX Cross-Encoder Reranker from: %s, Fixed "
      "MaxBatchSize: %zu, Device: %d\n",
      model_path_.c_str(), max_batch_size_, device_id_);
  return true;
}

const std::string& OnnxRerankEngine::EngineType() const {
  static const std::string type = kEngineType;
  return type;
}

int OnnxRerankEngine::ScoreTraceableBatch(
    const std::vector<TraceableItem<PairInput>>& input_pairs,
    std::vector<TraceableItem<float>>* output_scores) {
  if (!is_loaded_) return -9201;

  PairInput dummy_pad = {"<PAD_QUERY>", "<PAD_PASSAGE>"};

  return FixedBatchExecutor::Execute<PairInput, float>(
      input_pairs, max_batch_size_, dummy_pad,
      [this](const std::vector<PairInput>& batch_in,
             std::vector<float>* batch_out) {
        return this->RawOnnxRerankHardwareInfer(batch_in, batch_out);
      },
      output_scores);
}

int OnnxRerankEngine::RawOnnxRerankHardwareInfer(
    const std::vector<PairInput>& batch_inputs,
    std::vector<float>* batch_outputs) {
  if (batch_inputs.size() != max_batch_size_) {
    ALG_LOG_ERROR(
        "[OnnxRerankEngine] HARDWARE ERROR: Batch size %zu != Fixed MaxBatch "
        "%zu\n",
        batch_inputs.size(), max_batch_size_);
    return -9202;
  }

  ALG_LOG_DEBUG(
      "  [ONNX Rerank Engine] Executing Cross-Encoder Rerank kernel with "
      "batch=%zu\n",
      max_batch_size_);

  batch_outputs->resize(max_batch_size_);
  for (size_t i = 0; i < max_batch_size_; ++i) {
    if (batch_inputs[i].query == "<PAD_QUERY>") {
      (*batch_outputs)[i] = -999.0f;
    } else {
      // 模拟计算 query 和 passage 的语义相关性
      size_t q_hash = std::hash<std::string>{}(batch_inputs[i].query);
      size_t p_hash = std::hash<std::string>{}(batch_inputs[i].passage);
      float score = 0.5f + 0.4f * std::cos(static_cast<float>(q_hash ^ p_hash));
      (*batch_outputs)[i] = score;
    }
  }

  return 0;
}

EngineDefinition MakeOnnxRerankDefinition() {
  EngineDefinition def;
  def.engine_type = OnnxRerankEngine::kEngineType;
  def.capability = "rerank";
  def.description = "ONNX Runtime rerank engine";
  def.config_fields = {
      ConfigFieldDefinition{"max_batch_size", ConfigValueKind::kInteger, false,
                            4, 1.0, 4096.0},
      ConfigFieldDefinition{"device_id", ConfigValueKind::kInteger, false, -1,
                            -1.0, 1024.0}};
  def.thread_model = EngineThreadModel::kConcurrent;
  return def;
}

REGISTER_ENGINE_WITH_DEFINITION(OnnxRerankEngine, MakeOnnxRerankDefinition());

}  // namespace alg_framework
