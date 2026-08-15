#include "engine/mock_npu/mock_npu_asr_engine.h"

#include <iostream>
#include <vector>

#include "engine/engine_registry.h"

namespace alg_framework {

MockNpuAsrEngine::MockNpuAsrEngine() = default;

bool MockNpuAsrEngine::Load(const std::string& model_path,
                            const nlohmann::json& engine_config) {
  model_path_ = model_path;
  max_batch_size_ = engine_config.value("max_batch_size", 2);
  is_loaded_ = true;
  std::cout << "[MockNpuAsrEngine] Loaded Speech ASR model from: "
            << model_path_ << ", Fixed MaxBatchSize: " << max_batch_size_
            << std::endl;
  return true;
}

const std::string& MockNpuAsrEngine::EngineType() const {
  static std::string type = "mock_npu_asr";
  return type;
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
    std::cerr << "[MockNpuAsrEngine] HARDWARE ERROR: Batch size "
              << batch_audio.size() << " != Fixed MaxBatch " << max_batch_size_
              << std::endl;
    return -8002;
  }

  std::cout << "  [NPU Hardware] Executing NPU Audio ASR Speech-to-Text kernel "
               "with batch="
            << max_batch_size_ << std::endl;

  batch_transcripts->resize(max_batch_size_);
  for (size_t i = 0; i < max_batch_size_; ++i) {
    if (batch_audio[i].pcm_data.size() <= 4) {
      (*batch_transcripts)[i] = "";
    } else {
      // 模拟语音转文字
      float sum = 0.0f;
      for (float val : batch_audio[i].pcm_data) sum += val;
      if (sum > 50.0f) {
        (*batch_transcripts)[i] = "帮我导航到清华科技园，避开拥堵路段。";
      } else {
        (*batch_transcripts)[i] = "把空调温度调到24度，风量开到二档。";
      }
    }
  }

  return 0;
}

REGISTER_ENGINE("mock_npu_asr", MockNpuAsrEngine);

}  // namespace alg_framework
