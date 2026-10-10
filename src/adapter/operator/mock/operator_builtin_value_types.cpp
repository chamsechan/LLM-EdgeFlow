#include "adapter/operator/mock/operator_builtin_value_types.h"

#include "adapter/io_values.h"
#include "adapter/operator/mock/platform_value_binding.h"
#include "adapter/operator/operator_value_type_registry.h"

namespace llm_edgeflow {
namespace {
std::string ReadString(const CompanyString* value) {
  return !value || value->length == 0 ? std::string{}
                                      : std::string(value->data, value->length);
}
AdapterStatus WriteString(CompanyString* output, const std::string& text,
                          const char* field,
                          const ResolvedOutputPoolSpec& spec) {
  const auto it = spec.capacities.find(field);
  if (it == spec.capacities.end())
    return AdapterStatus::BufferTooSmall(
        "Missing output capacity specification", field);
  std::string error;
  const int code =
      CopyToOperatorString(text, output, it->second, field, &error);
  return AdapterStatus(code, std::move(error), code == 0 ? "" : field);
}
}  // namespace

void RegisterMockOperatorBindings(OperatorValueTypeRegistry& registry) {
  // 1. string -> CompanyString
  {
    auto binding = MakeTypedInputBinding<CompanyString>(
        "string",
        [](const CompanyString& in, const InputLimits& limits,
           std::string* err) -> int {
          return ValidateCompanyString(&in, limits.max_text_bytes, "string",
                                       err);
        });
    SetInputValue<CompanyString, std::string>(
        &binding, [](const CompanyString& in) { return ReadString(&in); });
    registry.RegisterBinding(std::move(binding));
  }

  // 2. buffer -> CompanyBuffer
  {
    auto binding = MakeTypedInputBinding<CompanyBuffer>(
        "buffer",
        [](const CompanyBuffer& in, const InputLimits& limits,
           std::string* err) -> int {
          return ValidateCompanyBuffer(&in, limits.max_buffer_bytes, "buffer",
                                       err);
        });
    registry.RegisterBinding(std::move(binding));
  }

  // 3. any -> CompanyAny
  {
    auto binding = MakeTypedInputBinding<CompanyAny>(
        "any",
        [](const CompanyAny& in, const InputLimits& limits,
           std::string* err) -> int {
          return ValidateCompanyAnyPayload(&in, limits.max_any_bytes, "any",
                                           err);
        });
    registry.RegisterBinding(std::move(binding));
  }

  // 4. frame -> CompanyFrame
  {
    auto binding = MakeTypedInputBinding<CompanyFrame>(
        "frame",
        [](const CompanyFrame& in, const InputLimits& limits,
           std::string* err) -> int {
          if (in.width <= 0 || in.height <= 0 || in.stride <= 0 ||
              static_cast<uint64_t>(in.stride) <
                  static_cast<uint64_t>(in.width) * 3) {
            if (err)
              *err =
                  "CompanyFrame requires positive dimensions and RGB8 stride "
                  ">= width * 3";
            return COMPANY_ALG_ERR_INVALID_INPUT;
          }
          if (!in.data) {
            if (err) *err = "CompanyFrame.data is null";
            return COMPANY_ALG_ERR_INVALID_INPUT;
          }
          if (static_cast<uint64_t>(in.stride) * in.height >
              limits.max_image_bytes) {
            if (err) *err = "CompanyFrame pixels exceed max_image_bytes";
            return COMPANY_ALG_ERR_INVALID_INPUT;
          }
          int ret = 0;
          if (in.metadata) {
            ret = ValidateCompanyAnyPayload(in.metadata, limits.max_any_bytes,
                                            "CompanyFrame.metadata", err);
            if (ret != 0) return ret;
          }
          return 0;
        });
    SetServiceTypeMember(&binding, &CompanyFrame::service_type);
    SetInputValue<CompanyFrame, ImageInputValue>(
        &binding, [](const CompanyFrame& in) {
          ImageFrame frame;
          frame.width = in.width;
          frame.height = in.height;
          frame.stride = static_cast<size_t>(in.stride);
          const auto* pixels = static_cast<const uint8_t*>(in.data);
          frame.data.assign(pixels, pixels + frame.stride * frame.height);
          return ImageInputValue{std::move(frame)};
        });
    binding.services = {{"ocr_invoice_qa", kMockServiceOcrInvoiceQa}};
    registry.RegisterBinding(std::move(binding));
  }

  // 5. od_out -> CompanyOdOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOdOutput>(
        "od_out", {{"result_json", &CompanyOdOutput::result_json, {65536}}},
        [](CompanyOdOutput& out) noexcept {
          out.service_type = 0;
          out.detected_box_count = 0;
          out.status_code = 0;
        },
        &CompanyOdOutput::metadata, 65536);
    SetServiceTypeMember(&binding, &CompanyOdOutput::service_type);
    SetOutputValue<CompanyOdOutput, DetectionOutputValue>(
        &binding, [](CompanyOdOutput& out, const DetectionOutputValue& value,
                     const ResolvedOutputPoolSpec& spec) {
          {
            auto status = WriteString(out.result_json, value.result_json,
                                      "result_json", spec);
            if (!status.IsOk()) return status;
          }
          out.detected_box_count = value.detected_box_count;
          out.status_code = value.status_code;
          return AdapterStatus::Ok();
        });
    binding.services = {{"ocr_invoice_qa", kMockServiceOcrInvoiceQa}};
    registry.RegisterBinding(std::move(binding));
  }

  // 6. keyword_in -> CompanyOperatorKeywordInput
  {
    auto binding = MakeTypedInputBinding<CompanyOperatorKeywordInput>(
        "keyword_in",
        [](const CompanyOperatorKeywordInput& in, const InputLimits& limits,
           std::string* err) -> int {
          return ValidateCompanyString(in.sentence_text, limits.max_text_bytes,
                                       "sentence_text", err);
        });
    SetServiceTypeMember(&binding, &CompanyOperatorKeywordInput::service_type);
    SetInputValue<CompanyOperatorKeywordInput, TextInputValue>(
        &binding, [](const CompanyOperatorKeywordInput& in) {
          return TextInputValue{ReadString(in.sentence_text)};
        });
    binding.services = {{"keyword_match", kMockServiceKeywordMatch}};
    registry.RegisterBinding(std::move(binding));
  }

  // 7. keyword_out -> CompanyOperatorKeywordOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOperatorKeywordOutput>(
        "keyword_out",
        {{"match_result_json",
          &CompanyOperatorKeywordOutput::match_result_json,
          {65536}}},
        [](CompanyOperatorKeywordOutput& out) noexcept {
          out.service_type = 0;
          out.is_hit = 0;
          out.status_code = 0;
        });
    SetServiceTypeMember(&binding, &CompanyOperatorKeywordOutput::service_type);
    SetOutputValue<CompanyOperatorKeywordOutput, KeywordOutputValue>(
        &binding,
        [](CompanyOperatorKeywordOutput& out, const KeywordOutputValue& value,
           const ResolvedOutputPoolSpec& spec) {
          {
            auto status =
                WriteString(out.match_result_json, value.match_result_json,
                            "match_result_json", spec);
            if (!status.IsOk()) return status;
          }
          out.is_hit = value.is_hit;
          out.status_code = value.status_code;
          return AdapterStatus::Ok();
        });
    binding.services = {{"keyword_match", kMockServiceKeywordMatch}};
    registry.RegisterBinding(std::move(binding));
  }

  // 8. entity_in -> CompanyOperatorEntityInput
  {
    auto binding = MakeTypedInputBinding<CompanyOperatorEntityInput>(
        "entity_in",
        [](const CompanyOperatorEntityInput& in, const InputLimits& limits,
           std::string* err) -> int {
          return ValidateCompanyString(in.sentence_text, limits.max_text_bytes,
                                       "sentence_text", err);
        });
    SetServiceTypeMember(&binding, &CompanyOperatorEntityInput::service_type);
    SetInputValue<CompanyOperatorEntityInput, TextInputValue>(
        &binding, [](const CompanyOperatorEntityInput& in) {
          return TextInputValue{ReadString(in.sentence_text)};
        });
    binding.services = {{"entity_extract", kMockServiceEntityExtract},
                        {"translate", kMockServiceTranslate}};
    registry.RegisterBinding(std::move(binding));
  }

  // 9. entity_out -> CompanyOperatorEntityOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOperatorEntityOutput>(
        "entity_out",
        {{"entities_json",
          &CompanyOperatorEntityOutput::entities_json,
          {65536}}},
        [](CompanyOperatorEntityOutput& out) noexcept {
          out.service_type = 0;
          out.status_code = 0;
        });
    SetServiceTypeMember(&binding, &CompanyOperatorEntityOutput::service_type);
    SetOutputValue<CompanyOperatorEntityOutput, EntityOutputValue>(
        &binding,
        [](CompanyOperatorEntityOutput& out, const EntityOutputValue& value,
           const ResolvedOutputPoolSpec& spec) {
          {
            auto status = WriteString(out.entities_json, value.entities_json,
                                      "entities_json", spec);
            if (!status.IsOk()) return status;
          }
          out.status_code = value.status_code;
          return AdapterStatus::Ok();
        });
    binding.services = {{"entity_extract", kMockServiceEntityExtract},
                        {"translate", kMockServiceTranslate}};
    registry.RegisterBinding(std::move(binding));
  }

  // 10. doc_in -> CompanyOperatorDocInput
  {
    auto binding = MakeTypedInputBinding<CompanyOperatorDocInput>(
        "doc_in",
        [](const CompanyOperatorDocInput& in, const InputLimits& limits,
           std::string* err) -> int {
          int ret = ValidateCompanyString(in.query_text, limits.max_text_bytes,
                                          "query_text", err);
          if (ret != 0) return ret;
          if (in.doc_text) {
            ret = ValidateCompanyString(in.doc_text, limits.max_doc_text_bytes,
                                        "doc_text", err);
            if (ret != 0) return ret;
          }
          return 0;
        });
    SetServiceTypeMember(&binding, &CompanyOperatorDocInput::service_type);
    SetInputValue<CompanyOperatorDocInput, DocumentInputValue>(
        &binding, [](const CompanyOperatorDocInput& in) {
          return DocumentInputValue{ReadString(in.doc_text),
                                    ReadString(in.query_text)};
        });
    binding.services = {{"doc_qa", kMockServiceDocQa}};
    registry.RegisterBinding(std::move(binding));
  }

  // 11. doc_out -> CompanyOperatorDocOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOperatorDocOutput>(
        "doc_out",
        {{"intent_name", &CompanyOperatorDocOutput::intent_name, {255}},
         {"answer_text", &CompanyOperatorDocOutput::answer_text, {65536}}},
        [](CompanyOperatorDocOutput& out) noexcept {
          out.service_type = 0;
          out.confidence = 0.0f;
          out.chunk_count = 0;
          out.status_code = 0;
        });
    SetServiceTypeMember(&binding, &CompanyOperatorDocOutput::service_type);
    SetOutputValue<CompanyOperatorDocOutput, DocumentOutputValue>(
        &binding,
        [](CompanyOperatorDocOutput& out, const DocumentOutputValue& value,
           const ResolvedOutputPoolSpec& spec) {
          {
            auto status = WriteString(out.intent_name, value.intent_name,
                                      "intent_name", spec);
            if (!status.IsOk()) return status;
          }
          {
            auto status = WriteString(out.answer_text, value.answer_text,
                                      "answer_text", spec);
            if (!status.IsOk()) return status;
          }
          out.confidence = value.confidence;
          out.chunk_count = value.chunk_count;
          out.status_code = value.status_code;
          return AdapterStatus::Ok();
        });
    binding.services = {{"doc_qa", kMockServiceDocQa}};
    registry.RegisterBinding(std::move(binding));
  }

  // 12. audit_in -> CompanyOperatorAuditInput
  {
    auto binding = MakeTypedInputBinding<CompanyOperatorAuditInput>(
        "audit_in",
        [](const CompanyOperatorAuditInput& in, const InputLimits& limits,
           std::string* err) -> int {
          int ret = ValidateCompanyString(in.user_text, limits.max_text_bytes,
                                          "user_text", err);
          if (ret != 0) return ret;
          if (in.channel_name) {
            ret = ValidateCompanyString(in.channel_name,
                                        input_limits::kMaxChannelNameBytes,
                                        "channel_name", err);
            if (ret != 0) return ret;
          }
          return 0;
        });
    SetServiceTypeMember(&binding, &CompanyOperatorAuditInput::service_type);
    SetInputValue<CompanyOperatorAuditInput, AuditInputValue>(
        &binding, [](const CompanyOperatorAuditInput& in) {
          return AuditInputValue{ReadString(in.user_text),
                                 ReadString(in.channel_name)};
        });
    binding.services = {{"dialogue_audit", kMockServiceDialogueAudit}};
    registry.RegisterBinding(std::move(binding));
  }

  // 13. audit_out -> CompanyOperatorAuditOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOperatorAuditOutput>(
        "audit_out",
        {{"risk_level", &CompanyOperatorAuditOutput::risk_level, {255}},
         {"matched_policy_clause",
          &CompanyOperatorAuditOutput::matched_policy_clause,
          {4096}},
         {"audit_verdict_json",
          &CompanyOperatorAuditOutput::audit_verdict_json,
          {65536}}},
        [](CompanyOperatorAuditOutput& out) noexcept {
          out.service_type = 0;
          out.risk_score = 0.0f;
          out.status_code = 0;
        });
    SetServiceTypeMember(&binding, &CompanyOperatorAuditOutput::service_type);
    SetOutputValue<CompanyOperatorAuditOutput, AuditOutputValue>(
        &binding,
        [](CompanyOperatorAuditOutput& out, const AuditOutputValue& value,
           const ResolvedOutputPoolSpec& spec) {
          {
            auto status = WriteString(out.risk_level, value.risk_level,
                                      "risk_level", spec);
            if (!status.IsOk()) return status;
          }
          {
            auto status = WriteString(out.matched_policy_clause,
                                      value.matched_policy_clause,
                                      "matched_policy_clause", spec);
            if (!status.IsOk()) return status;
          }
          {
            auto status =
                WriteString(out.audit_verdict_json, value.audit_verdict_json,
                            "audit_verdict_json", spec);
            if (!status.IsOk()) return status;
          }
          out.risk_score = value.risk_score;
          out.status_code = value.status_code;
          return AdapterStatus::Ok();
        });
    binding.services = {{"dialogue_audit", kMockServiceDialogueAudit}};
    registry.RegisterBinding(std::move(binding));
  }

  // 14. audio_in -> CompanyOperatorAudioInput
  {
    auto binding = MakeTypedInputBinding<CompanyOperatorAudioInput>(
        "audio_in",
        [](const CompanyOperatorAudioInput& in, const InputLimits& limits,
           std::string* err) -> int {
          if (in.sample_rate < limits.min_sample_rate ||
              in.sample_rate > limits.max_sample_rate) {
            if (err) {
              *err = "sample_rate " + std::to_string(in.sample_rate) +
                     " out of valid range [" +
                     std::to_string(limits.min_sample_rate) + ", " +
                     std::to_string(limits.max_sample_rate) + "]";
            }
            return COMPANY_ALG_ERR_INVALID_INPUT;
          }
          if (in.pcm_length < 0 ||
              in.pcm_length > limits.max_audio_pcm_samples ||
              static_cast<size_t>(in.pcm_length) >
                  limits.max_audio_pcm_bytes / sizeof(float)) {
            if (err)
              *err = "pcm_length " + std::to_string(in.pcm_length) +
                     " invalid or exceeds limit " +
                     std::to_string(limits.max_audio_pcm_samples);
            return COMPANY_ALG_ERR_INVALID_INPUT;
          }
          if (in.pcm_length > 0 && !in.pcm_buffer) {
            if (err) *err = "pcm_buffer pointer is null";
            return COMPANY_ALG_ERR_INVALID_INPUT;
          }
          return 0;
        });
    SetServiceTypeMember(&binding, &CompanyOperatorAudioInput::service_type);
    SetInputValue<CompanyOperatorAudioInput, AudioInputValue>(
        &binding, [](const CompanyOperatorAudioInput& in) {
          AudioInputValue value;
          value.sample_rate = in.sample_rate;
          if (in.pcm_length)
            value.pcm.assign(in.pcm_buffer, in.pcm_buffer + in.pcm_length);
          return value;
        });
    binding.services = {{"audio_asr_intent", kMockServiceAudioAsrIntent}};
    registry.RegisterBinding(std::move(binding));
  }

  // 15. audio_out -> CompanyOperatorAudioOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOperatorAudioOutput>(
        "audio_out",
        {{"transcribed_text",
          &CompanyOperatorAudioOutput::transcribed_text,
          {16384}},
         {"intent_slot_json",
          &CompanyOperatorAudioOutput::intent_slot_json,
          {65536}}},
        [](CompanyOperatorAudioOutput& out) noexcept {
          out.service_type = 0;
          out.status_code = 0;
        });
    SetServiceTypeMember(&binding, &CompanyOperatorAudioOutput::service_type);
    SetOutputValue<CompanyOperatorAudioOutput, AudioOutputValue>(
        &binding,
        [](CompanyOperatorAudioOutput& out, const AudioOutputValue& value,
           const ResolvedOutputPoolSpec& spec) {
          {
            auto status =
                WriteString(out.transcribed_text, value.transcribed_text,
                            "transcribed_text", spec);
            if (!status.IsOk()) return status;
          }
          {
            auto status =
                WriteString(out.intent_slot_json, value.intent_slot_json,
                            "intent_slot_json", spec);
            if (!status.IsOk()) return status;
          }
          out.status_code = value.status_code;
          return AdapterStatus::Ok();
        });
    binding.services = {{"audio_asr_intent", kMockServiceAudioAsrIntent}};
    registry.RegisterBinding(std::move(binding));
  }

  // 16. rerank_in -> CompanyOperatorRerankInput
  {
    auto binding = MakeTypedInputBinding<CompanyOperatorRerankInput>(
        "rerank_in",
        [](const CompanyOperatorRerankInput& in, const InputLimits& limits,
           std::string* err) -> int {
          int ret = ValidateCompanyString(in.query_text, limits.max_text_bytes,
                                          "query_text", err);
          if (ret != 0) return ret;
          if (in.candidate_count <= 0 ||
              in.candidate_count > COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) {
            if (err) {
              *err = "candidate_count " + std::to_string(in.candidate_count) +
                     " out of valid range [1, " +
                     std::to_string(COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) +
                     "]";
            }
            return COMPANY_ALG_ERR_INVALID_INPUT;
          }
          for (int i = 0; i < in.candidate_count; ++i) {
            if (!in.candidate_passages[i]) {
              if (err)
                *err = "candidate_passages[" + std::to_string(i) + "] is null";
              return COMPANY_ALG_ERR_INVALID_INPUT;
            }
            ret = ValidateCompanyString(
                in.candidate_passages[i], limits.max_doc_text_bytes,
                ("candidate_passages[" + std::to_string(i) + "]").c_str(), err);
            if (ret != 0) return ret;
          }
          return 0;
        });
    SetServiceTypeMember(&binding, &CompanyOperatorRerankInput::service_type);
    SetInputValue<CompanyOperatorRerankInput, RerankInputValue>(
        &binding, [](const CompanyOperatorRerankInput& in) {
          RerankInputValue value;
          value.query_text = ReadString(in.query_text);
          for (int i = 0; i < in.candidate_count; ++i)
            value.candidate_passages.push_back(
                ReadString(in.candidate_passages[i]));
          return value;
        });
    binding.services = {{"cross_rerank", kMockServiceCrossRerank}};
    registry.RegisterBinding(std::move(binding));
  }

  // 17. rerank_out -> CompanyOperatorRerankOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOperatorRerankOutput>(
        "rerank_out", {}, [](CompanyOperatorRerankOutput& out) noexcept {
          out.service_type = 0;
          out.count = 0;
          out.status_code = 0;
          for (int i = 0; i < COMPANY_OPERATOR_MAX_RERANK_CANDIDATES; ++i) {
            out.scores[i] = 0.0f;
            out.sorted_indices[i] = -1;
          }
        });
    SetServiceTypeMember(&binding, &CompanyOperatorRerankOutput::service_type);
    SetOutputValue<CompanyOperatorRerankOutput, RerankOutputValue>(
        &binding,
        [](CompanyOperatorRerankOutput& out, const RerankOutputValue& value,
           const ResolvedOutputPoolSpec&) {
          out.status_code = value.status_code;
          if (value.items.size() > COMPANY_OPERATOR_MAX_RERANK_CANDIDATES)
            return AdapterStatus::BufferTooSmall(
                "Ranked result exceeds platform capacity", "items");
          out.count = static_cast<int32_t>(value.items.size());
          for (size_t i = 0; i < value.items.size(); ++i) {
            if (value.items[i].original_index < 0 ||
                value.items[i].original_index >=
                    COMPANY_OPERATOR_MAX_RERANK_CANDIDATES)
              return AdapterStatus::InvalidInput("Invalid ranked result index",
                                                 "items");
            out.scores[i] = value.items[i].score;
            out.sorted_indices[i] = value.items[i].original_index;
          }
          return AdapterStatus::Ok();
        });
    binding.services = {{"cross_rerank", kMockServiceCrossRerank}};
    registry.RegisterBinding(std::move(binding));
  }
}

namespace {
const bool kMockBindingsRegistered = [] {
  RegisterMockOperatorBindings(OperatorValueTypeRegistry::Instance());
  return true;
}();
}  // namespace

}  // namespace llm_edgeflow
