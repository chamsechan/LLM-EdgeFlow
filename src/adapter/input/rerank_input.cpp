#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_input_constraints.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kInputSlot = "rerank_in";

// Converter 级业务上限。Operator 允许的段落上限为 max_doc_text_bytes；
// 当前以这个更严格的上限为准，尚待方案负责人确认。
constexpr size_t kMaxCandidatePassageBytes = biz_input::kMaxTextBytes;

int DecodeOperatorRerankInput(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              AlgContext* context, AdapterStatus* status) {
  if (!ValidateDecodeRequest(source, options, context, status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  std::vector<uint64_t> raw_req_ids;
  TextBatch queries;
  RankedTextBatch candidates;
  QueryCandidatesBatch pairs;

  raw_req_ids.reserve(source.count);
  queries.reserve(source.count);

  for (size_t i = 0; i < source.count; ++i) {
    const auto* in = ReadInputSlot<CompanyOperatorRerankInput>(
        source, kInputSlot, i, options, status);
    if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;

    if (!IsValidInputString(in->query_text)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid query_text CompanyString", "rerank_in.query_text",
          options.Label().c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(in->query_text->length) >
        biz_input::kMaxTextBytes) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "query_text length exceeds limit", "rerank_in.query_text",
          options.Label().c_str(), static_cast<int>(i));
    }

    if (in->candidate_count < 1 ||
        in->candidate_count > COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status,
          "candidate_count out of valid range [1, " +
              std::to_string(COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) + "]",
          "rerank_in.candidate_count", options.Label().c_str(),
          static_cast<int>(i));
    }

    std::string query_str = CopyInputString(*in->query_text);
    raw_req_ids.push_back(in->request_id);
    queries.emplace_back(static_cast<uint32_t>(i), 0, query_str);

    for (int c = 0; c < in->candidate_count; ++c) {
      const auto* pass = in->candidate_passages[c];
      if (!IsValidInputString(pass)) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "Invalid candidate passage CompanyString",
            "rerank_in.candidate_passages", options.Label().c_str(),
            static_cast<int>(i));
      }
      if (static_cast<size_t>(pass->length) > kMaxCandidatePassageBytes) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "candidate passage length exceeds limit",
            "rerank_in.candidate_passages", options.Label().c_str(),
            static_cast<int>(i));
      }

      std::string passage = CopyInputString(*pass);
      candidates.emplace_back(
          static_cast<uint32_t>(i), static_cast<uint32_t>(c),
          RankedCandidate(passage, 0.0f, c + 1, static_cast<uint32_t>(c)));
      pairs.emplace_back(static_cast<uint32_t>(i), static_cast<uint32_t>(c),
                         QueryCandidatePair(query_str, std::move(passage)));
    }
  }

  if (!PublishRequestIds(options, std::move(raw_req_ids), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, kRerankQueries, std::move(queries), options.Label().c_str(),
          status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, kRerankCandidates, std::move(candidates),
          options.Label().c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, kRerankPairs, std::move(pairs), options.Label().c_str(),
          status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorRerankInputConverter() {
  InputConverterDefinition def;
  def.type = kInputSlot;
  def.name = "cross_rerank";
  def.service_type = kMockServiceCrossRerank;
  def.slot = ExternalInputSlot<CompanyOperatorRerankInput>(kInputSlot);
  def.logical_ports = {OutputPort(kRerankQueries),
                       OutputPort(kRerankCandidates, "N:1"),
                       OutputPort(kRerankPairs, "N:1")};
  def.decode_fn = &DecodeOperatorRerankInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorRerankInputConverter());

}  // namespace
}  // namespace llm_edgeflow
