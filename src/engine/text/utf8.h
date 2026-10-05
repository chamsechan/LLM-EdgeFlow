#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llm_edgeflow {
namespace utf8 {

// 返回消耗的字节数；UTF-8 序列非法时返回 0。
size_t DecodeCodePoint(const char* data, size_t length,
                       uint32_t* code_point) noexcept;

// 生成每个 Unicode 码点边界的字节偏移，包含 0 和 text.size()。
// 失败时返回 false 并给出首个非法字节的偏移。
bool BuildCodePointBoundaries(std::string_view text,
                              std::vector<size_t>* boundaries,
                              size_t* invalid_offset = nullptr);

// 只移除末尾不完整或非法的码元序列。完整的 UTF-8 内容及之前的字节
// 保持不变。
void StripIncompleteSuffix(std::string* text) noexcept;

}  // namespace utf8
}  // namespace llm_edgeflow
