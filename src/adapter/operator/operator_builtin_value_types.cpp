#include "adapter/io_converter.h"
#include "adapter/operator/operator_value_type_registry.h"

namespace llm_edgeflow {

void OperatorValueTypeRegistry::RegisterBuiltinBindings() {
  // 1. string -> CompanyString
  {
    auto binding = MakeTypedInputBinding<CompanyString>(
        "string",
        [](const CompanyString& in, const InputLimits& limits,
           std::string* err) -> int {
          return ValidateCompanyString(&in, limits.max_text_bytes, "string",
                                       err);
        });
    RegisterBinding(std::move(binding));
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
    RegisterBinding(std::move(binding));
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
    RegisterBinding(std::move(binding));
  }

  // 4. frame -> CompanyFrame
  {
    auto binding = MakeTypedInputBinding<CompanyFrame>(
        "frame",
        [](const CompanyFrame& in, const InputLimits& limits,
           std::string* err) -> int {
          if (!in.image_uri) {
            if (err) *err = "CompanyFrame.image_uri is null";
            return -3;
          }
          int ret =
              ValidateCompanyString(in.image_uri, limits.max_image_uri_bytes,
                                    "CompanyFrame.image_uri", err);
          if (ret != 0) return ret;
          if (in.metadata) {
            ret = ValidateCompanyAnyPayload(in.metadata, limits.max_any_bytes,
                                            "CompanyFrame.metadata", err);
            if (ret != 0) return ret;
          }
          return 0;
        });
    SetRequestIdMember(&binding, &CompanyFrame::request_id);
    SetServiceTypeMember(&binding, &CompanyFrame::service_type);
    RegisterBinding(std::move(binding));
  }

  // 5. od_out -> CompanyOdOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOdOutput>(
        "od_out", {{"result_json", &CompanyOdOutput::result_json, {65536}}},
        [](CompanyOdOutput& out) noexcept {
          out.request_id = 0;
          out.service_type = 0;
          out.detected_box_count = 0;
          out.status_code = 0;
        },
        &CompanyOdOutput::metadata, 65536);
    SetRequestIdMember(&binding, &CompanyOdOutput::request_id);
    SetServiceTypeMember(&binding, &CompanyOdOutput::service_type);
    RegisterBinding(std::move(binding));
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
    SetRequestIdMember(&binding, &CompanyOperatorKeywordInput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorKeywordInput::service_type);
    RegisterBinding(std::move(binding));
  }

  // 7. keyword_out -> CompanyOperatorKeywordOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOperatorKeywordOutput>(
        "keyword_out",
        {{"match_result_json",
          &CompanyOperatorKeywordOutput::match_result_json,
          {65536}}},
        [](CompanyOperatorKeywordOutput& out) noexcept {
          out.request_id = 0;
          out.service_type = 0;
          out.is_hit = 0;
          out.status_code = 0;
        });
    SetRequestIdMember(&binding, &CompanyOperatorKeywordOutput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorKeywordOutput::service_type);
    RegisterBinding(std::move(binding));
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
    SetRequestIdMember(&binding, &CompanyOperatorEntityInput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorEntityInput::service_type);
    RegisterBinding(std::move(binding));
  }

  // 9. entity_out -> CompanyOperatorEntityOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOperatorEntityOutput>(
        "entity_out",
        {{"entities_json",
          &CompanyOperatorEntityOutput::entities_json,
          {65536}}},
        [](CompanyOperatorEntityOutput& out) noexcept {
          out.request_id = 0;
          out.service_type = 0;
          out.status_code = 0;
        });
    SetRequestIdMember(&binding, &CompanyOperatorEntityOutput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorEntityOutput::service_type);
    RegisterBinding(std::move(binding));
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
    SetRequestIdMember(&binding, &CompanyOperatorDocInput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorDocInput::service_type);
    RegisterBinding(std::move(binding));
  }

  // 11. doc_out -> CompanyOperatorDocOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOperatorDocOutput>(
        "doc_out",
        {{"intent_name", &CompanyOperatorDocOutput::intent_name, {255}},
         {"answer_text", &CompanyOperatorDocOutput::answer_text, {65536}}},
        [](CompanyOperatorDocOutput& out) noexcept {
          out.request_id = 0;
          out.service_type = 0;
          out.confidence = 0.0f;
          out.chunk_count = 0;
          out.status_code = 0;
        });
    SetRequestIdMember(&binding, &CompanyOperatorDocOutput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorDocOutput::service_type);
    RegisterBinding(std::move(binding));
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
    SetRequestIdMember(&binding, &CompanyOperatorAuditInput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorAuditInput::service_type);
    RegisterBinding(std::move(binding));
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
          out.request_id = 0;
          out.service_type = 0;
          out.risk_score = 0.0f;
          out.status_code = 0;
        });
    SetRequestIdMember(&binding, &CompanyOperatorAuditOutput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorAuditOutput::service_type);
    RegisterBinding(std::move(binding));
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
            return -3;
          }
          if (in.pcm_length < 0 ||
              in.pcm_length > limits.max_audio_pcm_samples ||
              static_cast<size_t>(in.pcm_length) >
                  limits.max_audio_pcm_bytes / sizeof(float)) {
            if (err)
              *err = "pcm_length " + std::to_string(in.pcm_length) +
                     " invalid or exceeds limit " +
                     std::to_string(limits.max_audio_pcm_samples);
            return -3;
          }
          if (in.pcm_length > 0 && !in.pcm_buffer) {
            if (err) *err = "pcm_buffer pointer is null";
            return -3;
          }
          return 0;
        });
    SetRequestIdMember(&binding, &CompanyOperatorAudioInput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorAudioInput::service_type);
    RegisterBinding(std::move(binding));
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
          out.request_id = 0;
          out.service_type = 0;
          out.status_code = 0;
        });
    SetRequestIdMember(&binding, &CompanyOperatorAudioOutput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorAudioOutput::service_type);
    RegisterBinding(std::move(binding));
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
            return -3;
          }
          for (int i = 0; i < in.candidate_count; ++i) {
            if (!in.candidate_passages[i]) {
              if (err)
                *err = "candidate_passages[" + std::to_string(i) + "] is null";
              return -3;
            }
            ret = ValidateCompanyString(
                in.candidate_passages[i], limits.max_doc_text_bytes,
                ("candidate_passages[" + std::to_string(i) + "]").c_str(), err);
            if (ret != 0) return ret;
          }
          return 0;
        });
    SetRequestIdMember(&binding, &CompanyOperatorRerankInput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorRerankInput::service_type);
    RegisterBinding(std::move(binding));
  }

  // 17. rerank_out -> CompanyOperatorRerankOutput
  {
    auto binding = MakePooledOutputBinding<CompanyOperatorRerankOutput>(
        "rerank_out", {}, [](CompanyOperatorRerankOutput& out) noexcept {
          out.request_id = 0;
          out.service_type = 0;
          out.count = 0;
          out.status_code = 0;
          for (int i = 0; i < COMPANY_OPERATOR_MAX_RERANK_CANDIDATES; ++i) {
            out.scores[i] = 0.0f;
            out.sorted_indices[i] = -1;
          }
        });
    SetRequestIdMember(&binding, &CompanyOperatorRerankOutput::request_id);
    SetServiceTypeMember(&binding, &CompanyOperatorRerankOutput::service_type);
    RegisterBinding(std::move(binding));
  }
}

}  // namespace llm_edgeflow
