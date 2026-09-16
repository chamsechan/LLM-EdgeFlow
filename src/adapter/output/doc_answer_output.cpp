#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/c_api.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

int EncodeCAbiDocAnswer(AlgContext* context, const OutputPortBindings& bindings,
                        const OutputEncodeOptions& options,
                        ExternalOutputBatchView* destination,
                        size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* answers =
      context->Read<TextBatch>(bindings.GetActualKey("llm_answers"));
  if (!answers) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: llm_answers", "answers",
        options.converter_id.c_str());
  }

  const auto* raw_req_ids = context->Read<std::vector<uint64_t>>(
      bindings.GetActualKey("raw_request_ids"));
  if (!raw_req_ids) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: raw_request_ids",
        "raw_request_ids", options.converter_id.c_str());
  }

  const auto* intent_matches =
      context->Read<RuleMatchBatch>(bindings.GetActualKey("intent_matches"));
  if (!intent_matches) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: intent_matches",
        "intent_matches", options.converter_id.c_str());
  }

  const auto* chunk_counts =
      context->Read<Int32Batch>(bindings.GetActualKey("doc_chunk_counts"));
  if (!chunk_counts) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: doc_chunk_counts",
        "doc_chunk_counts", options.converter_id.c_str());
  }

  int count = static_cast<int>(answers->size());
  int cap = static_cast<int>(destination->capacity > 0 ? destination->capacity
                                                       : destination->count);
  int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
      destination->items, &cap, count, options.converter_id.c_str(), status);
  if (valid_ret != 0) return valid_ret;

  std::vector<const TextBatch::value_type*> answers_by_req;
  std::vector<const RuleMatchBatch::value_type*> intents_by_req;
  std::vector<const Int32Batch::value_type*> chunks_by_req;

  if (!IndexResults(answers, raw_req_ids, &answers_by_req, "answers",
                    options.converter_id.c_str(), status) ||
      !IndexResults(intent_matches, raw_req_ids, &intents_by_req,
                    "intent_matches", options.converter_id.c_str(), status) ||
      !IndexResults(chunk_counts, raw_req_ids, &chunks_by_req,
                    "doc_chunk_counts", options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (int i = 0; i < count; ++i) {
    auto* out_ptr = destination->GetCAbi<CompanyDocOutputStruct>(i);
    out_ptr->request_id = (*raw_req_ids)[i];

    const auto& match = intents_by_req[i]->data;
    out_ptr->confidence = match.score;
    out_ptr->chunk_count = chunks_by_req[i]->data;
    out_ptr->status_code = match.status_code;

    if (!AdapterValidationHelper::CheckedStringCopy(
            out_ptr->intent_name, sizeof(out_ptr->intent_name),
            match.category.c_str(), "outputs[i].intent_name", i,
            options.converter_id.c_str(), status)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    if (!AdapterValidationHelper::CheckedStringCopy(
            out_ptr->answer_text, sizeof(out_ptr->answer_text),
            answers_by_req[i]->data.c_str(), "outputs[i].answer_text", i,
            options.converter_id.c_str(), status)) {
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }
  }

  if (written_count) *written_count = static_cast<size_t>(count);
  return COMPANY_ALG_SUCCESS;
}

int EncodeOperatorDocAnswer(AlgContext* context,
                            const OutputPortBindings& bindings,
                            const OutputEncodeOptions& options,
                            ExternalOutputBatchView* destination,
                            size_t* written_count, AdapterStatus* status) {
  if (!context) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Null AlgContext passed to Encode", "context",
        options.converter_id.c_str());
  }

  const auto* answers =
      context->Read<TextBatch>(bindings.GetActualKey("llm_answers"));
  if (!answers) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: llm_answers", "answers",
        options.converter_id.c_str());
  }

  const auto* raw_req_ids = context->Read<std::vector<uint64_t>>(
      bindings.GetActualKey("raw_request_ids"));
  if (!raw_req_ids) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Missing required context value: raw_request_ids",
        "raw_request_ids", options.converter_id.c_str());
  }

  const auto* intent_matches =
      context->Read<RuleMatchBatch>(bindings.GetActualKey("intent_matches"));
  if (!intent_matches) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: intent_matches",
        "intent_matches", options.converter_id.c_str());
  }

  const auto* chunk_counts =
      context->Read<Int32Batch>(bindings.GetActualKey("doc_chunk_counts"));
  if (!chunk_counts) {
    return AdapterValidationHelper::ReturnInvalidInput(
        status, "Missing required context value: doc_chunk_counts",
        "doc_chunk_counts", options.converter_id.c_str());
  }

  size_t count = answers->size();
  if (destination->count < count) {
    return AdapterValidationHelper::ReturnBufferTooSmall(
        status, "Destination item count is less than output count",
        "destination", options.converter_id.c_str());
  }

  std::vector<const TextBatch::value_type*> answers_by_req;
  std::vector<const RuleMatchBatch::value_type*> intents_by_req;
  std::vector<const Int32Batch::value_type*> chunks_by_req;

  if (!IndexResults(answers, raw_req_ids, &answers_by_req, "answers",
                    options.converter_id.c_str(), status) ||
      !IndexResults(intent_matches, raw_req_ids, &intents_by_req,
                    "intent_matches", options.converter_id.c_str(), status) ||
      !IndexResults(chunk_counts, raw_req_ids, &chunks_by_req,
                    "doc_chunk_counts", options.converter_id.c_str(), status)) {
    return COMPANY_ALG_ERR_INVALID_INPUT;
  }

  for (size_t i = 0; i < count; ++i) {
    auto* out = destination->GetSlot<CompanyOperatorDocOutput>("doc_out", i);
    if (!out) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status, "Missing doc_out slot item", "doc_out",
          options.converter_id.c_str(), static_cast<int>(i));
    }

    out->request_id = (*raw_req_ids)[i];
    const auto& match = intents_by_req[i]->data;
    out->confidence = match.score;
    out->chunk_count = chunks_by_req[i]->data;
    out->status_code = match.status_code;

    std::string err;
    int ret = CopyToOperatorString(
        match.category.c_str(), out->intent_name,
        destination->GetSlotCapacity("doc_out", "intent_name", 63),
        "intent_name", &err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status,
          err.empty() ? "Buffer too small for intent_name" : err.c_str(),
          "intent_name", options.converter_id.c_str(), static_cast<int>(i));
    }

    ret = CopyToOperatorString(
        answers_by_req[i]->data.c_str(), out->answer_text,
        destination->GetSlotCapacity("doc_out", "answer_text", 1023),
        "answer_text", &err);
    if (ret != 0) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          status,
          err.empty() ? "Buffer too small for answer_text" : err.c_str(),
          "answer_text", options.converter_id.c_str(), static_cast<int>(i));
    }
  }

  if (written_count) *written_count = count;
  return COMPANY_ALG_SUCCESS;
}

OutputConverterDefinition MakeCAbiDocAnswerOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "doc_answer.plain.cabi.v1";
  def.transport = "cabi";
  def.schema_id = "doc_answer.plain.response";
  def.schema_version = 1;
  def.external_type = "CompanyDocOutputStruct";
  def.cardinality = "1:1";
  def.max_batch_size = 64;
  def.capacity_policy = "reject_overflow";
  def.thread_model = "stateless";
  def.external_slots = {{"outputs",
                         "CompanyDocOutputStruct",
                         PortDirection::kOutput,
                         true,
                         "CompanyDocOutputStruct",
                         "",
                         {"intent_name", "answer_text"}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("llm_answers", "TextBatch", true, "1:1"),
      NodePortDefinition("intent_matches", "RuleMatchBatch", true, "1:1"),
      NodePortDefinition("doc_chunk_counts", "Int32Batch", true, "1:1")};
  def.encode_fn = &EncodeCAbiDocAnswer;
  return def;
}

OutputConverterDefinition MakeOperatorDocAnswerOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "doc_answer.plain.operator.v1";
  def.transport = "operator";
  def.schema_id = "doc_answer.plain.response";
  def.schema_version = 1;
  def.external_type = "CompanyOperatorDocOutput";
  def.cardinality = "1:1";
  def.max_batch_size = 64;
  def.capacity_policy = "reject_overflow";
  def.thread_model = "stateless";
  def.external_slots = {{"doc_out",
                         "CompanyOperatorDocOutput",
                         PortDirection::kOutput,
                         true,
                         "CompanyOperatorDocOutput",
                         "doc_out",
                         {"intent_name", "answer_text"}}};
  def.logical_ports = {
      NodePortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      NodePortDefinition("llm_answers", "TextBatch", true, "1:1"),
      NodePortDefinition("intent_matches", "RuleMatchBatch", true, "1:1"),
      NodePortDefinition("doc_chunk_counts", "Int32Batch", true, "1:1")};
  def.encode_fn = &EncodeOperatorDocAnswer;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeCAbiDocAnswerOutputConverter());
REGISTER_OUTPUT_CONVERTER(MakeOperatorDocAnswerOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
