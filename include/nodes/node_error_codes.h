#pragma once

namespace alg_framework {
namespace node_error {

// Runtime node errors use non-overlapping per-node numeric ranges.
namespace control {
inline constexpr int kInvalidRequest = -1;
}  // namespace control

namespace vector_top_k {
inline constexpr int kMissingInput = -3101;
}  // namespace vector_top_k

namespace text_chunk {
inline constexpr int kMissingInput = -4001;
inline constexpr int kInvalidUtf8 = -4002;
}  // namespace text_chunk

namespace text_embedding {
inline constexpr int kMissingInput = -4101;
inline constexpr int kOutputCountMismatch = -4102;
inline constexpr int kOutputProvenanceMismatch = -4103;
inline constexpr int kSessionInferenceFailed = -5101;
}  // namespace text_embedding

namespace llm_generate {
inline constexpr int kMissingInput = -4301;
inline constexpr int kOutputCountMismatch = -4302;
inline constexpr int kOutputProvenanceMismatch = -4303;
}  // namespace llm_generate

namespace text_rule_match {
inline constexpr int kMissingInput = -5001;
inline constexpr int kRegexExecutionFailed = -5002;
}  // namespace text_rule_match

namespace structured_json_parse {
inline constexpr int kMissingInput = -6101;
inline constexpr int kParseFailed = -6102;
}  // namespace structured_json_parse

namespace text_template {
inline constexpr int kRenderedOutputTooLong = -6201;
inline constexpr int kMissingVariable = -6202;
inline constexpr int kInvalidUtf8 = -6203;
}  // namespace text_template

namespace text_rerank {
inline constexpr int kMissingInput = -7001;
inline constexpr int kModelOutputMismatch = -7002;
}  // namespace text_rerank

namespace ocr_detect {
inline constexpr int kMissingInput = -7101;
inline constexpr int kOutputCountMismatch = -7102;
inline constexpr int kOutputProvenanceMismatch = -7103;
}  // namespace ocr_detect

namespace asr_transcribe {
inline constexpr int kMissingInput = -7201;
inline constexpr int kOutputCountMismatch = -7202;
inline constexpr int kOutputProvenanceMismatch = -7203;
}  // namespace asr_transcribe

}  // namespace node_error
}  // namespace alg_framework
