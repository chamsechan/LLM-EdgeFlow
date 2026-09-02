#include "engine/models/qwen_causal_lm/qwen_causal_lm_model.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/text/utf8.h"

namespace alg_framework {
namespace {

uint64_t MixSeed(uint64_t seed, uint32_t req_id, uint32_t sub_id) noexcept {
  seed ^= static_cast<uint64_t>(req_id) + 0x9e3779b97f4a7c15ULL + (seed << 6U) +
          (seed >> 2U);
  seed ^= static_cast<uint64_t>(sub_id) + 0x9e3779b97f4a7c15ULL + (seed << 6U) +
          (seed >> 2U);
  return seed;
}

}  // namespace

std::shared_ptr<IModel> QwenCausalLmModel::Create(const ModelCreateContext& ctx,
                                                  std::string* diagnostic) {
  try {
    auto session =
        std::dynamic_pointer_cast<ITextGenerationSession>(ctx.backend_session);
    if (!session || session->Protocol() != ExecutionProtocol::kTextGeneration) {
      if (diagnostic) {
        *diagnostic =
            "Backend session does not implement the text-generation protocol";
      }
      return nullptr;
    }
    const BatchPolicy policy = session->GetBatchPolicy();
    if (policy.max_batch_size == 0 || policy.fixed_batch_size != 0) {
      if (diagnostic) {
        *diagnostic =
            "Text-generation session must expose a non-fixed batch policy";
      }
      return nullptr;
    }

    const std::string chat_template =
        ctx.model_config.value("chat_template", "qwen_chatml");
    if (chat_template != "qwen_chatml") {
      if (diagnostic) *diagnostic = "Unsupported Qwen chat template";
      return nullptr;
    }
    const std::string system_prompt =
        ctx.model_config.value("system_prompt", "");
    const bool add_bos = ctx.model_config.value("add_bos", false);
    const int64_t random_seed =
        ctx.model_config.value("random_seed", int64_t{-1});
    if (random_seed < -1) {
      if (diagnostic) *diagnostic = "random_seed must be -1 or non-negative";
      return nullptr;
    }
    return std::make_shared<QwenCausalLmModel>(
        std::move(session), system_prompt, add_bos, random_seed);
  } catch (const std::exception& e) {
    if (diagnostic) {
      *diagnostic = std::string("Qwen model creation exception: ") + e.what();
    }
    return nullptr;
  } catch (...) {
    if (diagnostic) *diagnostic = "Unknown Qwen model creation exception";
    return nullptr;
  }
}

QwenCausalLmModel::QwenCausalLmModel(
    std::shared_ptr<ITextGenerationSession> session, std::string system_prompt,
    bool add_bos, int64_t random_seed)
    : session_(std::move(session)),
      system_prompt_(std::move(system_prompt)),
      add_bos_(add_bos),
      random_seed_(random_seed) {}

const std::string& QwenCausalLmModel::ModelType() const noexcept {
  static const std::string type = kModelType;
  return type;
}

const std::string& QwenCausalLmModel::Capability() const noexcept {
  static const std::string capability = kCapability;
  return capability;
}

InferenceConcurrency QwenCausalLmModel::Concurrency() const noexcept {
  return InferenceConcurrency::kConcurrent;
}

size_t QwenCausalLmModel::GetMaxBatchSize() const noexcept {
  return session_ ? session_->GetBatchPolicy().max_batch_size : 0;
}

std::string QwenCausalLmModel::ApplyChatTemplate(
    const std::string& prompt) const {
  std::string formatted;
  if (!system_prompt_.empty()) {
    formatted += "<|im_start|>system\n";
    formatted += system_prompt_;
    formatted += "<|im_end|>\n";
  }
  formatted += "<|im_start|>user\n";
  formatted += prompt;
  formatted += "<|im_end|>\n<|im_start|>assistant\n";
  return formatted;
}

int QwenCausalLmModel::Generate(const TextBatch& prompts,
                                const GenerateOptions& options,
                                TextBatch* outputs) noexcept {
  if (!outputs) return -1;
  outputs->clear();
  if (prompts.empty()) return 0;
  if (!session_) return -1;

  const BatchPolicy policy = session_->GetBatchPolicy();
  if (policy.max_batch_size == 0 || policy.fixed_batch_size != 0) return -1;
  return FixedBatchExecutor::Execute<std::string, std::string>(
      prompts, policy,
      [this, &prompts, &options](const BatchSlice& slice,
                                 std::vector<std::string>* batch_outputs) {
        if (!batch_outputs) return -1;
        batch_outputs->clear();
        try {
          batch_outputs->reserve(slice.valid_count);
          for (size_t i = 0; i < slice.valid_count; ++i) {
            std::string output;
            const int result =
                GenerateOne(prompts[slice.offset + i], options, &output);
            if (result != 0) {
              batch_outputs->clear();
              return result;
            }
            batch_outputs->push_back(std::move(output));
          }
          return 0;
        } catch (...) {
          batch_outputs->clear();
          return -1;
        }
      },
      outputs);
}

int QwenCausalLmModel::GenerateOne(const TraceableItem<std::string>& prompt,
                                   const GenerateOptions& options,
                                   std::string* output) noexcept {
  if (!output) return -1;
  output->clear();
  if (!session_ || prompt.data.empty()) return -1;

  try {
    std::optional<uint64_t> seed;
    if (random_seed_ >= 0) {
      seed = MixSeed(static_cast<uint64_t>(random_seed_), prompt.req_id,
                     prompt.sub_id);
    }
    std::string diagnostic;
    const int result =
        session_->Generate(ApplyChatTemplate(prompt.data), add_bos_, options,
                           seed, output, &diagnostic);
    if (result != 0) {
      output->clear();
      ALG_LOG_ERROR("[QwenCausalLmModel] Generate failed: %s\n",
                    diagnostic.c_str());
      return result;
    }
    utf8::StripIncompleteSuffix(output);
    return 0;
  } catch (const std::exception& e) {
    ALG_LOG_ERROR("[QwenCausalLmModel] Generate exception: %s\n", e.what());
    output->clear();
    return -1;
  } catch (...) {
    ALG_LOG_ERROR("[QwenCausalLmModel] Unknown Generate exception\n");
    output->clear();
    return -1;
  }
}

void QwenCausalLmModel::StripIncompleteUtf8Suffix(std::string* text) noexcept {
  utf8::StripIncompleteSuffix(text);
}

static const ModelDefinition kQwenCausalLmModelDefinition = [] {
  ModelDefinition definition;
  definition.model_type = QwenCausalLmModel::kModelType;
  definition.capability = QwenCausalLmModel::kCapability;
  definition.description =
      "Qwen ChatML model using the unified text-generation protocol";
  definition.required_protocol = ExecutionProtocol::kTextGeneration;
  definition.concurrency = InferenceConcurrency::kConcurrent;
  definition.config_fields = {
      {"chat_template",
       ConfigValueKind::kString,
       false,
       "qwen_chatml",
       std::nullopt,
       std::nullopt,
       {"qwen_chatml"}},
      {"system_prompt", ConfigValueKind::kString, false, ""},
      {"add_bos", ConfigValueKind::kBoolean, false, false},
      {"random_seed", ConfigValueKind::kInteger, false, -1, -1.0,
       static_cast<double>(std::numeric_limits<int32_t>::max())},
  };
  return definition;
}();

REGISTER_MODEL_WITH_DEFINITION(QwenCausalLmModel, kQwenCausalLmModelDefinition);

}  // namespace alg_framework
