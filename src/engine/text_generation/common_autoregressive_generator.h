#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "contracts/inference_payloads.h"

namespace alg_framework {
namespace text_generation {

// Backend-private, vendor-neutral state for one autoregressive request. It is
// intentionally not a Pipeline/Catalog execution protocol.
class IAutoregressiveDecoder {
 public:
  virtual ~IAutoregressiveDecoder() = default;

  virtual int Encode(const std::string& text, bool add_bos,
                     std::vector<int32_t>* tokens,
                     std::string* diagnostic = nullptr) noexcept = 0;
  virtual int DecodeToken(int32_t token, std::string* piece,
                          std::string* diagnostic = nullptr) noexcept = 0;
  virtual bool IsEndToken(int32_t token) const noexcept = 0;
  virtual size_t MaxContextTokens() const noexcept = 0;

  // The first call receives the complete prompt. Later calls receive only the
  // newly sampled token; the concrete decoder owns its incremental state.
  virtual int Evaluate(const std::vector<int32_t>& incremental_tokens,
                       std::vector<float>* logits,
                       std::string* diagnostic = nullptr) noexcept = 0;
};

bool ValidateGenerateOptions(const GenerateOptions& options,
                             std::string* diagnostic = nullptr) noexcept;

class CommonAutoregressiveGenerator final {
 public:
  static int Generate(IAutoregressiveDecoder& decoder,
                      const std::string& formatted_prompt, bool add_bos,
                      const GenerateOptions& options,
                      std::optional<uint64_t> seed, std::string* output,
                      std::string* diagnostic = nullptr) noexcept;
};

}  // namespace text_generation
}  // namespace alg_framework
