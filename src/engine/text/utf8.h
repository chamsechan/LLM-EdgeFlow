#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llm_edgeflow {
namespace utf8 {

/**
 * @brief 返回消耗的字节数；UTF-8 序列非法时返回 0。
 */
size_t DecodeCodePoint(const char* data, size_t length,
                       uint32_t* code_point) noexcept;

/**
 * @brief 将合法 Unicode 码点编码为 UTF-8，追加到已有文本。
 * @param code_point 不大于 U+10FFFF 且不在代理项范围内的码点。
 * @param text 非空输出指针，已有内容保持不变。
 */
void AppendCodePoint(uint32_t code_point, std::string* text);

/**
 * @brief 生成每个 Unicode 码点边界的字节偏移，包含 0 和 text.size()。
 * 失败时返回 false 并给出首个非法字节的偏移。
 */
bool BuildCodePointBoundaries(std::string_view text,
                              std::vector<size_t>* boundaries,
                              size_t* invalid_offset = nullptr);

/**
 * @brief 移除末尾连续的不完整或非法 UTF-8 序列，保留最后一个完整字符及
 * 之前的所有字节；不校验或改写文本内部。空指针不做处理。
 */
void StripIncompleteSuffix(std::string* text) noexcept;

}  // namespace utf8
}  // namespace llm_edgeflow
