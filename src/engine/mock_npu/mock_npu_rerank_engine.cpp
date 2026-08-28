#include "engine/mock_npu/mock_npu_rerank_engine.h"

#include <algorithm>

#include "company_alg_log.h"
#include "engine/engine_registry.h"

namespace alg_framework {

MockNpuRerankEngine::MockNpuRerankEngine() = default;

bool MockNpuRerankEngine::Load(const std::string& model_path,
                               const nlohmann::json& engine_config) {
  model_path_ = model_path;
  max_batch_size_ = engine_config.value("max_batch_size", 4);
  device_id_ = engine_config.value("device_id", -1);
  is_loaded_ = true;
  ALG_LOG_INFO(
      "[MockNpuRerankEngine] Loaded Cross-Encoder Reranker from: %s, Fixed "
      "MaxBatchSize: %zu, Device: %d\n",
      model_path.c_str(), max_batch_size_, device_id_);
  return true;
}

const std::string& MockNpuRerankEngine::EngineType() const {
  static const std::string type = kEngineType;
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
    ALG_LOG_ERROR(
        "[MockNpuRerankEngine] HARDWARE ERROR: Batch size %zu != Fixed "
        "MaxBatch %zu\n",
        batch_inputs.size(), max_batch_size_);
    return -7002;
  }

  ALG_LOG_DEBUG(
      "  [NPU Hardware] Executing Cross-Encoder Rerank kernel with "
      "batch=%zu\n",
      max_batch_size_);
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

EngineDefinition MakeMockNpuRerankDefinition() {
  EngineDefinition def;
  def.engine_type = MockNpuRerankEngine::kEngineType;
  def.capability = "rerank";
  def.description = "Mock NPU rerank engine";
  def.config_fields = {
      ConfigFieldDefinition{"max_batch_size", ConfigValueKind::kInteger, false,
                            4, 1.0, 4096.0},
      ConfigFieldDefinition{"device_id", ConfigValueKind::kInteger, false, -1,
                            -1.0, 1024.0}};
  def.thread_model = EngineThreadModel::kSerialized;
  return def;
}

REGISTER_ENGINE_WITH_DEFINITION(MockNpuRerankEngine,
                                MakeMockNpuRerankDefinition());

}  // namespace alg_framework
