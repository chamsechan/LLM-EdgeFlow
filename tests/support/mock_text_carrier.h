#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow::test_support {

// 一份文本请求 envelope：MockInput -> MockDataPool -> MockTextInput。
// 成员互相引用，地址必须稳定，因此不可复制或移动。
struct MockTextCarrier {
  explicit MockTextCarrier(std::string value = {}) : text(std::move(value)) {
    leaf = {static_cast<int32_t>(sizeof(MockTextInput)), text.data(),
            static_cast<int32_t>(text.size())};
    pool = {static_cast<int32_t>(sizeof(MockDataPool)), kMockDataText, &leaf};
    root = {static_cast<int32_t>(sizeof(MockInput)), kMockInputMultidata, 1,
            &pool};
  }
  MockTextCarrier(const MockTextCarrier&) = delete;
  MockTextCarrier& operator=(const MockTextCarrier&) = delete;

  std::string text;
  MockTextInput leaf{};
  MockDataPool pool{};
  MockInput root{};
};

inline std::string_view MockOutputText(const void* output) {
  const auto* out = static_cast<const MockOutput*>(output);
  if (!out || !out->text || !out->text->value) return {};
  return {out->text->value, static_cast<size_t>(out->text->length)};
}

}  // namespace llm_edgeflow::test_support
