#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kOutputSlot = "doc_out";

struct Params {
  int64_t intent_name_max_bytes = 0;
  int64_t answer_text_max_bytes = 0;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>({
      MaxBytes("intent_name", &Params::intent_name_max_bytes).Default(63),
      MaxBytes("answer_text", &Params::answer_text_max_bytes).Default(1023),
  });
}

int EncodeOperatorDocAnswer(AlgContext* context,
                            const OutputEncodeOptions& options,
                            ExternalOutputBatchView* destination,
                            size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.Label().c_str());
  }

  const auto* answers =
      ReadOutputValue(*context, kLlmAnswers, options, status, "answers");
  if (!answers) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* raw_req_ids = RequestIds(options, status);
  if (!raw_req_ids) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* intent_matches =
      ReadOutputValue(*context, kIntentMatches, options, status);
  if (!intent_matches) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* chunk_counts =
      ReadOutputValue(*context, kDocChunkCounts, options, status);
  if (!chunk_counts) return COMPANY_ALG_ERR_INVALID_INPUT;

  size_t count = answers->size();
  if (destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.Label().c_str());
  }

  std::vector<const TextBatch::value_type*> answers_by_req;
  std::vector<const RuleMatchBatch::value_type*> intents_by_req;
  std::vector<const Int32Batch::value_type*> chunks_by_req;

  if (!IndexResults(answers, raw_req_ids, &answers_by_req, "answers",
                    options.Label().c_str(), status) ||
      !IndexResults(intent_matches, raw_req_ids, &intents_by_req,
                    "intent_matches", options.Label().c_str(), status) ||
      !IndexResults(chunk_counts, raw_req_ids, &chunks_by_req,
                    "doc_chunk_counts", options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (size_t i = 0; i < count; ++i) {
    auto* out = destination->GetSlot<CompanyOperatorDocOutput>(kOutputSlot, i);
    if (!out) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, "Missing doc_out slot item", kOutputSlot,
          options.Label().c_str(), static_cast<int>(i));
    }

    out->request_id = (*raw_req_ids)[i];
    const auto& match = intents_by_req[i]->data;
    out->confidence = match.score;
    out->chunk_count = chunks_by_req[i]->data;
    out->status_code = match.status_code;

    if (!WriteOutputString(*destination, kOutputSlot, out->intent_name,
                           "intent_name", match.category, options, status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    if (!WriteOutputString(*destination, kOutputSlot, out->answer_text,
                           "answer_text", answers_by_req[i]->data, options,
                           status, i)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorDocAnswerOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "doc_qa";
  def.service_type = kMockServiceDocQa;
  def.slot = ExternalOutputSlot<CompanyOperatorDocOutput>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kLlmAnswers),
                       RequiredInputPort(kIntentMatches),
                       RequiredInputPort(kDocChunkCounts)};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorDocAnswer;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorDocAnswerOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
