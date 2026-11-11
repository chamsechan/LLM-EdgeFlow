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
 * 本 Model 只负责 prompt 格式化、种子处理和来源追踪。分词、采样、生成循环和厂商
 * 资源都位于统一的 ITextGenerationSession 边界之下。
 */
class QwenCausalLmModel final
    : public ModelIdentity<QwenCausalLmModel, ILlmModel> {
 public:
  inline static constexpr char kImplName[] = "qwen_causal_lm";
  static constexpr InferenceConcurrency kConcurrency =
      InferenceConcurrency::kConcurrent;

  static std::shared_ptr<IModel> Create(const ModelCreateContext& ctx,
                                        std::string* diagnostic);

  QwenCausalLmModel(std::shared_ptr<ITextGenerationSession> session,
                    bool add_bos);
  ~QwenCausalLmModel() override = default;

  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs,
               std::string* diagnostic = nullptr) noexcept override;

 private:
  /**
   * @brief 为单条输入套用 ChatML 模板；显式设置非负 random_seed 且 Session
   * 支持时，按来源编号派生随机种子。调用文本生成协议后移除尾部不完整的 UTF-8
   * 序列；失败时清空该条输出。模板和种子语义在 Model 层确定，Backend
   * 接收已格式化的 prompt。
   */
  int GenerateOne(const TraceableItem<std::string>& prompt,
                  const GenerateOptions& options, std::string* output,
                  std::string* diagnostic) noexcept;
  std::string ApplyChatTemplate(const std::string& prompt,
                                const std::string& system_prompt) const;

  std::shared_ptr<ITextGenerationSession> session_;
  bool add_bos_ = false;
};

}  // namespace llm_edgeflow
