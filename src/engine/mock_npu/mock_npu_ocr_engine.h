#pragma once

#include <string>
#include <vector>

#include "engine/engine_interface.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

class MockNpuOcrEngine : public IOcrEngine {
 public:
  MockNpuOcrEngine();
  ~MockNpuOcrEngine() override = default;

  bool Load(const std::string& model_path,
            const nlohmann::json& engine_config) override;
  size_t GetMaxBatchSize() const override { return max_batch_size_; }
  const std::string& EngineType() const override;
  const std::string& GetLoadedModelPath() const override { return model_path_; }
  int GetDeviceId() const override { return device_id_; }

  int InferTraceableBatch(
      const std::vector<TraceableItem<std::string>>& input_image_paths,
      std::vector<TraceableItem<std::vector<OcrBoxItem>>>* output_boxes)
      override;

 private:
  int RawNpuOcrHardwareInfer(const std::vector<std::string>& batch_images,
                             std::vector<std::vector<OcrBoxItem>>* batch_boxes);

 private:
  std::string model_path_;
  int device_id_ = -1;
  size_t max_batch_size_ = 2;
  bool is_loaded_ = false;
};

}  // namespace alg_framework
