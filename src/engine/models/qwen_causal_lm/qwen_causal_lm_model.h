#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "engine/backend_interface.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace alg_framework {

/**
 * @brief Qwen ChatML semantics over a neutral text-generation session.
 *
 * This Model owns prompt formatting and provenance only. Tokenization,
 * sampling, generation loops and vendor resources belong below the unified
 * ITextGenerationSession boundary.
 */
class QwenCausalLmModel final : public ILlmModel {
 public:
  inline static constexpr char kModelType[] = "qwen_causal_lm";
  inline static constexpr char kCapability[] = "llm";

  static std::shared_ptr<IModel> Create(const ModelCreateContext& ctx,
                                        std::string* diagnostic);

  QwenCausalLmModel(std::shared_ptr<ITextGenerationSession> session,
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

  std::shared_ptr<ITextGenerationSession> session_;
  std::string system_prompt_;
  bool add_bos_ = false;
  int64_t random_seed_ = -1;
};

}  // namespace alg_framework
