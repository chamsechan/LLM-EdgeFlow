#include "engine/text/unicode_tables.h"

#include <algorithm>

#include "engine/text/utf8.h"

namespace llm_edgeflow {
namespace text {
namespace {

bool IsScalar(uint32_t code_point) noexcept {
  return code_point <= 0x10FFFF &&
         !(code_point >= 0xD800 && code_point <= 0xDFFF);
}

// 在 stride 个 uint32 为一行、首列为键的有序表中查找键。
const uint32_t* FindRow(const uint32_t* table, size_t rows, size_t stride,
                        uint32_t key) noexcept {
  size_t low = 0;
  size_t high = rows;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (table[middle * stride] < key) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low < rows && table[low * stride] == key ? table + low * stride
                                                  : nullptr;
}

// Hangul 音节的分解与合成使用公式，不进入表。
constexpr uint32_t kHangulBase = 0xAC00;
constexpr uint32_t kHangulEnd = 0xD7A4;
constexpr uint32_t kLeadBase = 0x1100;
constexpr uint32_t kVowelBase = 0x1161;
constexpr uint32_t kTrailBase = 0x11A7;
constexpr uint32_t kLeadCount = 19;
constexpr uint32_t kVowelCount = 21;
constexpr uint32_t kTrailCount = 28;

}  // namespace

bool UnicodeTables::Bind(const UnicodeSections& sections, bool need_properties,
                         bool need_nfc, const char** reason) noexcept {
  const auto fail = [reason](const char* message) {
    *reason = message;
    return false;
  };
  if ((sections.property_rows != 0) != need_properties) {
    return fail(
        "Unicode property table presence does not match the "
        "pre-tokenizer");
  }
  if (need_nfc
          ? (sections.decomposition_rows == 0 || sections.combining_rows == 0 ||
             sections.composition_rows == 0 || sections.pool_count == 0)
          : (sections.decomposition_rows != 0 || sections.combining_rows != 0 ||
             sections.composition_rows != 0 || sections.pool_count != 0)) {
    return fail(
        "Unicode normalization tables presence does not match the "
        "normalizer");
  }
  for (size_t i = 0; i < sections.property_rows; ++i) {
    const uint32_t* row = sections.properties + i * 3;
    if (!IsScalar(row[0]) || !IsScalar(row[1]) || row[0] > row[1] ||
        (row[2] & ~(kUnicodeLetter | kUnicodeNumber | kUnicodeSpace)) != 0 ||
        row[2] == 0 || (i != 0 && row[-2] >= row[0])) {
      return fail("Unicode property table is invalid");
    }
  }
  for (size_t i = 0; i < sections.decomposition_rows; ++i) {
    const uint32_t* row = sections.decompositions + i * 3;
    if (!IsScalar(row[0]) || row[2] == 0 || row[2] > 4 ||
        row[1] > sections.pool_count || row[2] > sections.pool_count - row[1] ||
        (i != 0 && row[-3] >= row[0])) {
      return fail("Unicode decomposition table is invalid");
    }
  }
  for (size_t i = 0; i < sections.pool_count; ++i) {
    if (!IsScalar(sections.pool[i])) {
      return fail("Unicode decomposition pool is invalid");
    }
  }
  for (size_t i = 0; i < sections.combining_rows; ++i) {
    const uint32_t* row = sections.combining + i * 2;
    if (!IsScalar(row[0]) || row[1] == 0 || row[1] > 255 ||
        (i != 0 && row[-2] >= row[0])) {
      return fail("Unicode combining class table is invalid");
    }
  }
  for (size_t i = 0; i < sections.composition_rows; ++i) {
    const uint32_t* row = sections.compositions + i * 3;
    if (!IsScalar(row[0]) || !IsScalar(row[1]) || !IsScalar(row[2]) ||
        (i != 0 &&
         !(row[-3] < row[0] || (row[-3] == row[0] && row[-2] < row[1])))) {
      return fail("Unicode composition table is invalid");
    }
  }
  sections_ = sections;
  return true;
}

uint32_t UnicodeTables::Flags(uint32_t code_point) const noexcept {
  // 找到第一个止点不小于 code_point 的区间。
  size_t low = 0;
  size_t high = sections_.property_rows;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (sections_.properties[middle * 3 + 1] < code_point) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low < sections_.property_rows &&
                 sections_.properties[low * 3] <= code_point
             ? sections_.properties[low * 3 + 2]
             : 0;
}

uint32_t UnicodeTables::CombiningClass(uint32_t code_point) const noexcept {
  const uint32_t* row =
      FindRow(sections_.combining, sections_.combining_rows, 2, code_point);
  return row ? row[1] : 0;
}

uint32_t UnicodeTables::Compose(uint32_t first,
                                uint32_t second) const noexcept {
  if (first >= kLeadBase && first < kLeadBase + kLeadCount &&
      second >= kVowelBase && second < kVowelBase + kVowelCount) {
    return kHangulBase +
           ((first - kLeadBase) * kVowelCount + (second - kVowelBase)) *
               kTrailCount;
  }
  if (first >= kHangulBase && first < kHangulEnd &&
      (first - kHangulBase) % kTrailCount == 0 && second > kTrailBase &&
      second < kTrailBase + kTrailCount) {
    return first + (second - kTrailBase);
  }
  size_t low = 0;
  size_t high = sections_.composition_rows;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const uint32_t* row = sections_.compositions + middle * 3;
    if (row[0] < first || (row[0] == first && row[1] < second)) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  const uint32_t* row = sections_.compositions + low * 3;
  return low < sections_.composition_rows && row[0] == first && row[1] == second
             ? row[2]
             : 0;
}

void UnicodeTables::NormalizeNfc(std::string_view text,
                                 std::vector<uint32_t>* code_points) const {
  std::vector<uint32_t>& chars = *code_points;
  chars.clear();
  chars.reserve(text.size());
  // 纯 ASCII 文本的 NFC 是它本身。
  if (std::all_of(text.begin(), text.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x80;
      })) {
    chars.assign(text.begin(), text.end());
    return;
  }
  // 分解后的码点数不超过输入码点数的 4 倍。按下标写入，容量不够时翻倍。
  chars.resize(text.size() + 4);
  size_t count = 0;
  for (size_t position = 0; position < text.size();) {
    uint32_t code_point = 0;
    const size_t consumed = utf8::DecodeCodePoint(
        text.data() + position, text.size() - position, &code_point);
    if (consumed == 0) break;  // 调用方保证 UTF-8 有效。
    position += consumed;
    if (count + 4 > chars.size()) chars.resize(chars.size() * 2);
    if (code_point >= kHangulBase && code_point < kHangulEnd) {
      const uint32_t index = code_point - kHangulBase;
      chars[count++] = kLeadBase + index / (kVowelCount * kTrailCount);
      chars[count++] = kVowelBase + (index / kTrailCount) % kVowelCount;
      if (index % kTrailCount != 0)
        chars[count++] = kTrailBase + index % kTrailCount;
    } else if (const uint32_t* row =
                   FindRow(sections_.decompositions,
                           sections_.decomposition_rows, 3, code_point)) {
      for (uint32_t j = 0; j < row[2]; ++j) {
        chars[count++] = sections_.pool[row[1] + j];
      }
    } else {
      chars[count++] = code_point;
    }
  }
  chars.resize(count);
  if (chars.empty()) return;

  // 每个码点的组合类只查一次。
  std::vector<uint8_t> classes(chars.size());
  for (size_t i = 0; i < chars.size(); ++i) {
    classes[i] = static_cast<uint8_t>(CombiningClass(chars[i]));
  }

  // 规范排序：每一段连续的非起始符（组合类非零）按组合类稳定排序，O(n log n)。
  struct Mark {
    uint32_t cls;
    uint32_t code_point;
  };
  std::vector<Mark> run;
  for (size_t i = 0; i < chars.size();) {
    if (classes[i] == 0) {
      ++i;
      continue;
    }
    size_t end = i;
    while (end < chars.size() && classes[end] != 0) ++end;
    if (end - i > 1) {
      run.resize(end - i);
      for (size_t k = i; k < end; ++k) {
        run[k - i].cls = classes[k];
        run[k - i].code_point = chars[k];
      }
      std::stable_sort(
          run.begin(), run.end(),
          [](const Mark& a, const Mark& b) { return a.cls < b.cls; });
      for (size_t k = i; k < end; ++k) {
        chars[k] = run[k - i].code_point;
        classes[k] = static_cast<uint8_t>(run[k - i].cls);
      }
    }
    i = end;
  }

  // 规范合成：每个字符尝试与最近的起始符合成，被同组合类或更大组合类的字符
  // 阻隔时不合成。
  size_t written = 1;
  size_t starter = 0;
  uint32_t previous = classes[0];
  for (size_t i = 1; i < chars.size(); ++i) {
    const uint32_t code_point = chars[i];
    const uint32_t cls = classes[i];
    const uint32_t composite = (previous == 0 || previous < cls)
                                   ? Compose(chars[starter], code_point)
                                   : 0;
    if (composite != 0) {
      chars[starter] = composite;
    } else {
      if (cls == 0) starter = written;
      chars[written++] = code_point;
      previous = cls;
    }
  }
  chars.resize(written);
}

}  // namespace text
}  // namespace llm_edgeflow
