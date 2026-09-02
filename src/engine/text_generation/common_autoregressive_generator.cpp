#include "engine/text_generation/common_autoregressive_generator.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <numeric>
#include <random>
#include <unordered_set>
#include <utility>

#include "engine/text/utf8.h"

namespace llm_edgeflow {
namespace text_generation {
namespace {

void SetDiagnostic(std::string* diagnostic,
                   const std::string& message) noexcept {
  if (!diagnostic) return;
  try {
    *diagnostic = message;
  } catch (...) {
  }
}

bool ValidateLogits(const std::vector<float>& logits,
                    std::string* diagnostic) noexcept {
  if (logits.empty()) {
    SetDiagnostic(diagnostic, "Autoregressive decoder returned empty logits");
    return false;
  }
  if (!std::all_of(logits.begin(), logits.end(),
                   [](float value) { return std::isfinite(value); })) {
    SetDiagnostic(diagnostic,
                  "Autoregressive decoder returned non-finite logits");
    return false;
  }
  return true;
}

void ApplyRepetitionPenalty(const std::vector<int32_t>& history, float penalty,
                            std::vector<float>* logits) {
  if (!logits || penalty == 1.0f) return;
  std::unordered_set<int32_t> seen;
  seen.reserve(history.size());
  for (int32_t token : history) {
    if (token < 0 || static_cast<size_t>(token) >= logits->size() ||
        !seen.insert(token).second) {
      continue;
    }
    float& value = (*logits)[static_cast<size_t>(token)];
    value = value < 0.0f ? value * penalty : value / penalty;
  }
}

bool SampleToken(const std::vector<float>& source_logits,
                 const std::vector<int32_t>& history,
                 const GenerateOptions& options, std::mt19937_64* rng,
                 int32_t* token, std::string* diagnostic) {
  if (!rng || !token || !ValidateLogits(source_logits, diagnostic)) {
    return false;
  }

  std::vector<float> logits = source_logits;
  ApplyRepetitionPenalty(history, options.repetition_penalty, &logits);
  if (!ValidateLogits(logits, diagnostic)) return false;

  if (options.temperature <= 0.01f) {
    const auto selected = std::max_element(logits.begin(), logits.end());
    const size_t index =
        static_cast<size_t>(std::distance(logits.begin(), selected));
    if (index > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      SetDiagnostic(diagnostic, "Selected token id exceeds int32 range");
      return false;
    }
    *token = static_cast<int32_t>(index);
    return true;
  }

  std::vector<size_t> indices(logits.size());
  std::iota(indices.begin(), indices.end(), size_t{0});
  std::stable_sort(
      indices.begin(), indices.end(),
      [&logits](size_t lhs, size_t rhs) { return logits[lhs] > logits[rhs]; });
  if (options.top_k > 0 &&
      static_cast<size_t>(options.top_k) < indices.size()) {
    indices.resize(static_cast<size_t>(options.top_k));
  }
  if (indices.empty()) {
    SetDiagnostic(diagnostic, "Sampling candidate set is empty");
    return false;
  }

  const double inverse_temperature = 1.0 / options.temperature;
  const float max_logit = logits[indices.front()];
  std::vector<double> probabilities(indices.size());
  double total = 0.0;
  for (size_t i = 0; i < indices.size(); ++i) {
    const double probability =
        std::exp((static_cast<double>(logits[indices[i]]) - max_logit) *
                 inverse_temperature);
    if (!std::isfinite(probability)) {
      SetDiagnostic(diagnostic, "Sampling probability is non-finite");
      return false;
    }
    probabilities[i] = probability;
    total += probability;
  }
  if (!std::isfinite(total) || total <= 0.0) {
    SetDiagnostic(diagnostic, "Sampling probability mass is invalid");
    return false;
  }
  for (double& probability : probabilities) probability /= total;

  double nucleus_mass = 0.0;
  size_t candidate_count = 0;
  for (; candidate_count < probabilities.size(); ++candidate_count) {
    nucleus_mass += probabilities[candidate_count];
    if (nucleus_mass >= static_cast<double>(options.top_p)) {
      ++candidate_count;
      break;
    }
  }
  if (candidate_count == 0 || !std::isfinite(nucleus_mass) ||
      nucleus_mass <= 0.0) {
    SetDiagnostic(diagnostic, "Top-p candidate mass is invalid");
    return false;
  }

  std::uniform_real_distribution<double> distribution(0.0, nucleus_mass);
  const double choice = distribution(*rng);
  double cursor = 0.0;
  size_t selected = indices[candidate_count - 1];
  for (size_t i = 0; i < candidate_count; ++i) {
    cursor += probabilities[i];
    if (choice <= cursor) {
      selected = indices[i];
      break;
    }
  }
  if (selected > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    SetDiagnostic(diagnostic, "Sampled token id exceeds int32 range");
    return false;
  }
  *token = static_cast<int32_t>(selected);
  return true;
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

bool ValidateGenerateOptions(const GenerateOptions& options,
                             std::string* diagnostic) noexcept {
  try {
    if (options.max_tokens <= 0 || !std::isfinite(options.temperature) ||
        options.temperature < 0.0f || options.temperature > 2.0f ||
        options.top_k < 0 || !std::isfinite(options.top_p) ||
        options.top_p <= 0.0f || options.top_p > 1.0f ||
        !std::isfinite(options.repetition_penalty) ||
        options.repetition_penalty <= 0.0f ||
        options.repetition_penalty > 100.0f ||
        !std::all_of(options.stop_words.begin(), options.stop_words.end(),
                     [](const std::string& word) { return !word.empty(); })) {
      SetDiagnostic(diagnostic, "Invalid text generation options");
      return false;
    }
    return true;
  } catch (...) {
    SetDiagnostic(diagnostic, "Exception validating text generation options");
    return false;
  }
}

int CommonAutoregressiveGenerator::Generate(
    IAutoregressiveDecoder& decoder, const std::string& formatted_prompt,
    bool add_bos, const GenerateOptions& options, std::optional<uint64_t> seed,
    std::string* output, std::string* diagnostic) noexcept {
  if (!output) {
    SetDiagnostic(diagnostic, "Text generation output pointer is null");
    return -1;
  }
  output->clear();
  if (formatted_prompt.empty() ||
      !ValidateGenerateOptions(options, diagnostic)) {
    if (formatted_prompt.empty()) {
      SetDiagnostic(diagnostic, "Formatted prompt is empty");
    }
    return -1;
  }

  try {
    std::vector<int32_t> prompt_tokens;
    if (decoder.Encode(formatted_prompt, add_bos, &prompt_tokens, diagnostic) !=
            0 ||
        prompt_tokens.empty()) {
      output->clear();
      return -1;
    }
    const size_t max_context = decoder.MaxContextTokens();
    if (max_context < 2 || prompt_tokens.size() >= max_context) {
      SetDiagnostic(diagnostic, "Prompt exceeds text generation context");
      return -1;
    }

    std::vector<float> logits;
    if (decoder.Evaluate(prompt_tokens, &logits, diagnostic) != 0 ||
        !ValidateLogits(logits, diagnostic)) {
      output->clear();
      return -1;
    }

    uint64_t actual_seed = 0;
    if (seed.has_value()) {
      actual_seed = *seed;
    } else {
      std::random_device random_device;
      actual_seed = (static_cast<uint64_t>(random_device()) << 32U) ^
                    static_cast<uint64_t>(random_device());
    }
    std::mt19937_64 rng(actual_seed);

    const size_t steps = std::min(static_cast<size_t>(options.max_tokens),
                                  max_context - prompt_tokens.size());
    std::vector<int32_t> history = prompt_tokens;
    history.reserve(prompt_tokens.size() + steps);
    for (size_t step = 0; step < steps; ++step) {
      int32_t token = 0;
      if (!SampleToken(logits, history, options, &rng, &token, diagnostic) ||
          token < 0 || static_cast<size_t>(token) >= logits.size()) {
        output->clear();
        return -1;
      }
      if (decoder.IsEndToken(token)) break;

      std::string piece;
      if (decoder.DecodeToken(token, &piece, diagnostic) != 0) {
        output->clear();
        return -1;
      }
      output->append(piece);
      history.push_back(token);
      const size_t stop_position = FindFirstStop(*output, options.stop_words);
      if (stop_position != std::string::npos) {
        output->resize(stop_position);
        break;
      }
      if (step + 1 < steps &&
          (decoder.Evaluate({token}, &logits, diagnostic) != 0 ||
           !ValidateLogits(logits, diagnostic))) {
        output->clear();
        return -1;
      }
    }
    utf8::StripIncompleteSuffix(output);
    return 0;
  } catch (const std::exception& e) {
    output->clear();
    SetDiagnostic(diagnostic,
                  std::string("Text generation exception: ") + e.what());
    return -1;
  } catch (...) {
    output->clear();
    SetDiagnostic(diagnostic, "Unknown text generation exception");
    return -1;
  }
}

}  // namespace text_generation
}  // namespace llm_edgeflow
