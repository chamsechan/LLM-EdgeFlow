#pragma once

#include <string>
#include <vector>

#include "engine/engine_interface.h"
#include "engine/fixed_batch_executor.h"
#include "engine/model_interface.h"

namespace alg_framework {

class MockNpuAsrEngine : public IAudioAsrEngine, public IAsrModel {
 public:
  inline static constexpr char kEngineType[] = "mock_npu_asr";

  MockNpuAsrEngine();
  ~MockNpuAsrEngine() override = default;

  bool Load(const std::string& model_path,
            const nlohmann::json& engine_config) override;
  size_t GetMaxBatchSize() const noexcept override { return max_batch_size_; }
  const std::string& EngineType() const override;
  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  const std::string& GetLoadedModelPath() const override { return model_path_; }
  int GetDeviceId() const override { return device_id_; }

  int InferTraceableBatch(
      const std::vector<TraceableItem<AudioPcmData>>& input_audio,
      std::vector<TraceableItem<std::string>>* output_transcripts) override;

  int Transcribe(const AudioPcmBatch& audio,
                 TextBatch* outputs) noexcept override;

 private:
  int RawNpuAsrHardwareInfer(const std::vector<AudioPcmData>& batch_audio,
                             std::vector<std::string>* batch_transcripts);

 private:
  std::string model_path_;
  int device_id_ = -1;
  size_t max_batch_size_ = 2;
  bool is_loaded_ = false;
};

}  // namespace alg_framework
