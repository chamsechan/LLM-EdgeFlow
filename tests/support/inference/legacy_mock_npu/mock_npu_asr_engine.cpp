#include "tests/support/inference/legacy_mock_npu/mock_npu_asr_engine.h"

#include <vector>

#include "company_alg_log.h"
#include "engine/engine_registry.h"

namespace alg_framework {

MockNpuAsrEngine::MockNpuAsrEngine() = default;

bool MockNpuAsrEngine::Load(const std::string& model_path,
                            const nlohmann::json& engine_config) {
  model_path_ = model_path;
  max_batch_size_ = engine_config.value("max_batch_size", 2);
  device_id_ = engine_config.value("device_id", -1);
  is_loaded_ = true;
  ALG_LOG_INFO(
      "[MockNpuAsrEngine] Loaded Speech ASR model from: %s, Fixed "
      "MaxBatchSize: %zu, Device: %d\n",
      model_path_.c_str(), max_batch_size_, device_id_);
  return true;
}

const std::string& MockNpuAsrEngine::EngineType() const {
  static const std::string type = kEngineType;
  return type;
}

const std::string& MockNpuAsrEngine::ModelType() const noexcept {
  static const std::string type = kEngineType;
  return type;
}

const std::string& MockNpuAsrEngine::Capability() const noexcept {
  static const std::string capability = "asr";
  return capability;
}

InferenceConcurrency MockNpuAsrEngine::Concurrency() const noexcept {
  return InferenceConcurrency::kSerialized;
}

int MockNpuAsrEngine::Transcribe(const AudioPcmBatch& audio,
                                 TextBatch* outputs) noexcept {
  if (!outputs) return -1;
  outputs->clear();
  try {
    std::vector<TraceableItem<AudioPcmData>> legacy_audio;
    legacy_audio.reserve(audio.size());
    for (const auto& item : audio) {
      legacy_audio.emplace_back(
          item.req_id, item.sub_id,
          AudioPcmData{item.data.pcm_data, item.data.sample_rate});
    }
    return InferTraceableBatch(legacy_audio, outputs);
  } catch (...) {
    outputs->clear();
    return -1;
  }
}

int MockNpuAsrEngine::InferTraceableBatch(
    const std::vector<TraceableItem<AudioPcmData>>& input_audio,
    std::vector<TraceableItem<std::string>>* output_transcripts) {
  if (!is_loaded_) return -8001;

  AudioPcmData dummy_pad;
  dummy_pad.pcm_data = {0.0f, 0.0f, 0.0f, 0.0f};

  return FixedBatchExecutor::Execute<AudioPcmData, std::string>(
      input_audio, max_batch_size_, dummy_pad,
      [this](const std::vector<AudioPcmData>& batch_in,
             std::vector<std::string>* batch_out) {
        return this->RawNpuAsrHardwareInfer(batch_in, batch_out);
      },
      output_transcripts);
}

int MockNpuAsrEngine::RawNpuAsrHardwareInfer(
    const std::vector<AudioPcmData>& batch_audio,
    std::vector<std::string>* batch_transcripts) {
  if (batch_audio.size() != max_batch_size_) {
    ALG_LOG_ERROR(
        "[MockNpuAsrEngine] HARDWARE ERROR: Batch size %zu != Fixed MaxBatch "
        "%zu\n",
        batch_audio.size(), max_batch_size_);
    return -8002;
  }

  ALG_LOG_DEBUG(
      "  [NPU Hardware] Executing NPU Audio ASR Speech-to-Text kernel with "
      "batch=%zu\n",
      max_batch_size_);

  batch_transcripts->resize(max_batch_size_);
  for (size_t i = 0; i < max_batch_size_; ++i) {
    if (batch_audio[i].pcm_data.size() <= 4) {
      (*batch_transcripts)[i] = "";
    } else {
      // 模拟语音转文字
      float sum = 0.0f;
      for (float val : batch_audio[i].pcm_data) sum += val;
      if (sum > 120.0f) {
        (*batch_transcripts)[i] = "帮我导航到清华科技园，避开拥堵路段。";
      } else if (sum > 40.0f) {
        (*batch_transcripts)[i] = "今天北京天气怎么样？";
      } else {
        (*batch_transcripts)[i] = "把空调温度调到24度，风量开到二档。";
      }
    }
  }

  return 0;
}

EngineDefinition MakeMockNpuAsrDefinition() {
  EngineDefinition def;
  def.engine_type = MockNpuAsrEngine::kEngineType;
  def.capability = "asr";
  def.description = "Mock NPU ASR engine";
  def.config_fields = {
      ConfigFieldDefinition{"max_batch_size", ConfigValueKind::kInteger, false,
                            2, 1.0, 4096.0},
      ConfigFieldDefinition{"device_id", ConfigValueKind::kInteger, false, -1,
                            -1.0, 1024.0}};
  def.thread_model = EngineThreadModel::kSerialized;
  return def;
}

REGISTER_ENGINE_WITH_DEFINITION(MockNpuAsrEngine, MakeMockNpuAsrDefinition());

}  // namespace alg_framework
