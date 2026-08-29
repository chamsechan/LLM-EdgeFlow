#include "engine/models/qwen_causal_lm/qwen_causal_lm_model.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

namespace {

bool ValidateOptions(const GenerateOptions& options) noexcept {
  if (options.max_tokens <= 0 || !std::isfinite(options.temperature) ||
      options.temperature < 0.0f || options.temperature > 2.0f ||
      !std::isfinite(options.top_p) || options.top_p <= 0.0f ||
      options.top_p > 1.0f) {
    return false;
  }
  return std::all_of(options.stop_words.begin(), options.stop_words.end(),
                     [](const std::string& word) { return !word.empty(); });
}

bool ValidateLogits(const std::vector<float>& logits) noexcept {
  return !logits.empty() &&
         std::all_of(logits.begin(), logits.end(),
                     [](float value) { return std::isfinite(value); });
}

int32_t GreedyToken(const std::vector<float>& logits) noexcept {
  return static_cast<int32_t>(std::distance(
      logits.begin(), std::max_element(logits.begin(), logits.end())));
}

bool SampleTopP(const std::vector<float>& logits, float temperature,
                float top_p, std::mt19937_64* rng,
                int32_t* sampled_token) noexcept {
  if (!rng || !sampled_token || !ValidateLogits(logits) ||
      temperature <= 0.01f || top_p <= 0.0f || top_p > 1.0f) {
    return false;
  }
  try {
    const double inverse_temperature = 1.0 / temperature;
    const float max_logit = *std::max_element(logits.begin(), logits.end());
    std::vector<double> probabilities(logits.size());
    double sum = 0.0;
    for (size_t i = 0; i < logits.size(); ++i) {
      const double probability = std::exp(
          (static_cast<double>(logits[i]) - max_logit) * inverse_temperature);
      if (!std::isfinite(probability)) return false;
      probabilities[i] = probability;
      sum += probability;
    }
    if (!std::isfinite(sum) || sum <= 0.0) return false;
    for (double& probability : probabilities) probability /= sum;

    std::vector<size_t> indices(probabilities.size());
    std::iota(indices.begin(), indices.end(), size_t{0});
    std::stable_sort(indices.begin(), indices.end(),
                     [&probabilities](size_t lhs, size_t rhs) {
                       return probabilities[lhs] > probabilities[rhs];
                     });

    double cumulative = 0.0;
    size_t candidate_count = 0;
    for (; candidate_count < indices.size(); ++candidate_count) {
      cumulative += probabilities[indices[candidate_count]];
      if (cumulative >= static_cast<double>(top_p)) {
        ++candidate_count;
        break;
      }
    }
    if (candidate_count == 0 || !std::isfinite(cumulative) ||
        cumulative <= 0.0) {
      return false;
    }

    std::uniform_real_distribution<double> distribution(0.0, cumulative);
    const double choice = distribution(*rng);
    double cursor = 0.0;
    size_t selected = indices[candidate_count - 1];
    for (size_t i = 0; i < candidate_count; ++i) {
      cursor += probabilities[indices[i]];
      if (choice <= cursor) {
        selected = indices[i];
        break;
      }
    }
    if (selected > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      return false;
    }
    *sampled_token = static_cast<int32_t>(selected);
    return true;
  } catch (...) {
    return false;
  }
}

uint64_t MixSeed(uint64_t seed, uint32_t req_id, uint32_t sub_id) noexcept {
  seed ^= static_cast<uint64_t>(req_id) + 0x9e3779b97f4a7c15ULL + (seed << 6U) +
          (seed >> 2U);
  seed ^= static_cast<uint64_t>(sub_id) + 0x9e3779b97f4a7c15ULL + (seed << 6U) +
          (seed >> 2U);
  return seed;
}

size_t FindFirstStop(const std::string& text,
                     const std::vector<std::string>& stop_words) noexcept {
  size_t first = std::string::npos;
  for (const auto& word : stop_words) {
    const size_t position = text.find(word);
    if (position != std::string::npos &&
        (first == std::string::npos || position < first)) {
      first = position;
    }
  }
  return first;
}

}  // namespace

std::shared_ptr<IModel> QwenCausalLmModel::Create(const ModelCreateContext& ctx,
                                                  std::string* diagnostic) {
  try {
    auto session =
        std::dynamic_pointer_cast<ICausalLmSession>(ctx.backend_session);
    if (!session || session->Protocol() != ExecutionProtocol::kCausalLm) {
      if (diagnostic) {
        *diagnostic =
            "Backend session does not implement the Causal LM protocol";
      }
      return nullptr;
    }
    const BatchPolicy policy = session->GetBatchPolicy();
    if (policy.max_batch_size == 0 || policy.fixed_batch_size != 0) {
      if (diagnostic) {
        *diagnostic = "Causal LM session must expose a non-fixed batch policy";
      }
      return nullptr;
    }
    if (session->MaxContextTokens() < 2) {
      if (diagnostic) *diagnostic = "Causal LM context is too small";
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

QwenCausalLmModel::QwenCausalLmModel(std::shared_ptr<ICausalLmSession> session,
                                     std::string system_prompt, bool add_bos,
                                     int64_t random_seed)
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
  // Each request owns independent generation state. A serialized backend may
  // narrow actual execution internally without making the Model unsafe to
  // invoke concurrently.
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
  if (!session_ || !ValidateOptions(options)) return -1;

  BatchPolicy policy = session_->GetBatchPolicy();
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
                this->GenerateOne(prompts[slice.offset + i], options, &output);
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
  if (!session_ || prompt.data.empty() || !ValidateOptions(options)) return -1;

  try {
    ITokenCodec& codec = session_->TokenCodec();
    std::vector<int32_t> prompt_tokens;
    std::string diagnostic;
    if (codec.Encode(ApplyChatTemplate(prompt.data), add_bos_, &prompt_tokens,
                     &diagnostic) != 0 ||
        prompt_tokens.empty()) {
      return -1;
    }
    const size_t max_context = session_->MaxContextTokens();
    if (prompt_tokens.size() >= max_context) return -1;

    auto state = session_->CreateSequence(&diagnostic);
    if (!state) return -1;
    std::vector<float> logits;
    if (session_->Evaluate(prompt_tokens, *state, &logits, &diagnostic) != 0 ||
        !ValidateLogits(logits)) {
      return -1;
    }

    uint64_t seed = 0;
    if (random_seed_ < 0) {
      std::random_device random_device;
      seed = (static_cast<uint64_t>(random_device()) << 32U) ^
             static_cast<uint64_t>(random_device());
    } else {
      seed = static_cast<uint64_t>(random_seed_);
    }
    std::mt19937_64 rng(MixSeed(seed, prompt.req_id, prompt.sub_id));

    const size_t steps = std::min(static_cast<size_t>(options.max_tokens),
                                  max_context - prompt_tokens.size());
    for (size_t step = 0; step < steps; ++step) {
      if (!ValidateLogits(logits)) return -1;
      int32_t token = 0;
      if (options.temperature <= 0.01f) {
        token = GreedyToken(logits);
      } else if (!SampleTopP(logits, options.temperature, options.top_p, &rng,
                             &token)) {
        return -1;
      }
      if (token < 0 || static_cast<size_t>(token) >= logits.size()) return -1;
      if (codec.IsEndToken(token)) break;

      std::string piece;
      if (codec.DecodeToken(token, &piece, &diagnostic) != 0) return -1;
      output->append(piece);
      const size_t stop_position = FindFirstStop(*output, options.stop_words);
      if (stop_position != std::string::npos) {
        output->resize(stop_position);
        break;
      }
      if (step + 1 < steps) {
        if (session_->Evaluate({token}, *state, &logits, &diagnostic) != 0 ||
            !ValidateLogits(logits)) {
          output->clear();
          return -1;
        }
      }
    }
    StripIncompleteUtf8Suffix(output);
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
  if (!text || text->empty()) return;
  try {
    const size_t size = text->size();
    size_t continuation_count = 0;
    while (continuation_count < size &&
           (static_cast<unsigned char>((*text)[size - continuation_count - 1]) &
            0xC0U) == 0x80U) {
      ++continuation_count;
    }

    if (continuation_count == size) {
      text->clear();
      return;
    }
    const size_t lead_position = size - continuation_count - 1;
    const unsigned char lead =
        static_cast<unsigned char>((*text)[lead_position]);
    size_t expected = 1;
    if ((lead & 0x80U) == 0) {
      expected = 1;
    } else if ((lead & 0xE0U) == 0xC0U) {
      expected = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
      expected = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
      expected = 4;
    } else {
      text->resize(lead_position);
      return;
    }
    if (expected == 1 && continuation_count > 0) {
      text->resize(lead_position + 1);
    } else if (continuation_count + 1 != expected) {
      text->resize(lead_position);
    }
  } catch (...) {
    text->clear();
  }
}

static const ModelDefinition kQwenCausalLmModelDefinition = [] {
  ModelDefinition definition;
  definition.model_type = QwenCausalLmModel::kModelType;
  definition.capability = QwenCausalLmModel::kCapability;
  definition.description =
      "Qwen ChatML autoregressive model using the Causal LM protocol";
  definition.required_protocol = ExecutionProtocol::kCausalLm;
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
