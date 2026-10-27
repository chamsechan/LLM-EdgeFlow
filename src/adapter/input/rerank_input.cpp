#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/input_limits.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "contracts/error_codes.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kQueryText = MakeBlackboardKey<TextBatch>("query_text");
constexpr auto kCandidates = MakeBlackboardKey<RankedTextBatch>("candidates");

constexpr const char* kInputSlot = "rerank_in";

// Converter 级业务上限，比 Operator 允许的段落上限 max_doc_text_bytes
// 更严格；此处以该上限为准。
constexpr size_t kMaxCandidatePassageBytes = input_limits::kMaxTextBytes;

int DecodeOperatorRerankInput(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              AlgContext* context, AdapterStatus* status) {
  if (!ValidateDecodeRequest(source, options, context, status)) {
    return error::kInvalidInput;
  }

  TextBatch queries;
  RankedTextBatch candidates;

  queries.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    auto in =
        ReadInputSlot<RerankInputValue>(source, kInputSlot, i, options, status);
    if (!in) return error::kInvalidInput;

    if (in->candidate_passages.empty()) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "At least one candidate passage is required",
          "rerank_in.candidate_passages", options.Label().c_str(),
          static_cast<int>(i));
    }

    const std::string& query_str = in->query_text;
    queries.emplace_back(static_cast<uint32_t>(i), 0, query_str);

    for (size_t c = 0; c < in->candidate_passages.size(); ++c) {
      const auto& passage = in->candidate_passages[c];
      if (passage.size() > kMaxCandidatePassageBytes) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "candidate passage length exceeds limit",
            "rerank_in.candidate_passages", options.Label().c_str(),
            static_cast<int>(i));
      }

      candidates.emplace_back(
          static_cast<uint32_t>(i), static_cast<uint32_t>(c),
          RankedCandidate(passage, 0.0f, static_cast<int>(c + 1),
                          static_cast<uint32_t>(c)));
    }
  }

  if (!AdapterValidationHelper::PublishContextValue(
          *context, options.Port(kQueryText.name), std::move(queries),
          options.Label().c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, options.Port(kCandidates.name), std::move(candidates),
          options.Label().c_str(), status)) {
    return error::kInvalidInput;
  }

  return error::kOk;
}

InputConverterDefinition MakeOperatorRerankInputConverter() {
  InputConverterDefinition def;
  def.type = kInputSlot;
  def.name = "cross_rerank";
  def.slot = ExternalInputSlot<RerankInputValue>();
  def.logical_ports = {OutputPort(kQueryText), OutputPort(kCandidates, "N:1")};
  def.decode_fn = &DecodeOperatorRerankInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorRerankInputConverter());

}  // namespace
}  // namespace llm_edgeflow
