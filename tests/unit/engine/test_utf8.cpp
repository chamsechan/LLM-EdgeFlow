#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "engine/text/utf8.h"

namespace llm_edgeflow {
namespace {

TEST(Utf8Test, AppendsScalarEncodingWithoutChangingExistingBytes) {
  struct Case {
    uint32_t code_point;
    std::string encoded;
  };
  const std::vector<Case> cases = {
      {0, std::string(1, '\0')},
      {0x7F, "\x7F"},
      {0x80, "\xC2\x80"},
      {0x7FF, "\xDF\xBF"},
      {0x800, "\xE0\xA0\x80"},
      {0x4F60, "\xE4\xBD\xA0"},
      {0xD7FF, "\xED\x9F\xBF"},
      {0xE000, "\xEE\x80\x80"},
      {0xFFFF, "\xEF\xBF\xBF"},
      {0x10000, "\xF0\x90\x80\x80"},
      {0x1F600, "\xF0\x9F\x98\x80"},
      {0x10FFFF, "\xF4\x8F\xBF\xBF"},
  };
  const std::string prefix("head\0", 5);
  for (const auto& test : cases) {
    SCOPED_TRACE(test.code_point);
    std::string text = prefix;
    utf8::AppendCodePoint(test.code_point, &text);
    EXPECT_EQ(text, prefix + test.encoded);
  }
}

TEST(Utf8Test, RemovesOnlyInvalidOrIncompleteSuffixAndIsIdempotent) {
  struct Case {
    const char* name;
    std::string input;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {"empty", "", ""},
      {"ascii", "answer", "answer"},
      {"complete multibyte", "Aé你😀", "Aé你😀"},
      {"nul is a complete scalar", std::string("A\0", 2),
       std::string("A\0", 2)},
      {"truncated two-byte scalar", "A\xC2", "A"},
      {"truncated three-byte scalar", "A\xE4\xBD", "A"},
      {"truncated four-byte scalar", "A\xF0\x9F\x98", "A"},
      {"entirely incomplete", "\xE4", ""},
      {"only continuation bytes", "\x80\xBF", ""},
      {"ascii followed by continuation bytes", "A\x80\xBF", "A"},
      {"two-byte scalar followed by continuation", "é\x80", "é"},
      {"three-byte scalar followed by continuation", "A你\x80", "A你"},
      {"four-byte scalar followed by continuation", "😀\xBF", "😀"},
      {"overlong two-byte scalar", "A\xC0\x80", "A"},
      {"overlong three-byte scalar", "A\xE0\x80\x80", "A"},
      {"overlong four-byte scalar", "A\xF0\x80\x80\x80", "A"},
      {"surrogate", "A\xED\xA0\x80", "A"},
      {"above unicode maximum", "A\xF4\x90\x80\x80", "A"},
      {"consecutive invalid sequences", "A你\xC0\x80\xFF\xE4", "A你"},
      {"entirely invalid sequences", "\xFF\xC0\x80\xE4", ""},
      {"invalid interior before ascii",
       "\xFF"
       "answer",
       "\xFF"
       "answer"},
      {"invalid interior before multibyte", "\xFF你", "\xFF你"},
      {"invalid interior and suffix",
       "\xFF"
       "A\xC0\x80",
       "\xFF"
       "A"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    std::string text = test.input;
    utf8::StripIncompleteSuffix(&text);
    EXPECT_EQ(text, test.expected);
    utf8::StripIncompleteSuffix(&text);
    EXPECT_EQ(text, test.expected);
  }
}

TEST(Utf8Test, AcceptsNullSuffixOutput) {
  EXPECT_NO_THROW(utf8::StripIncompleteSuffix(nullptr));
}

}  // namespace
}  // namespace llm_edgeflow
