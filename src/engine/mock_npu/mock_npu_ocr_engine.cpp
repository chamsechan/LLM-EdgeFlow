#include "engine/mock_npu/mock_npu_ocr_engine.h"

#include <vector>

#include "company_alg_log.h"
#include "engine/engine_registry.h"

namespace alg_framework {

MockNpuOcrEngine::MockNpuOcrEngine() = default;

bool MockNpuOcrEngine::Load(const std::string& model_path,
                            const nlohmann::json& engine_config) {
  model_path_ = model_path;
  max_batch_size_ = engine_config.value("max_batch_size", 2);
  device_id_ = engine_config.value("device_id", -1);
  is_loaded_ = true;
  ALG_LOG_INFO(
      "[MockNpuOcrEngine] Loaded OCR Detection/Recog model from: %s, Fixed "
      "MaxBatchSize: %zu, Device: %d\n",
      model_path_.c_str(), max_batch_size_, device_id_);
  return true;
}

const std::string& MockNpuOcrEngine::EngineType() const {
  static const std::string type = kEngineType;
  return type;
}

int MockNpuOcrEngine::InferTraceableBatch(
    const std::vector<TraceableItem<std::string>>& input_image_paths,
    std::vector<TraceableItem<std::vector<OcrBoxItem>>>* output_boxes) {
  if (!is_loaded_) return -7001;

  std::string dummy_pad = "<DUMMY_IMAGE_PAD>";

  return FixedBatchExecutor::Execute<std::string, std::vector<OcrBoxItem>>(
      input_image_paths, max_batch_size_, dummy_pad,
      [this](const std::vector<std::string>& batch_in,
             std::vector<std::vector<OcrBoxItem>>* batch_out) {
        return this->RawNpuOcrHardwareInfer(batch_in, batch_out);
      },
      output_boxes);
}

int MockNpuOcrEngine::RawNpuOcrHardwareInfer(
    const std::vector<std::string>& batch_images,
    std::vector<std::vector<OcrBoxItem>>* batch_outputs) {
  if (batch_images.size() != max_batch_size_) {
    ALG_LOG_ERROR(
        "[MockNpuOcrEngine] HARDWARE ERROR: Batch size %zu != Fixed MaxBatch "
        "%zu\n",
        batch_images.size(), max_batch_size_);
    return -7002;
  }

  ALG_LOG_DEBUG(
      "  [NPU Hardware] Executing NPU OCR Detection & Recognition kernel "
      "with batch=%zu\n",
      max_batch_size_);

  batch_outputs->resize(max_batch_size_);
  for (size_t i = 0; i < max_batch_size_; ++i) {
    if (batch_images[i] == "<DUMMY_IMAGE_PAD>") {
      (*batch_outputs)[i] = {};
    } else {
      // 模拟 OCR 识别出的文字框
      std::vector<OcrBoxItem> boxes;
      boxes.push_back(
          {10.0f, 20.0f, 200.0f, 30.0f, "发票代码: 011002200111", 0.99f});
      boxes.push_back(
          {10.0f, 60.0f, 180.0f, 30.0f, "发票号码: 88765432", 0.99f});
      boxes.push_back(
          {10.0f, 100.0f, 150.0f, 30.0f, "开票日期: 2026年08月15日", 0.98f});
      boxes.push_back({10.0f, 140.0f, 220.0f, 35.0f,
                       "购买方名称: 北京某某科技有限责任公司", 0.97f});
      boxes.push_back({10.0f, 180.0f, 120.0f, 30.0f,
                       "价税合计(大写): 壹万贰仟元整", 0.99f});
      boxes.push_back(
          {10.0f, 220.0f, 100.0f, 30.0f, "小写金额: ¥12000.00", 0.99f});
      (*batch_outputs)[i] = boxes;
    }
  }

  return 0;
}

EngineDefinition MakeMockNpuOcrDefinition() {
  EngineDefinition def;
  def.engine_type = MockNpuOcrEngine::kEngineType;
  def.capability = "ocr";
  def.description = "Mock NPU OCR engine";
  def.config_fields = {
      ConfigFieldDefinition{"max_batch_size", ConfigValueKind::kInteger, false,
                            2, 1.0, 4096.0},
      ConfigFieldDefinition{"device_id", ConfigValueKind::kInteger, false, -1,
                            -1.0, 1024.0}};
  def.thread_model = EngineThreadModel::kSerialized;
  return def;
}

REGISTER_ENGINE_WITH_DEFINITION(MockNpuOcrEngine, MakeMockNpuOcrDefinition());

}  // namespace alg_framework
