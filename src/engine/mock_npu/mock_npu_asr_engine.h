#pragma once

#include <string>
#include <vector>

#include "engine/engine_interface.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

class MockNpuAsrEngine : public IAudioAsrEngine {
 public:
  MockNpuAsrEngine();
  ~MockNpuAsrEngine() override = default;

  bool Load(const std::string& model_path,
            const nlohmann::json& engine_config) override;
  size_t GetMaxBatchSize() const override { return max_batch_size_; }
  const std::string& EngineType() const override;

  int InferTraceableBatch(
      const std::vector<TraceableItem<AudioPcmData>>& input_audio,
      std::vector<TraceableItem<std::string>>* output_transcripts) override;

 private:
  int RawNpuAsrHardwareInfer(const std::vector<AudioPcmData>& batch_audio,
                             std::vector<std::string>* batch_transcripts);

 private:
  std::string model_path_;
  size_t max_batch_size_ = 2;
  bool is_loaded_ = false;
};

}  // namespace alg_framework
