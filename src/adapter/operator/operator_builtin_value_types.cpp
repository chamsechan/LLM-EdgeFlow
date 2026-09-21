#include "adapter/operator/operator_value_type_registry.h"

namespace llm_edgeflow {

void OperatorValueTypeRegistry::RegisterBuiltinBindings() {
  // 1. string -> CompanyString
  RegisterBinding(MakeTypedInputBinding<CompanyString>(
      "string", "CompanyString",
      [](const CompanyString& in, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        return ValidateCompanyString(&in, limits.max_text_bytes, "string", err);
      }));

  // 2. buffer -> CompanyBuffer
  RegisterBinding(MakeTypedInputBinding<CompanyBuffer>(
      "buffer", "CompanyBuffer",
      [](const CompanyBuffer& in, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        return ValidateCompanyBuffer(&in, limits.max_buffer_bytes, "buffer",
                                     err);
      }));

  // 3. any -> CompanyAny
  RegisterBinding(MakeTypedInputBinding<CompanyAny>(
      "any", "CompanyAny",
      [](const CompanyAny& in, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        return ValidateCompanyAnyPayload(&in, limits.max_any_bytes, "any", err);
      }));

  // 4. frame -> CompanyFrame
  RegisterBinding(MakeTypedInputBinding<CompanyFrame>(
      "frame", "CompanyFrame",
      [](const CompanyFrame& in, const ResolvedInputLimits& limits,
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
      }));

  // 5. od_out -> CompanyOdOutput
  RegisterBinding(MakePooledOutputBinding<CompanyOdOutput>(
      "od_out", "CompanyOdOutput",
      {{"result_json", &CompanyOdOutput::result_json, {2047, 65536}}},
      [](CompanyOdOutput& out) noexcept {
        out.request_id = 0;
        out.detected_box_count = 0;
        out.status_code = 0;
      },
      &CompanyOdOutput::metadata, 65536));

  // 6. keyword_in -> CompanyOperatorKeywordInput
  RegisterBinding(MakeTypedInputBinding<CompanyOperatorKeywordInput>(
      "keyword_in", "CompanyOperatorKeywordInput",
      [](const CompanyOperatorKeywordInput& in,
         const ResolvedInputLimits& limits, std::string* err) -> int {
        return ValidateCompanyString(in.sentence_text, limits.max_text_bytes,
                                     "sentence_text", err);
      }));

  // 7. keyword_out -> CompanyOperatorKeywordOutput
  RegisterBinding(MakePooledOutputBinding<CompanyOperatorKeywordOutput>(
      "keyword_out", "CompanyOperatorKeywordOutput",
      {{"match_result_json",
        &CompanyOperatorKeywordOutput::match_result_json,
        {2047, 65536}}},
      [](CompanyOperatorKeywordOutput& out) noexcept {
        out.request_id = 0;
        out.is_hit = 0;
        out.status_code = 0;
      }));

  // 8. entity_in -> CompanyOperatorEntityInput
  RegisterBinding(MakeTypedInputBinding<CompanyOperatorEntityInput>(
      "entity_in", "CompanyOperatorEntityInput",
      [](const CompanyOperatorEntityInput& in,
         const ResolvedInputLimits& limits, std::string* err) -> int {
        return ValidateCompanyString(in.sentence_text, limits.max_text_bytes,
                                     "sentence_text", err);
      }));

  // 9. entity_out -> CompanyOperatorEntityOutput
  RegisterBinding(MakePooledOutputBinding<CompanyOperatorEntityOutput>(
      "entity_out", "CompanyOperatorEntityOutput",
      {{"entities_json",
        &CompanyOperatorEntityOutput::entities_json,
        {2047, 65536}}},
      [](CompanyOperatorEntityOutput& out) noexcept {
        out.request_id = 0;
        out.status_code = 0;
      }));

  // 10. doc_in -> CompanyOperatorDocInput
  RegisterBinding(MakeTypedInputBinding<CompanyOperatorDocInput>(
      "doc_in", "CompanyOperatorDocInput",
      [](const CompanyOperatorDocInput& in, const ResolvedInputLimits& limits,
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
      }));

  // 11. doc_out -> CompanyOperatorDocOutput
  RegisterBinding(MakePooledOutputBinding<CompanyOperatorDocOutput>(
      "doc_out", "CompanyOperatorDocOutput",
      {{"intent_name", &CompanyOperatorDocOutput::intent_name, {63, 255}},
       {"answer_text", &CompanyOperatorDocOutput::answer_text, {1023, 65536}}},
      [](CompanyOperatorDocOutput& out) noexcept {
        out.request_id = 0;
        out.confidence = 0.0f;
        out.chunk_count = 0;
        out.status_code = 0;
      }));

  // 12. audit_in -> CompanyOperatorAuditInput
  RegisterBinding(MakeTypedInputBinding<CompanyOperatorAuditInput>(
      "audit_in", "CompanyOperatorAuditInput",
      [](const CompanyOperatorAuditInput& in, const ResolvedInputLimits& limits,
         std::string* err) -> int {
        int ret = ValidateCompanyString(in.user_text, limits.max_text_bytes,
                                        "user_text", err);
        if (ret != 0) return ret;
        if (in.channel_name) {
          ret = ValidateCompanyString(in.channel_name,
                                      biz_input::kMaxChannelNameBytes,
                                      "channel_name", err);
          if (ret != 0) return ret;
        }
        return 0;
      }));

  // 13. audit_out -> CompanyOperatorAuditOutput
  RegisterBinding(MakePooledOutputBinding<CompanyOperatorAuditOutput>(
      "audit_out", "CompanyOperatorAuditOutput",
      {{"risk_level", &CompanyOperatorAuditOutput::risk_level, {31, 255}},
       {"matched_policy_clause",
        &CompanyOperatorAuditOutput::matched_policy_clause,
        {255, 4096}},
       {"audit_verdict_json",
        &CompanyOperatorAuditOutput::audit_verdict_json,
        {1023, 65536}}},
      [](CompanyOperatorAuditOutput& out) noexcept {
        out.request_id = 0;
        out.risk_score = 0.0f;
        out.status_code = 0;
      }));

  // 14. audio_in -> CompanyOperatorAudioInput
  RegisterBinding(MakeTypedInputBinding<CompanyOperatorAudioInput>(
      "audio_in", "CompanyOperatorAudioInput",
      [](const CompanyOperatorAudioInput& in, const ResolvedInputLimits& limits,
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
        if (in.pcm_length < 0 || in.pcm_length > limits.max_audio_pcm_samples ||
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
      }));

  // 15. audio_out -> CompanyOperatorAudioOutput
  RegisterBinding(MakePooledOutputBinding<CompanyOperatorAudioOutput>(
      "audio_out", "CompanyOperatorAudioOutput",
      {{"transcribed_text",
        &CompanyOperatorAudioOutput::transcribed_text,
        {511, 16384}},
       {"intent_slot_json",
        &CompanyOperatorAudioOutput::intent_slot_json,
        {1023, 65536}}},
      [](CompanyOperatorAudioOutput& out) noexcept {
        out.request_id = 0;
        out.status_code = 0;
      }));

  // 16. rerank_in -> CompanyOperatorRerankInput
  RegisterBinding(MakeTypedInputBinding<CompanyOperatorRerankInput>(
      "rerank_in", "CompanyOperatorRerankInput",
      [](const CompanyOperatorRerankInput& in,
         const ResolvedInputLimits& limits, std::string* err) -> int {
        int ret = ValidateCompanyString(in.query_text, limits.max_text_bytes,
                                        "query_text", err);
        if (ret != 0) return ret;
        if (in.candidate_count <= 0 ||
            in.candidate_count > COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) {
          if (err) {
            *err = "candidate_count " + std::to_string(in.candidate_count) +
                   " out of valid range [1, " +
                   std::to_string(COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) + "]";
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
      }));

  // 17. rerank_out -> CompanyOperatorRerankOutput
  RegisterBinding(MakePooledOutputBinding<CompanyOperatorRerankOutput>(
      "rerank_out", "CompanyOperatorRerankOutput", {},
      [](CompanyOperatorRerankOutput& out) noexcept {
        out.request_id = 0;
        out.count = 0;
        out.status_code = 0;
        for (int i = 0; i < COMPANY_OPERATOR_MAX_RERANK_CANDIDATES; ++i) {
          out.scores[i] = 0.0f;
          out.sorted_indices[i] = -1;
        }
      }));
}

}  // namespace llm_edgeflow
