#include "engine/text/utf8.h"

namespace alg_framework {
namespace utf8 {

size_t DecodeCodePoint(const char* data, size_t length,
                       uint32_t* code_point) noexcept {
  if (!data || !code_point || length == 0) return 0;

  const auto c0 = static_cast<unsigned char>(data[0]);
  if (c0 < 0x80) {
    *code_point = c0;
    return 1;
  }
  if ((c0 & 0xE0) == 0xC0) {
    if (length < 2) return 0;
    const auto c1 = static_cast<unsigned char>(data[1]);
    if ((c1 & 0xC0) != 0x80) return 0;
    *code_point = ((c0 & 0x1F) << 6U) | (c1 & 0x3F);
    return *code_point >= 0x80 ? 2 : 0;
  }
  if ((c0 & 0xF0) == 0xE0) {
    if (length < 3) return 0;
    const auto c1 = static_cast<unsigned char>(data[1]);
    const auto c2 = static_cast<unsigned char>(data[2]);
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return 0;
    *code_point = ((c0 & 0x0F) << 12U) | ((c1 & 0x3F) << 6U) | (c2 & 0x3F);
    if (*code_point < 0x800 ||
        (*code_point >= 0xD800 && *code_point <= 0xDFFF)) {
      return 0;
    }
    return 3;
  }
  if ((c0 & 0xF8) == 0xF0) {
    if (length < 4) return 0;
    const auto c1 = static_cast<unsigned char>(data[1]);
    const auto c2 = static_cast<unsigned char>(data[2]);
    const auto c3 = static_cast<unsigned char>(data[3]);
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
      return 0;
    }
    *code_point = ((c0 & 0x07) << 18U) | ((c1 & 0x3F) << 12U) |
                  ((c2 & 0x3F) << 6U) | (c3 & 0x3F);
    return (*code_point >= 0x10000 && *code_point <= 0x10FFFF) ? 4 : 0;
  }
  return 0;
}

bool BuildCodePointBoundaries(std::string_view text,
                              std::vector<size_t>* boundaries,
                              size_t* invalid_offset) {
  if (!boundaries) return false;
  boundaries->clear();
  boundaries->reserve(text.size() + 1);
  boundaries->push_back(0);

  size_t offset = 0;
  while (offset < text.size()) {
    uint32_t code_point = 0;
    const size_t consumed = DecodeCodePoint(text.data() + offset,
                                            text.size() - offset, &code_point);
    if (consumed == 0) {
      boundaries->clear();
      if (invalid_offset) *invalid_offset = offset;
      return false;
    }
    offset += consumed;
    boundaries->push_back(offset);
  }

  if (invalid_offset) *invalid_offset = text.size();
  return true;
}

void StripIncompleteSuffix(std::string* text) noexcept {
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

}  // namespace utf8
}  // namespace alg_framework
