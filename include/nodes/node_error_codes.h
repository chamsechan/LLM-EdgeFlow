#pragma once

namespace llm_edgeflow {
namespace node_error {

// Shared authoring failures and operation-specific business failures.
namespace control {
inline constexpr int kInvalidRequest = -1;
}  // namespace control

namespace text_chunk {
inline constexpr int kInvalidUtf8 = -4002;
}  // namespace text_chunk

namespace text_embedding {
inline constexpr int kSessionInferenceFailed = -5101;
}  // namespace text_embedding

namespace text_rule_match {
inline constexpr int kRegexExecutionFailed = -5002;
}  // namespace text_rule_match

namespace structured_json_parse {
inline constexpr int kParseFailed = -6102;
}  // namespace structured_json_parse

namespace text_template {
inline constexpr int kRenderedOutputTooLong = -6201;
inline constexpr int kMissingVariable = -6202;
inline constexpr int kInvalidUtf8 = -6203;
}  // namespace text_template

namespace text_rerank {
inline constexpr int kMissingInput = -7001;
}  // namespace text_rerank

namespace author_node {
inline constexpr int kMissingInput = -8001;
inline constexpr int kBusinessError = -8002;
inline constexpr int kModelCallFailed = -8003;
inline constexpr int kOutputCountMismatch = -8004;
inline constexpr int kOutputProvenanceMismatch = -8005;
inline constexpr int kInternalError = -8008;
}  // namespace author_node

}  // namespace node_error
}  // namespace llm_edgeflow
