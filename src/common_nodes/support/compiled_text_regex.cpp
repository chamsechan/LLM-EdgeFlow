#include "common_nodes/support/compiled_text_regex.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace llm_edgeflow {

namespace {

struct Pcre2CodeDeleter {
  void operator()(pcre2_code* code) const noexcept {
    if (code) pcre2_code_free(code);
  }
};

struct Pcre2MatchDataDeleter {
  void operator()(pcre2_match_data* match_data) const noexcept {
    if (match_data) pcre2_match_data_free(match_data);
  }
};

using Pcre2CodePtr = std::unique_ptr<pcre2_code, Pcre2CodeDeleter>;
using Pcre2MatchDataPtr =
    std::unique_ptr<pcre2_match_data, Pcre2MatchDataDeleter>;

std::string Pcre2ErrorMessage(int error_code) {
  PCRE2_UCHAR buffer[256] = {};
  const int length =
      pcre2_get_error_message(error_code, buffer, sizeof(buffer));
  if (length < 0) {
    return "PCRE2 error " + std::to_string(error_code);
  }
  return std::string(reinterpret_cast<const char*>(buffer),
                     static_cast<size_t>(length));
}

struct NamedCapture {
  std::string name;
  uint32_t group_number = 0;
};

}  // namespace

struct CompiledTextRegex::Impl {
  Pcre2CodePtr code;
  std::vector<NamedCapture> named_captures;
};

CompiledTextRegex::CompiledTextRegex() = default;
CompiledTextRegex::~CompiledTextRegex() = default;
CompiledTextRegex::CompiledTextRegex(CompiledTextRegex&&) noexcept = default;
CompiledTextRegex& CompiledTextRegex::operator=(CompiledTextRegex&&) noexcept =
    default;

bool CompiledTextRegex::Compile(const std::string& pattern,
                                std::string* diagnostic) {
  int error_code = 0;
  PCRE2_SIZE error_offset = 0;
  Pcre2CodePtr code(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                                  pattern.size(), PCRE2_UTF | PCRE2_UCP,
                                  &error_code, &error_offset, nullptr));
  if (!code) {
    if (diagnostic) {
      *diagnostic = "at byte offset " + std::to_string(error_offset) + ": " +
                    Pcre2ErrorMessage(error_code);
    }
    return false;
  }

  uint32_t name_count = 0;
  uint32_t entry_size = 0;
  PCRE2_SPTR name_table = nullptr;
  if (pcre2_pattern_info(code.get(), PCRE2_INFO_NAMECOUNT, &name_count) != 0 ||
      pcre2_pattern_info(code.get(), PCRE2_INFO_NAMEENTRYSIZE, &entry_size) !=
          0 ||
      pcre2_pattern_info(code.get(), PCRE2_INFO_NAMETABLE, &name_table) != 0) {
    if (diagnostic) *diagnostic = "Failed to inspect PCRE2 named captures";
    return false;
  }

  auto next = std::make_unique<Impl>();
  next->code = std::move(code);
  next->named_captures.reserve(name_count);
  for (uint32_t i = 0; i < name_count; ++i) {
    const PCRE2_SPTR entry = name_table + i * entry_size;
    const uint32_t group_number = (static_cast<uint32_t>(entry[0]) << 8U) |
                                  static_cast<uint32_t>(entry[1]);
    next->named_captures.push_back(
        {reinterpret_cast<const char*>(entry + 2), group_number});
  }

  impl_ = std::move(next);
  if (diagnostic) diagnostic->clear();
  return true;
}

TextRegexSearchStatus CompiledTextRegex::Search(
    const std::string& subject,
    std::unordered_map<std::string, std::string>* named_captures,
    std::string* diagnostic) const {
  if (named_captures) named_captures->clear();
  if (diagnostic) diagnostic->clear();
  if (!impl_ || !impl_->code) return TextRegexSearchStatus::kNotMatched;

  Pcre2MatchDataPtr match_data(
      pcre2_match_data_create_from_pattern(impl_->code.get(), nullptr));
  if (!match_data) {
    if (diagnostic) *diagnostic = "Failed to allocate PCRE2 match data";
    return TextRegexSearchStatus::kError;
  }

  const int match_count = pcre2_match(
      impl_->code.get(), reinterpret_cast<PCRE2_SPTR>(subject.data()),
      subject.size(), 0, 0, match_data.get(), nullptr);
  if (match_count == PCRE2_ERROR_NOMATCH) {
    return TextRegexSearchStatus::kNotMatched;
  }
  if (match_count < 0) {
    if (diagnostic) *diagnostic = Pcre2ErrorMessage(match_count);
    return TextRegexSearchStatus::kError;
  }

  if (named_captures) {
    const PCRE2_SIZE* offsets = pcre2_get_ovector_pointer(match_data.get());
    const uint32_t offset_count = pcre2_get_ovector_count(match_data.get());
    for (const auto& capture : impl_->named_captures) {
      if (capture.group_number >= offset_count) continue;
      const PCRE2_SIZE begin = offsets[2 * capture.group_number];
      const PCRE2_SIZE end = offsets[2 * capture.group_number + 1];
      if (begin == PCRE2_UNSET || end == PCRE2_UNSET) {
        (*named_captures)[capture.name] = "";
      } else {
        (*named_captures)[capture.name] = subject.substr(begin, end - begin);
      }
    }
  }
  return TextRegexSearchStatus::kMatched;
}

}  // namespace llm_edgeflow
