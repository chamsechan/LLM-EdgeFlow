#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "engine/backend_interface.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace alg_framework {

/**
 * @brief Qwen ChatML and autoregressive generation semantics over a neutral
 * Causal LM backend session.
 *
 * This model deliberately owns no llama.cpp or other vendor resource. Token
 * encoding, sequence state and logits evaluation are provided exclusively by
 * ICausalLmSession.
 */
class QwenCausalLmModel final : public ILlmModel {
 public:
  inline static constexpr char kModelType[] = "qwen_causal_lm";
  inline static constexpr char kCapability[] = "llm";

  static std::shared_ptr<IModel> Create(const ModelCreateContext& ctx,
                                        std::string* diagnostic);

  QwenCausalLmModel(std::shared_ptr<ICausalLmSession> session,
                    std::string system_prompt, bool add_bos,
                    int64_t random_seed);
  ~QwenCausalLmModel() override = default;

  const std::string& ModelType() const noexcept override;
  const std::string& Capability() const noexcept override;
  InferenceConcurrency Concurrency() const noexcept override;
  size_t GetMaxBatchSize() const noexcept override;

  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs) noexcept override;

  static void StripIncompleteUtf8Suffix(std::string* text) noexcept;

 private:
  int GenerateOne(const TraceableItem<std::string>& prompt,
                  const GenerateOptions& options, std::string* output) noexcept;
  std::string ApplyChatTemplate(const std::string& prompt) const;

  std::shared_ptr<ICausalLmSession> session_;
  std::string system_prompt_;
  bool add_bos_ = false;
  int64_t random_seed_ = -1;
};

}  // namespace alg_framework
