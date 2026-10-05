#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "contracts/inference_payloads.h"

namespace llm_edgeflow {
namespace text_generation {

// Backend 私有、与厂商无关的单次自回归请求状态。
// 有意不作为 Pipeline/Catalog 执行协议。
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

  // 首次调用接收完整 prompt，后续调用只接收新采样的 token；
  // 增量状态由具体解码器维护。
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
}  // namespace llm_edgeflow
