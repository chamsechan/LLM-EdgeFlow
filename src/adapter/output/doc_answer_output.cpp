#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kAnswerText = MakeBlackboardKey<TextBatch>("answer_text");
constexpr auto kIntent = MakeBlackboardKey<RuleMatchBatch>("intent");
constexpr auto kChunkCount = MakeBlackboardKey<Int32Batch>("chunk_count");

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
  if (written_count) *written_count = 0;
  if (!context) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Null AlgContext passed to Encode", "context",
        options.Label().c_str());
  }

  const auto* answers =
      ReadOutputValue(*context, kAnswerText, options, status, "answers");
  if (!answers) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* intent_matches =
      ReadOutputValue(*context, kIntent, options, status);
  if (!intent_matches) return COMPANY_ALG_ERR_INVALID_INPUT;

  const auto* chunk_counts =
      ReadOutputValue(*context, kChunkCount, options, status);
  if (!chunk_counts) return COMPANY_ALG_ERR_INVALID_INPUT;

  size_t count = answers->size();
  if (!destination || destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.Label().c_str());
  }

  std::vector<const TextBatch::value_type*> answers_by_req;
  std::vector<const RuleMatchBatch::value_type*> intents_by_req;
  std::vector<const Int32Batch::value_type*> chunks_by_req;

  if (!IndexResults(answers, destination->count, &answers_by_req, "answers",
                    options.Label().c_str(), status) ||
      !IndexResults(intent_matches, destination->count, &intents_by_req,
                    "intent_matches", options.Label().c_str(), status) ||
      !IndexResults(chunk_counts, destination->count, &chunks_by_req,
                    "doc_chunk_counts", options.Label().c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  size_t written = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!destination->HasSlot(kOutputSlot, i) && !destination->required) {
      continue;
    }
    DocumentOutputValue out;

    const auto& match = intents_by_req[i]->data;
    out.confidence = match.score;
    out.chunk_count = chunks_by_req[i]->data;
    out.status_code = match.status_code;

    out.intent_name = match.category;
    out.answer_text = answers_by_req[i]->data;
    if (!WriteOutputValue(*destination, kOutputSlot, i, out, options, status)) {
      return status ? status->Code() : COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
    ++written;
  }

  if (written_count) *written_count = written;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeOperatorDocAnswerOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "doc_qa";
  def.slot = ExternalOutputSlot<DocumentOutputValue>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kAnswerText),
                       RequiredInputPort(kIntent),
                       RequiredInputPort(kChunkCount)};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorDocAnswer;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorDocAnswerOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
