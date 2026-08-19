#include "engine/mock_npu/mock_npu_rerank_engine.h"

#include <algorithm>
#include <iostream>

#include "engine/engine_registry.h"

namespace alg_framework {

MockNpuRerankEngine::MockNpuRerankEngine() = default;

bool MockNpuRerankEngine::Load(const std::string& model_path,
                               const nlohmann::json& engine_config) {
  model_path_ = model_path;
  max_batch_size_ = engine_config.value("max_batch_size", 4);
  device_id_ = engine_config.value("device_id", -1);
  is_loaded_ = true;
  std::cout << "[MockNpuRerankEngine] Loaded Cross-Encoder Reranker from: "
            << model_path << ", Fixed MaxBatchSize: " << max_batch_size_
            << ", Device: " << device_id_ << std::endl;
  return true;
}

const std::string& MockNpuRerankEngine::EngineType() const {
  static std::string type = "mock_npu_rerank";
  return type;
}

int MockNpuRerankEngine::ScoreTraceableBatch(
    const std::vector<TraceableItem<PairInput>>& input_pairs,
    std::vector<TraceableItem<float>>* output_scores) {
  if (!is_loaded_) return -7001;

  PairInput dummy_pad = {"<PAD_QUERY>", "<PAD_PASSAGE>"};

  return FixedBatchExecutor::Execute<PairInput, float>(
      input_pairs, max_batch_size_, dummy_pad,
      [this](const std::vector<PairInput>& batch_in,
             std::vector<float>* batch_out) {
        return this->RawNpuHardwareRerankInfer(batch_in, batch_out);
      },
      output_scores);
}

int MockNpuRerankEngine::RawNpuHardwareRerankInfer(
    const std::vector<PairInput>& batch_inputs,
    std::vector<float>* batch_outputs) {
  if (batch_inputs.size() != max_batch_size_) {
    std::cerr << "[MockNpuRerankEngine] HARDWARE ERROR: Batch size "
              << batch_inputs.size() << " != Fixed MaxBatch " << max_batch_size_
              << std::endl;
    return -7002;
  }

  std::cout
      << "  [NPU Hardware] Executing Cross-Encoder Rerank kernel with batch="
      << max_batch_size_ << std::endl;
  batch_outputs->resize(max_batch_size_);

  for (size_t i = 0; i < max_batch_size_; ++i) {
    if (batch_inputs[i].query == "<PAD_QUERY>") {
      (*batch_outputs)[i] = 0.0f;
    } else {
      (*batch_outputs)[i] = ComputePairScore(batch_inputs[i]);
    }
  }

  return 0;
}

float MockNpuRerankEngine::ComputePairScore(const PairInput& pair) {
  // 模拟 Cross-Encoder 计算 query 与 passage 的词重叠与深度交叉语义
  if (pair.query.empty() || pair.passage.empty()) return 0.0f;

  float overlap_score = 0.0f;
  std::vector<std::string> test_words = {"违规", "欺诈", "退款", "敏感",
                                         "泄密", "账号", "密码", "保密",
                                         "合规", "禁止"};
  for (const auto& w : test_words) {
    if (pair.query.find(w) != std::string::npos &&
        pair.passage.find(w) != std::string::npos) {
      overlap_score += 0.35f;
    }
  }

  return std::min(1.0f, 0.2f + overlap_score);
}

REGISTER_ENGINE("mock_npu_rerank", MockNpuRerankEngine);

}  // namespace alg_framework
