#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/engine_interface.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

class OnnxRerankEngine : public IRerankEngine {
 public:
  OnnxRerankEngine();
  ~OnnxRerankEngine() override = default;

  bool Load(const std::string& model_path,
            const nlohmann::json& engine_config) override;
  size_t GetMaxBatchSize() const override { return max_batch_size_; }
  const std::string& EngineType() const override;

  int ScoreTraceableBatch(
      const std::vector<TraceableItem<PairInput>>& input_pairs,
      std::vector<TraceableItem<float>>* output_scores) override;

 private:
  int RawOnnxRerankHardwareInfer(const std::vector<PairInput>& batch_inputs,
                                 std::vector<float>* batch_outputs);

 private:
  std::string model_path_;
  size_t max_batch_size_ = 4;
  bool is_loaded_ = false;
};

}  // namespace alg_framework
