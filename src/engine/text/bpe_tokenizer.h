#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace llm_edgeflow {
namespace text {

// 单次 Encode 接受的最大输入字节数。
inline constexpr size_t kMaxEncodeBytes = size_t{1} << 20;

// 通用 BPE 分词器，读取 tools/prepare_bpe_tokenizer.py 生成的 `.eftok` 文件。
// 分词流程中的规范化、预分词、字节回退、合并优先级和解码器由文件头的参数选择，
// 与具体模型无关；文件格式见 doc/dev_guide/bpe_tokenizer_format.md。
//
// 实例加载后只读，多个线程可同时调用同一实例。所有方法成功返回 0，失败返回 -1
// 并写入 diagnostic（可为空指针），不抛出异常。
class BpeTokenizer final {
 public:
  ~BpeTokenizer();
  BpeTokenizer(const BpeTokenizer&) = delete;
  BpeTokenizer& operator=(const BpeTokenizer&) = delete;

  // 以只读 mmap 加载文件；文件须原子替换，加载后不得截断。
  static std::shared_ptr<const BpeTokenizer> Load(
      const std::string& file, std::string* diagnostic = nullptr) noexcept;

  // 有效 UTF-8，长度不超过 kMaxEncodeBytes；文本中的特殊 token 识别为对应 ID。
  // 不会自动添加 BOS。
  int Encode(std::string_view text, std::vector<int32_t>* ids,
             std::string* diagnostic = nullptr) const noexcept;
  // 追加单个 token 的原始字节；特殊 token 追加空串。
  int AppendTokenBytes(int32_t id, std::string* bytes,
                       std::string* diagnostic = nullptr) const noexcept;
  // 完整解码：跳过特殊 token，非法 UTF-8 按解码器参数处理，与 HF 一致。
  int Decode(const std::vector<int32_t>& ids, std::string* text,
             std::string* diagnostic = nullptr) const noexcept;
  bool IsEndToken(int32_t id) const noexcept;
  // 升序且无重复。
  const std::vector<int32_t>& EndTokens() const noexcept;
  size_t VocabSize() const noexcept;  // 最大 ID + 1

 private:
  class Impl;
  explicit BpeTokenizer(std::unique_ptr<const Impl> impl) noexcept;

  std::unique_ptr<const Impl> impl_;
};

}  // namespace text
}  // namespace llm_edgeflow
