#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace llm_edgeflow {
namespace text {

// 字符类别位，对应预分词正则中的 \p{L}、\p{N}、\s。
inline constexpr uint32_t kUnicodeLetter = 1;
inline constexpr uint32_t kUnicodeNumber = 2;
inline constexpr uint32_t kUnicodeSpace = 4;

// 分词器文件中 Unicode 区段的只读视图。指针须按 4 字节对齐，所指内存在
// UnicodeTables 存活期间保持有效。各表的行布局见
// doc/dev_guide/bpe_tokenizer_format.md。
struct UnicodeSections {
  const uint32_t* properties = nullptr;  // (起, 止, 类别位)，按码点升序且不重叠
  size_t property_rows = 0;
  const uint32_t* decompositions = nullptr;  // (码点, 池偏移, 长度)，按码点升序
  size_t decomposition_rows = 0;
  const uint32_t* pool = nullptr;  // 完全分解后的码点序列
  size_t pool_count = 0;
  const uint32_t* combining = nullptr;  // (码点, 组合类)，按码点升序
  size_t combining_rows = 0;
  const uint32_t* compositions = nullptr;  // (首, 次, 合成)，按 (首, 次) 升序
  size_t composition_rows = 0;
};

// 由文件中的 Unicode 表提供 NFC 规范化和字符类别查询。绑定后只读，
// 可被多个线程同时调用。
class UnicodeTables final {
 public:
  // 校验并绑定各表。校验失败返回 false，*reason 指向静态字符串。
  // 不需要的表必须为空，需要的表必须非空：
  // need_properties 对应 Flags，need_nfc 对应 NormalizeNfc。
  bool Bind(const UnicodeSections& sections, bool need_properties,
            bool need_nfc, const char** reason) noexcept;

  // 返回码点的类别位；未登记的码点返回 0。
  uint32_t Flags(uint32_t code_point) const noexcept;

  // 对有效 UTF-8 文本做 NFC 规范化，输出码点序列。
  void NormalizeNfc(std::string_view text,
                    std::vector<uint32_t>* code_points) const;

 private:
  uint32_t CombiningClass(uint32_t code_point) const noexcept;
  uint32_t Compose(uint32_t first, uint32_t second) const noexcept;

  UnicodeSections sections_;
};

}  // namespace text
}  // namespace llm_edgeflow
