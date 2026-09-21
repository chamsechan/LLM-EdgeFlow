#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr size_t kMaxBatchSize = 64;

constexpr size_t kMaxTextLen = 64 * 1024;  // 64KB

int DecodeOperatorRerankInput(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              const InputPortBindings& bindings,
                              AlgContext* context, AdapterStatus* status) {
  if (!ValidateDecodeRequest(source, options, context, kMaxBatchSize, status)) {
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
        source, "rerank_in", i, options, status);
    if (!in) return COMPANY_ALG_ERR_INVALID_INPUT;

    if (!IsValidInputString(in->query_text)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "Invalid query_text CompanyString", "rerank_in.query_text",
          options.converter_id.c_str(), static_cast<int>(i));
    }
    if (static_cast<size_t>(in->query_text->length) > kMaxTextLen) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "query_text length exceeds limit", "rerank_in.query_text",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    if (in->candidate_count < 1 || in->candidate_count > 8) {
      return AdapterValidationHelper::ReturnInvalidInput(
          status, "candidate_count out of valid range [1, 8]",
          "rerank_in.candidate_count", options.converter_id.c_str(),
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
            "rerank_in.candidate_passages", options.converter_id.c_str(),
            static_cast<int>(i));
      }
      if (static_cast<size_t>(pass->length) > kMaxTextLen) {
        return AdapterValidationHelper::ReturnInvalidInput(
            status, "candidate passage length exceeds limit",
            "rerank_in.candidate_passages", options.converter_id.c_str(),
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

  if (!AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kRawRequestIds), std::move(raw_req_ids),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kRerankQueries), std::move(queries),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kRerankCandidates), std::move(candidates),
          options.converter_id.c_str(), status) ||
      !AdapterValidationHelper::PublishContextValue(
          *context, bindings.Key(kRerankPairs), std::move(pairs),
          options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  return COMPANY_ALG_SUCCESS;
}

InputConverterDefinition MakeOperatorRerankInputConverter() {
  InputConverterDefinition def;
  def.converter_id = "rerank.plain.operator.v1";

  def.schema_id = "rerank.plain.request";
  def.external_type = "CompanyOperatorRerankInput";
  def.max_batch_size = kMaxBatchSize;

  def.external_slots = {
      ExternalInputSlot<CompanyOperatorRerankInput>("rerank_in")};
  def.logical_ports = {OutputPort(kRawRequestIds), OutputPort(kRerankQueries),
                       OutputPort(kRerankCandidates, "N:1"),
                       OutputPort(kRerankPairs, "N:1")};
  def.decode_fn = &DecodeOperatorRerankInput;
  return def;
}

REGISTER_INPUT_CONVERTER(MakeOperatorRerankInputConverter());

}  // namespace
}  // namespace llm_edgeflow
