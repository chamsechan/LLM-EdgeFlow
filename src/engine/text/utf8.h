#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alg_framework {
namespace utf8 {

// Returns the consumed byte count, or zero for an invalid UTF-8 sequence.
size_t DecodeCodePoint(const char* data, size_t length,
                       uint32_t* code_point) noexcept;

// Produces byte offsets for every Unicode code-point boundary, including 0
// and text.size(). Returns false and the first invalid byte offset on failure.
bool BuildCodePointBoundaries(std::string_view text,
                              std::vector<size_t>* boundaries,
                              size_t* invalid_offset = nullptr);

// Removes only an incomplete or invalid trailing code unit sequence. Complete
// UTF-8 content and any earlier bytes are left unchanged.
void StripIncompleteSuffix(std::string* text) noexcept;

}  // namespace utf8
}  // namespace alg_framework
