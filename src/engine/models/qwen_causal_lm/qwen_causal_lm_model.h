#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "engine/backend_interface.h"
#include "engine/model_identity.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

/**
 * @brief 基于中性文本生成会话的 Qwen ChatML 语义。
 *
 * 本 Model 只负责 prompt 格式化和来源追踪。分词、采样、生成循环和厂商
 * 资源都位于统一的 ITextGenerationSession 边界之下。
 */
class QwenCausalLmModel final
    : public ModelIdentity<QwenCausalLmModel, ILlmModel> {
 public:
  inline static constexpr char kModelType[] = "qwen_causal_lm";
  static constexpr InferenceConcurrency kConcurrency =
      InferenceConcurrency::kConcurrent;

  static std::shared_ptr<IModel> Create(const ModelCreateContext& ctx,
                                        std::string* diagnostic);

  QwenCausalLmModel(std::shared_ptr<ITextGenerationSession> session,
                    std::string system_prompt, bool add_bos,
                    int64_t random_seed);
  ~QwenCausalLmModel() override = default;

  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs,
               std::string* diagnostic = nullptr) noexcept override;

 private:
  int GenerateOne(const TraceableItem<std::string>& prompt,
                  const GenerateOptions& options, std::string* output,
                  std::string* diagnostic) noexcept;
  std::string ApplyChatTemplate(const std::string& prompt) const;

  std::shared_ptr<ITextGenerationSession> session_;
  std::string system_prompt_;
  bool add_bos_ = false;
  int64_t random_seed_ = -1;
};

}  // namespace llm_edgeflow
