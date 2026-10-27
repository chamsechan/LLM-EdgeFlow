#include "engine/text/byte_level_pretokenizer.h"

#include <cstdint>

#include "engine/text/utf8.h"

namespace llm_edgeflow {
namespace text {
namespace {

// (?i) 下的 ASCII 字母折叠；U+017F（长 s）与 s 等价。
uint32_t FoldCase(uint32_t code_point) noexcept {
  if (code_point >= 'A' && code_point <= 'Z') return code_point + 32;
  return code_point == 0x17F ? uint32_t{'s'} : code_point;
}

}  // namespace

bool SplitByteLevelQwen2(const UnicodeTables& tables, std::string_view text,
                         std::vector<std::pair<size_t, size_t>>* words) {
  words->clear();
  // 码点、类别及各码点的起始字节偏移（末尾多一项 text.size()）。
  std::vector<uint32_t> chars;
  std::vector<uint32_t> flags;
  std::vector<size_t> offsets;
  // 码点数不超过字节数：先按字节数分配，填完后收缩。按下标写入，避免在逐字符
  // 的循环里对向量 push_back 局部变量（在带 AddressSanitizer 的构建里会慢
  // 两个数量级）。
  chars.resize(text.size());
  flags.resize(text.size());
  offsets.resize(text.size() + 1);
  size_t count = 0;
  for (size_t position = 0; position < text.size();) {
    uint32_t code_point = static_cast<unsigned char>(text[position]);
    size_t consumed = 1;
    if (code_point >= 0x80) {
      consumed = utf8::DecodeCodePoint(text.data() + position,
                                       text.size() - position, &code_point);
      if (consumed == 0) return false;
    }
    chars[count] = code_point;
    flags[count] = tables.Flags(code_point);
    offsets[count] = position;
    ++count;
    position += consumed;
  }
  offsets[count] = text.size();
  chars.resize(count);
  flags.resize(count);
  offsets.resize(count + 1);

  const size_t n = chars.size();
  const auto letter = [&](size_t p) {
    return p < n && (flags[p] & kUnicodeLetter) != 0;
  };
  const auto space = [&](size_t p) {
    return p < n && (flags[p] & kUnicodeSpace) != 0;
  };
  const auto newline = [&](size_t p) {
    return p < n && (chars[p] == '\n' || chars[p] == '\r');
  };
  const auto symbol = [&](size_t p) {
    return p < n &&
           (flags[p] & (kUnicodeLetter | kUnicodeNumber | kUnicodeSpace)) == 0;
  };

  for (size_t i = 0; i < n;) {
    size_t end = i;
    // 分支 1：缩写，如 's、're。
    if (chars[i] == '\'' && i + 1 < n) {
      const uint32_t a = FoldCase(chars[i + 1]);
      if (a == 's' || a == 't' || a == 'm' || a == 'd') {
        end = i + 2;
      } else if (i + 2 < n) {
        const uint32_t b = FoldCase(chars[i + 2]);
        if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') ||
            (a == 'l' && b == 'l')) {
          end = i + 3;
        }
      }
    }
    // 分支 2：可选的一个非字母数字非换行前缀，加连续字母。
    if (end == i) {
      size_t start = i;
      if (!newline(i) && (flags[i] & (kUnicodeLetter | kUnicodeNumber)) == 0) {
        ++start;
      }
      if (!letter(start)) start = i;  // 回退到没有前缀。
      if (letter(start)) {
        end = start + 1;
        while (letter(end)) ++end;
      }
    }
    // 分支 3：单个数字。
    if (end == i && (flags[i] & kUnicodeNumber) != 0) end = i + 1;
    // 分支 4：可选空格，加连续符号，再加连续换行。
    if (end == i) {
      const size_t start = i + (chars[i] == ' ');
      if (symbol(start)) {
        end = start + 1;
        while (symbol(end)) ++end;
        while (newline(end)) ++end;
      }
    }
    // 分支 5 到 7：空白。
    if (end == i && space(i)) {
      size_t stop = i;
      size_t last_newline_end = i;
      while (space(stop)) {
        if (newline(stop)) last_newline_end = stop + 1;
        ++stop;
      }
      if (last_newline_end > i) {
        end = last_newline_end;  // \s*[\r\n]+
      } else if (stop == n || stop - i < 2) {
        end = stop;  // 空白一直到文本末尾，或只有一个字符：\s+
      } else {
        end = stop - 1;  // \s+(?!\S)：留一个空白给下一个分支 2 或 4 作前缀。
      }
    }
    if (end == i) return false;
    words->emplace_back(offsets[i], offsets[end]);
    i = end;
  }
  return true;
}

}  // namespace text
}  // namespace llm_edgeflow
