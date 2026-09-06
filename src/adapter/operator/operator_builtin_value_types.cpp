#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include "adapter/operator/operator_value_type_registry.h"

namespace llm_edgeflow {

namespace {

inline void DeleteCharArray(void* p) noexcept {
  delete[] static_cast<char*>(p);
}

inline void DeleteCompanyString(void* p) noexcept {
  delete static_cast<CompanyString*>(p);
}

inline void DeleteCompanyAny(void* p) noexcept {
  delete static_cast<CompanyAny*>(p);
}

inline void DeleteAnyPayload(void* p) noexcept {
  delete[] static_cast<uint8_t*>(p);
}

template <typename T>
inline void DeleteTypedObject(void* p) noexcept {
  delete static_cast<T*>(p);
}

void RegisterCleanup(OwnedExternalBlock* block, CleanupAction action);

template <typename T>
T* AllocateRootOutput(size_t cleanup_capacity, OwnedExternalBlock* block) {
  block->cleanups.reserve(cleanup_capacity);
  std::unique_ptr<T, decltype(&DeleteTypedObject<T>)> holder(
      new T(), DeleteTypedObject<T>);
  T* raw = holder.get();
  RegisterCleanup(block, {raw, DeleteTypedObject<T>});
  holder.release();
  return raw;
}

void DestroyExternalBlock(OwnedExternalBlock* block) noexcept {
  if (block) block->Destroy();
}

void RegisterCleanup(OwnedExternalBlock* block, CleanupAction action) {
  block->cleanups.push_back(action);
}

CompanyString* AllocateNestedCompanyString(uint32_t capacity,
                                           OwnedExternalBlock* block) {
  std::unique_ptr<CompanyString, decltype(&DeleteCompanyString)> str(
      new CompanyString(), DeleteCompanyString);
  size_t alloc_bytes = static_cast<size_t>(capacity) + 1;
  std::unique_ptr<char[], decltype(&DeleteCharArray)> data(
      new char[alloc_bytes], DeleteCharArray);
  std::memset(data.get(), 0, alloc_bytes);

  str->length = 0;
  str->data = data.get();

  char* raw_data = data.get();
  CompanyString* raw_str = str.get();
  RegisterCleanup(block, {raw_data, DeleteCharArray});
  data.release();
  RegisterCleanup(block, {raw_str, DeleteCompanyString});
  str.release();
  return raw_str;
}

CompanyAny* AllocateNestedCompanyAny(uint32_t meta_num, int32_t type_id,
                                     OwnedExternalBlock* block) {
  if (meta_num == 0 || type_id == 0) {
    return nullptr;
  }
  const auto* desc = FindCompanyAnyType(type_id);
  if (!desc || desc->element_size == 0) {
    return nullptr;
  }
  std::unique_ptr<CompanyAny, decltype(&DeleteCompanyAny)> any(
      new CompanyAny(), DeleteCompanyAny);
  size_t total_bytes = 0;
  if (!CheckedMultiply(meta_num, desc->element_size, &total_bytes)) {
    return nullptr;
  }
  std::unique_ptr<uint8_t[], decltype(&DeleteAnyPayload)> data(
      new uint8_t[total_bytes], DeleteAnyPayload);
  std::memset(data.get(), 0, total_bytes);

  any->type_id = type_id;
  any->element_count = 0;
  any->byte_length = 0;
  any->data = data.get();

  uint8_t* raw_data = data.get();
  CompanyAny* raw_any = any.get();
  RegisterCleanup(block, {raw_data, DeleteAnyPayload});
  data.release();
  RegisterCleanup(block, {raw_any, DeleteCompanyAny});
  any.release();
  return raw_any;
}

void ResetNestedCompanyString(CompanyString* str) noexcept {
  if (str) {
    str->length = 0;
    if (str->data) {
      str->data[0] = '\0';
    }
  }
}

void ResetNestedCompanyAny(CompanyAny* any) noexcept {
  if (any) {
    any->element_count = 0;
    any->byte_length = 0;
  }
}

// Keep the external null diagnostic and type erasure at the binding boundary.
template <typename T, typename Validate>
OperatorValueTypeBinding MakeTypedInputBinding(const char* suffix,
                                               const char* type_name,
                                               Validate validate) {
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = suffix;
  binding.external_c_type_name = type_name;
  binding.direction = IoDirection::kInput;
  binding.validate_external =
      [type_name, validate](const void* ptr, const ResolvedInputLimits& limits,
                            std::string* err) -> int {
    if (!ptr) {
      if (err) *err = std::string(type_name) + " pointer is null";
      return -3;
    }
    return validate(*static_cast<const T*>(ptr), limits, err);
  };
  return binding;
}

bool ComputeStandardOutputBlockPayloadBytes(size_t root_struct_bytes,
                                            const ResolvedOutputPoolSpec& spec,
                                            size_t* out_bytes,
                                            std::string* err) noexcept {
  if (!out_bytes) {
    if (err) *err = "Null output block payload pointer";
    return false;
  }
  *out_bytes = 0;

  size_t block_bytes = root_struct_bytes;
  for (const auto& [field, capacity] : spec.capacities) {
    size_t string_bytes = 0;
    if (!CheckedAdd(static_cast<size_t>(capacity), 1, &string_bytes) ||
        !CheckedAdd(string_bytes, sizeof(CompanyString), &string_bytes) ||
        !CheckedAdd(block_bytes, string_bytes, &block_bytes)) {
      if (err) {
        *err = spec.type + " capacity calculation overflowed for field '" +
               field + "'";
      }
      return false;
    }
  }

  if (spec.meta_num > 0) {
    const auto* metadata_desc = FindCompanyAnyType(spec.metadata_type_id);
    if (!metadata_desc || metadata_desc->element_size == 0) {
      if (err) *err = "Metadata type is not registered";
      return false;
    }
    size_t metadata_payload = 0;
    size_t metadata_bytes = 0;
    if (!CheckedMultiply(spec.meta_num, metadata_desc->element_size,
                         &metadata_payload) ||
        !CheckedAdd(metadata_payload, sizeof(CompanyAny), &metadata_bytes) ||
        !CheckedAdd(block_bytes, metadata_bytes, &block_bytes)) {
      if (err) *err = spec.type + " metadata calculation overflowed";
      return false;
    }
  }

  *out_bytes = block_bytes;
  return true;
}

// Each pooled string is declared once for capacity validation, allocation and
// reset. Member pointers keep the descriptor tied to its concrete C structure.
template <typename T>
struct OutputStringField {
  const char* name;
  CompanyString* T::*member;
  OutputCapacityFieldConfig capacity;
};

template <typename T, typename ResetScalars>
OperatorValueTypeBinding MakePooledOutputBinding(
    const char* suffix, const char* type_name,
    std::vector<OutputStringField<T>> string_fields, ResetScalars reset_scalars,
    CompanyAny* T::*metadata_field = nullptr,
    uint32_t max_metadata_elements = 0) {
  static_assert(std::is_nothrow_invocable_v<ResetScalars, T&>);
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = suffix;
  binding.external_c_type_name = type_name;
  binding.direction = IoDirection::kOutput;
  for (const auto& field : string_fields) {
    binding.output_layout.string_capacity_fields.emplace(field.name,
                                                         field.capacity);
  }
  binding.output_layout.max_metadata_elements = max_metadata_elements;
  binding.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec& spec, size_t* out_bytes,
         std::string* err) noexcept {
        return ComputeStandardOutputBlockPayloadBytes(sizeof(T), spec,
                                                      out_bytes, err);
      };
  binding.allocate_external = [string_fields, metadata_field, reset_scalars](
                                  const ResolvedOutputPoolSpec& spec,
                                  OwnedExternalBlock* block,
                                  std::string*) -> int {
    // Reserve before allocating: one root plus a wrapper and data buffer
    // for each nested field. OwnedExternalBlock rolls back partial failure.
    auto* raw = AllocateRootOutput<T>(
        1 + 2 * string_fields.size() + (metadata_field ? 2 : 0), block);
    reset_scalars(*raw);
    for (const auto& field : string_fields) {
      raw->*field.member =
          AllocateNestedCompanyString(spec.GetCapacity(field.name), block);
    }
    if (metadata_field) {
      raw->*metadata_field =
          AllocateNestedCompanyAny(spec.meta_num, spec.metadata_type_id, block);
    }
    block->raw_struct = raw;
    return 0;
  };
  binding.reset_external = [string_fields, metadata_field, reset_scalars](
                               void* ptr,
                               const ResolvedOutputPoolSpec&) noexcept {
    if (!ptr) return;
    auto* raw = static_cast<T*>(ptr);
    // Reset values only; nested storage and metadata type survive reuse.
    reset_scalars(*raw);
    for (const auto& field : string_fields) {
      ResetNestedCompanyString(raw->*field.member);
    }
    if (metadata_field) ResetNestedCompanyAny(raw->*metadata_field);
  };
  binding.destroy_external = DestroyExternalBlock;
  return binding;
}

}  // namespace

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
          ret =
              ValidateCompanyString(in.channel_name, 256, "channel_name", err);
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
        if (in.pcm_length <= 0 ||
            in.pcm_length > limits.max_audio_pcm_samples) {
          if (err)
            *err = "pcm_length " + std::to_string(in.pcm_length) +
                   " invalid or exceeds limit " +
                   std::to_string(limits.max_audio_pcm_samples);
          return -3;
        }
        if (!in.pcm_buffer) {
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
