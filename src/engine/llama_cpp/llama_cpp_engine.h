#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/engine_interface.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

/**
 * @brief 基于开源 llama.cpp (ggml) 的 GGUF 自回归大语言模型推理引擎
 *
 * 架构隔离性：
 * - 继承并实现 Layer 4 的 ILlmEngine 纯虚接口；
 * - 内部封装 GGUF
 * 模型加载、量化权重视图与自回归采样，上层算子不依赖三方头文件；
 * - 支持 FixedBatchExecutor 固定批次调度与 (req_id, sub_id) 溯源。
 */
class LlamaCppEngine : public ILlmEngine {
 public:
  inline static constexpr char kEngineType[] = "llama_cpp";

  LlamaCppEngine();
  ~LlamaCppEngine() override;

  bool Load(const std::string& model_path,
            const nlohmann::json& engine_config) override;
  size_t GetMaxBatchSize() const override { return max_batch_size_; }
  const std::string& EngineType() const override;
  const std::string& GetLoadedModelPath() const override { return model_path_; }
  int GetDeviceId() const override { return device_id_; }

  int Generate(const std::string& prompt, const GenerateOption& opt,
               std::string* output_text) override;

  int InferTraceableBatch(
      const std::vector<TraceableItem<std::string>>& input_prompts,
      const GenerateOption& option,
      std::vector<TraceableItem<std::string>>* output_texts) override;

  /**
   * @brief 剔除由于 max_tokens 截断可能导致字符串末尾残留的不完整多字节 UTF-8
   * 序列
   */
  static void StripIncompleteUtf8Trailing(std::string& s);

 private:
  int RawLlamaHardwareInfer(const std::vector<std::string>& batch_prompts,
                            const GenerateOption& option,
                            std::vector<std::string>* batch_outputs);

  std::string GenerateLlamaResponse(const std::string& prompt,
                                    const GenerateOption& option);

 private:
  std::string model_path_;
  int device_id_ = -1;
  size_t max_batch_size_ = 2;
  size_t max_seq_len_ = 1024;
  bool is_loaded_ = false;

  // 内部 PIMPL 状态（隐藏 llama.cpp 数据结构）
  struct Impl;
  std::unique_ptr<Impl> pimpl_;
};

}  // namespace alg_framework
