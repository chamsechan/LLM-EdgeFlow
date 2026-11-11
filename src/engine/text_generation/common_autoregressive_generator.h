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

  /**
   * @brief 首次调用接收完整 prompt，后续调用只接收新采样的 token；
   * 增量状态由具体解码器维护。
   */
  virtual int Evaluate(const std::vector<int32_t>& incremental_tokens,
                       std::vector<float>* logits,
                       std::string* diagnostic = nullptr) noexcept = 0;
};

bool ValidateGenerateOptions(const GenerateOptions& options,
                             std::string* diagnostic = nullptr) noexcept;

/**
 * @brief text 中没有出现任何停止词时返回 std::string::npos。
 */
size_t FindFirstStop(const std::string& text,
                     const std::vector<std::string>& stop_words) noexcept;

class CommonAutoregressiveGenerator final {
 public:
  /**
   * @brief 使用单次请求的解码状态完成 prompt 编码、增量推理和采样。
   * 生成步数同时受 max_tokens 与剩余上下文限制；遇 EOS 或停止词结束，
   * 输出不含停止词及尾部不完整的 UTF-8 序列，失败时清空输出。
   */
  static int Generate(IAutoregressiveDecoder& decoder,
                      const std::string& formatted_prompt, bool add_bos,
                      const GenerateOptions& options,
                      std::optional<uint64_t> seed, std::string* output,
                      std::string* diagnostic = nullptr) noexcept;
};

}  // namespace text_generation
}  // namespace llm_edgeflow
