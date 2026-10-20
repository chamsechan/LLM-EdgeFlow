#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/json_output_parameters.h"
#include "adapter/result_validation.h"
#include "contracts/error_codes.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {
namespace {

constexpr auto kAnswerText = MakeBlackboardKey<TextBatch>("answer_text");
constexpr auto kIntent = MakeBlackboardKey<RuleMatchBatch>("intent");
constexpr auto kChunkCount = MakeBlackboardKey<Int32Batch>("chunk_count");

constexpr const char* kOutputSlot = "json_out";

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
  if (!answers) return error::kInvalidInput;

  const auto* intent_matches =
      ReadOutputValue(*context, kIntent, options, status);
  if (!intent_matches) return error::kInvalidInput;

  const auto* chunk_counts =
      ReadOutputValue(*context, kChunkCount, options, status);
  if (!chunk_counts) return error::kInvalidInput;

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
    return error::kInvalidInput;
  }

  size_t written = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!destination->HasSlot(kOutputSlot, i) && !destination->required) {
      continue;
    }
    JsonOutputValue out;

    const auto& match = intents_by_req[i]->data;
    out.result_json = nlohmann::json({{"intent_name", match.category},
                                      {"confidence", match.score},
                                      {"answer_text", answers_by_req[i]->data},
                                      {"chunk_count", chunks_by_req[i]->data}})
                          .dump();
    if (!WriteOutputValue(*destination, kOutputSlot, i, out, options, status)) {
      return status ? status->Code() : error::kInvalidOutput;
    }
    ++written;
  }

  if (written_count) *written_count = written;
  return error::kOk;
}

OutputConverterDefinition MakeOperatorDocAnswerOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "doc_qa";
  def.slot = ExternalOutputSlot<JsonOutputValue>();
  def.logical_ports = {RequiredInputPort(kAnswerText),
                       RequiredInputPort(kIntent),
                       RequiredInputPort(kChunkCount)};
  def.params = JsonOutputParameters();
  def.encode_fn = &EncodeOperatorDocAnswer;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorDocAnswerOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
