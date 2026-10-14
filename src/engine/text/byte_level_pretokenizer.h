#pragma once

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/text/unicode_tables.h"

namespace llm_edgeflow {
namespace text {

// byte_level_qwen2 预分词：按下面这条正则切分已规范化的 UTF-8 文本。
//
//   (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}|
//    ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
//
// 这是按分支顺序、贪婪匹配和回退规则手写的切分，不是通用正则解释器。
// 字符类别来自 UnicodeTables::Flags。输出的区间 [起, 止) 是 text 内的字节偏移，
// 依次相邻并覆盖整个文本。text 须为有效 UTF-8；输入无法切分时返回 false。
bool SplitByteLevelQwen2(const UnicodeTables& tables, std::string_view text,
                         std::vector<std::pair<size_t, size_t>>* words);

}  // namespace text
}  // namespace llm_edgeflow
