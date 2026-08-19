#include "engine/onnx/onnx_rerank_engine.h"

#include <cmath>
#include <iostream>
#include <vector>

#include "engine/engine_registry.h"

namespace alg_framework {

OnnxRerankEngine::OnnxRerankEngine() = default;

bool OnnxRerankEngine::Load(const std::string& model_path,
                            const nlohmann::json& engine_config) {
  model_path_ = model_path;
  max_batch_size_ = engine_config.value("max_batch_size", 4);
  device_id_ = engine_config.value("device_id", -1);
  is_loaded_ = true;
  std::cout << "[OnnxRerankEngine] Loaded ONNX Cross-Encoder Reranker from: "
            << model_path_ << ", Fixed MaxBatchSize: " << max_batch_size_
            << ", Device: " << device_id_ << std::endl;
  return true;
}

const std::string& OnnxRerankEngine::EngineType() const {
  static std::string type = "onnx_rerank";
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
    std::cerr << "[OnnxRerankEngine] HARDWARE ERROR: Batch size "
              << batch_inputs.size() << " != Fixed MaxBatch " << max_batch_size_
              << std::endl;
    return -9202;
  }

  std::cout << "  [ONNX Rerank Engine] Executing Cross-Encoder Rerank kernel "
               "with batch="
            << max_batch_size_ << std::endl;

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

REGISTER_ENGINE("onnx_rerank", OnnxRerankEngine);

}  // namespace alg_framework
