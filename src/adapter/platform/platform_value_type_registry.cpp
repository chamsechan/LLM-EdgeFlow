#include "adapter/platform/platform_value_type_registry.h"

#include <cstring>
#include <iostream>
#include <limits>

namespace alg_framework {

namespace {

CompanyString* AllocateNestedCompanyString(
    uint32_t capacity, std::vector<void*>* nested_buffers,
    std::vector<CompanyString*>* nested_strings) {
  auto* cs = new CompanyString();
  cs->length = 0;
  cs->data = new char[capacity + 1];
  cs->data[0] = '\0';
  if (nested_buffers) {
    nested_buffers->push_back(cs->data);
  }
  if (nested_strings) {
    nested_strings->push_back(cs);
  }
  return cs;
}

void ResetNestedCompanyString(CompanyString* cs) noexcept {
  if (cs) {
    cs->length = 0;
    if (cs->data) {
      cs->data[0] = '\0';
    }
  }
}

}  // namespace

PlatformValueTypeRegistry& PlatformValueTypeRegistry::Instance() {
  static PlatformValueTypeRegistry instance;
  return instance;
}

bool PlatformValueTypeRegistry::ParseKey(const std::string& key,
                                         std::string* out_namespace,
                                         std::string* out_suffix) noexcept {
  if (key.empty()) return false;
  size_t last_dot = key.rfind('.');
  if (last_dot == std::string::npos || last_dot == 0 ||
      last_dot == key.size() - 1) {
    return false;
  }
  if (out_namespace) {
    *out_namespace = key.substr(0, last_dot);
  }
  if (out_suffix) {
    *out_suffix = key.substr(last_dot + 1);
  }
  return true;
}

int PlatformValueTypeRegistry::ValidateCompanyString(
    const CompanyString* str, size_t max_bytes, const char* field_name,
    std::string* err) noexcept {
  if (!str) {
    if (err) *err = std::string(field_name) + " pointer is null";
    return -3;
  }
  if (str->length < 0) {
    if (err)
      *err = std::string(field_name) + " has negative length " +
             std::to_string(str->length);
    return -3;
  }
  if (static_cast<size_t>(str->length) > max_bytes) {
    if (err)
      *err = std::string(field_name) + " length " +
             std::to_string(str->length) + " exceeds max limit " +
             std::to_string(max_bytes);
    return -3;
  }
  if (str->length > 0) {
    if (!str->data) {
      if (err)
        *err = std::string(field_name) + " length is " +
               std::to_string(str->length) + " but data pointer is null";
      return -3;
    }
    for (int32_t i = 0; i < str->length; ++i) {
      if (str->data[i] == '\0') {
        if (err)
          *err = std::string(field_name) +
                 " contains forbidden embedded NUL at byte offset " +
                 std::to_string(i);
        return -3;
      }
    }
  }
  return 0;
}

PlatformValueTypeRegistry::PlatformValueTypeRegistry() {
  RegisterBuiltinBindings();
}

bool PlatformValueTypeRegistry::RegisterBinding(
    PlatformValueTypeBinding binding) {
  if (binding.canonical_suffix.empty()) {
    has_conflict_ = true;
    return false;
  }
  if (bindings_by_canonical_.find(binding.canonical_suffix) !=
      bindings_by_canonical_.end()) {
    has_conflict_ = true;
    return false;
  }
  for (const auto& a : binding.aliases) {
    if (a.empty() || a == binding.canonical_suffix ||
        alias_to_canonical_.find(a) != alias_to_canonical_.end() ||
        bindings_by_canonical_.find(a) != bindings_by_canonical_.end()) {
      has_conflict_ = true;
      return false;
    }
    alias_to_canonical_[a] = binding.canonical_suffix;
  }
  bindings_by_canonical_[binding.canonical_suffix] = std::move(binding);
  return true;
}

const PlatformValueTypeBinding* PlatformValueTypeRegistry::GetBindingBySuffix(
    const std::string& suffix) const {
  auto it = bindings_by_canonical_.find(suffix);
  if (it != bindings_by_canonical_.end()) {
    return &it->second;
  }
  auto alias_it = alias_to_canonical_.find(suffix);
  if (alias_it != alias_to_canonical_.end()) {
    auto can_it = bindings_by_canonical_.find(alias_it->second);
    if (can_it != bindings_by_canonical_.end()) {
      return &can_it->second;
    }
  }
  return nullptr;
}

std::string PlatformValueTypeRegistry::NormalizeSuffix(
    const std::string& suffix) const {
  auto alias_it = alias_to_canonical_.find(suffix);
  if (alias_it != alias_to_canonical_.end()) {
    return alias_it->second;
  }
  return suffix;
}

int PlatformValueTypeRegistry::GlobalInit() {
  if (has_conflict_) {
    return -6;
  }
  audited_ = true;
  return 0;
}

void PlatformValueTypeRegistry::RegisterBuiltinBindings() {
  // 1. string -> CompanyString
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "string";
    b.external_c_type_name = "CompanyString";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      const auto* cs = static_cast<const CompanyString*>(ptr);
      return ValidateCompanyString(cs, limits.max_text_bytes, "string", err);
    };
    RegisterBinding(b);
  }

  // 2. buffer -> CompanyBuffer
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "buffer";
    b.external_c_type_name = "CompanyBuffer";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyBuffer pointer is null";
        return -3;
      }
      const auto* buf = static_cast<const CompanyBuffer*>(ptr);
      if (buf->length < 0) {
        if (err) *err = "CompanyBuffer has negative length";
        return -3;
      }
      if (static_cast<size_t>(buf->length) > limits.max_buffer_bytes) {
        if (err) *err = "CompanyBuffer length exceeds limit";
        return -3;
      }
      if (buf->length > 0 && !buf->data) {
        if (err) *err = "CompanyBuffer has non-zero length but null data";
        return -3;
      }
      return 0;
    };
    RegisterBinding(b);
  }

  // 3. any -> CompanyAny
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "any";
    b.external_c_type_name = "CompanyAny";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyAny pointer is null";
        return -3;
      }
      const auto* any = static_cast<const CompanyAny*>(ptr);
      if (any->element_count < 0 || any->byte_length < 0) {
        if (err) *err = "CompanyAny has negative count or length";
        return -3;
      }
      if (static_cast<size_t>(any->byte_length) > limits.max_any_bytes) {
        if (err) *err = "CompanyAny byte_length exceeds limit";
        return -3;
      }
      if (any->byte_length > 0 && !any->data) {
        if (err) *err = "CompanyAny has non-zero byte_length but null data";
        return -3;
      }
      return 0;
    };
    RegisterBinding(b);
  }

  // 4. frame -> CompanyFrame
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "frame";
    b.aliases = {"image_in"};
    b.external_c_type_name = "CompanyFrame";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyFrame pointer is null";
        return -3;
      }
      const auto* frame = static_cast<const CompanyFrame*>(ptr);
      if (!frame->image_uri) {
        if (err) *err = "CompanyFrame.image_uri pointer is null";
        return -3;
      }
      int ret =
          ValidateCompanyString(frame->image_uri, limits.max_image_uri_bytes,
                                "CompanyFrame.image_uri", err);
      if (ret != 0) return ret;
      if (frame->image_uri->length == 0) {
        if (err) *err = "CompanyFrame.image_uri cannot be empty";
        return -3;
      }
      if (frame->metadata) {
        if (frame->metadata->element_count < 0 ||
            frame->metadata->byte_length < 0) {
          if (err) *err = "CompanyFrame.metadata has negative count or length";
          return -3;
        }
        if (static_cast<size_t>(frame->metadata->byte_length) >
            limits.max_any_bytes) {
          if (err) *err = "CompanyFrame.metadata byte_length exceeds limit";
          return -3;
        }
        if (frame->metadata->byte_length > 0 && !frame->metadata->data) {
          if (err) *err = "CompanyFrame.metadata has null data";
          return -3;
        }
      }
      return 0;
    };
    RegisterBinding(b);
  }

  // 5. od_out -> CompanyOdOutput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "od_out";
    b.aliases = {"ocr_out"};
    b.external_c_type_name = "CompanyOdOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = new CompanyOdOutput();
      raw->request_id = 0;
      raw->detected_box_count = 0;
      raw->status_code = 0;
      raw->result_json = AllocateNestedCompanyString(
          spec.GetCapacity("result_json", 2047),
          &out_block->owned_nested_buffers, &out_block->nested_company_strings);
      if (spec.meta_num > 0) {
        auto* meta = new CompanyAny();
        meta->type_id = spec.metadata_type_id;
        meta->element_count = static_cast<int32_t>(spec.meta_num);
        meta->byte_length = static_cast<int32_t>(spec.meta_num * sizeof(float));
        meta->data = new uint8_t[meta->byte_length];
        out_block->owned_nested_buffers.push_back(meta->data);
        raw->metadata = meta;
      } else {
        raw->metadata = nullptr;
      }
      out_block->raw_struct = raw;
      out_block->spec = spec;
      return 0;
    };
    b.reset_external = [](void* ptr,
                          const ResolvedOutputPoolSpec& /*spec*/) noexcept {
      if (!ptr) return;
      auto* raw = static_cast<CompanyOdOutput*>(ptr);
      raw->request_id = 0;
      raw->detected_box_count = 0;
      raw->status_code = 0;
      ResetNestedCompanyString(raw->result_json);
      if (raw->metadata) {
        auto* meta = const_cast<CompanyAny*>(raw->metadata);
        if (meta->data && meta->byte_length > 0) {
          std::memset(meta->data, 0, meta->byte_length);
        }
      }
    };
    b.destroy_external = [](OwnedExternalBlock* block) noexcept {
      if (!block) return;
      for (void* buf : block->owned_nested_buffers) {
        delete[] static_cast<char*>(buf);
      }
      block->owned_nested_buffers.clear();
      for (CompanyString* cs : block->nested_company_strings) {
        delete cs;
      }
      block->nested_company_strings.clear();
      auto* raw = static_cast<CompanyOdOutput*>(block->raw_struct);
      if (raw) {
        if (raw->metadata) {
          delete const_cast<CompanyAny*>(raw->metadata);
          raw->metadata = nullptr;
        }
        delete raw;
      }
      block->raw_struct = nullptr;
    };
    RegisterBinding(b);
  }

  // 6. keyword_in -> CompanyPlatformKeywordInput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "keyword_in";
    b.aliases = {"sentence_in"};
    b.external_c_type_name = "CompanyPlatformKeywordInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyPlatformKeywordInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyPlatformKeywordInput*>(ptr);
      if (!in->sentence_text) {
        if (err) *err = "sentence_text pointer is null";
        return -3;
      }
      return ValidateCompanyString(in->sentence_text, limits.max_text_bytes,
                                   "sentence_text", err);
    };
    RegisterBinding(b);
  }

  // 7. keyword_out -> CompanyPlatformKeywordOutput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "keyword_out";
    b.aliases = {"match_out"};
    b.external_c_type_name = "CompanyPlatformKeywordOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = new CompanyPlatformKeywordOutput();
      raw->request_id = 0;
      raw->is_hit = 0;
      raw->status_code = 0;
      raw->match_result_json = AllocateNestedCompanyString(
          spec.GetCapacity("match_result_json", 2047),
          &out_block->owned_nested_buffers, &out_block->nested_company_strings);
      out_block->raw_struct = raw;
      out_block->spec = spec;
      return 0;
    };
    b.reset_external = [](void* ptr,
                          const ResolvedOutputPoolSpec& /*spec*/) noexcept {
      if (!ptr) return;
      auto* raw = static_cast<CompanyPlatformKeywordOutput*>(ptr);
      raw->request_id = 0;
      raw->is_hit = 0;
      raw->status_code = 0;
      ResetNestedCompanyString(raw->match_result_json);
    };
    b.destroy_external = [](OwnedExternalBlock* block) noexcept {
      if (!block) return;
      for (void* buf : block->owned_nested_buffers) {
        delete[] static_cast<char*>(buf);
      }
      block->owned_nested_buffers.clear();
      for (CompanyString* cs : block->nested_company_strings) {
        delete cs;
      }
      block->nested_company_strings.clear();
      delete static_cast<CompanyPlatformKeywordOutput*>(block->raw_struct);
      block->raw_struct = nullptr;
    };
    RegisterBinding(b);
  }

  // 8. entity_in -> CompanyPlatformEntityInput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "entity_in";
    b.aliases = {"text_in"};
    b.external_c_type_name = "CompanyPlatformEntityInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyPlatformEntityInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyPlatformEntityInput*>(ptr);
      if (!in->sentence_text) {
        if (err) *err = "sentence_text pointer is null";
        return -3;
      }
      return ValidateCompanyString(in->sentence_text, limits.max_text_bytes,
                                   "sentence_text", err);
    };
    RegisterBinding(b);
  }

  // 9. entity_out -> CompanyPlatformEntityOutput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "entity_out";
    b.aliases = {"extracted_out"};
    b.external_c_type_name = "CompanyPlatformEntityOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = new CompanyPlatformEntityOutput();
      raw->request_id = 0;
      raw->status_code = 0;
      raw->entities_json = AllocateNestedCompanyString(
          spec.GetCapacity("entities_json", 2047),
          &out_block->owned_nested_buffers, &out_block->nested_company_strings);
      out_block->raw_struct = raw;
      out_block->spec = spec;
      return 0;
    };
    b.reset_external = [](void* ptr,
                          const ResolvedOutputPoolSpec& /*spec*/) noexcept {
      if (!ptr) return;
      auto* raw = static_cast<CompanyPlatformEntityOutput*>(ptr);
      raw->request_id = 0;
      raw->status_code = 0;
      ResetNestedCompanyString(raw->entities_json);
    };
    b.destroy_external = [](OwnedExternalBlock* block) noexcept {
      if (!block) return;
      for (void* buf : block->owned_nested_buffers) {
        delete[] static_cast<char*>(buf);
      }
      block->owned_nested_buffers.clear();
      for (CompanyString* cs : block->nested_company_strings) {
        delete cs;
      }
      block->nested_company_strings.clear();
      delete static_cast<CompanyPlatformEntityOutput*>(block->raw_struct);
      block->raw_struct = nullptr;
    };
    RegisterBinding(b);
  }

  // 10. doc_in -> CompanyPlatformDocInput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "doc_in";
    b.aliases = {"qa_in"};
    b.external_c_type_name = "CompanyPlatformDocInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyPlatformDocInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyPlatformDocInput*>(ptr);
      if (!in->query_text) {
        if (err) *err = "query_text pointer is null";
        return -3;
      }
      int ret = ValidateCompanyString(in->query_text, limits.max_text_bytes,
                                      "query_text", err);
      if (ret != 0) return ret;
      if (in->doc_text) {
        ret = ValidateCompanyString(in->doc_text, limits.max_doc_text_bytes,
                                    "doc_text", err);
        if (ret != 0) return ret;
      }
      return 0;
    };
    RegisterBinding(b);
  }

  // 11. doc_out -> CompanyPlatformDocOutput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "doc_out";
    b.aliases = {"qa_out"};
    b.external_c_type_name = "CompanyPlatformDocOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = new CompanyPlatformDocOutput();
      raw->request_id = 0;
      raw->confidence = 0.0f;
      raw->chunk_count = 0;
      raw->status_code = 0;
      raw->intent_name = AllocateNestedCompanyString(
          spec.GetCapacity("intent_name", 63), &out_block->owned_nested_buffers,
          &out_block->nested_company_strings);
      raw->answer_text = AllocateNestedCompanyString(
          spec.GetCapacity("answer_text", 1023),
          &out_block->owned_nested_buffers, &out_block->nested_company_strings);
      out_block->raw_struct = raw;
      out_block->spec = spec;
      return 0;
    };
    b.reset_external = [](void* ptr,
                          const ResolvedOutputPoolSpec& /*spec*/) noexcept {
      if (!ptr) return;
      auto* raw = static_cast<CompanyPlatformDocOutput*>(ptr);
      raw->request_id = 0;
      raw->confidence = 0.0f;
      raw->chunk_count = 0;
      raw->status_code = 0;
      ResetNestedCompanyString(raw->intent_name);
      ResetNestedCompanyString(raw->answer_text);
    };
    b.destroy_external = [](OwnedExternalBlock* block) noexcept {
      if (!block) return;
      for (void* buf : block->owned_nested_buffers) {
        delete[] static_cast<char*>(buf);
      }
      block->owned_nested_buffers.clear();
      for (CompanyString* cs : block->nested_company_strings) {
        delete cs;
      }
      block->nested_company_strings.clear();
      delete static_cast<CompanyPlatformDocOutput*>(block->raw_struct);
      block->raw_struct = nullptr;
    };
    RegisterBinding(b);
  }

  // 12. audit_in -> CompanyPlatformAuditInput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "audit_in";
    b.aliases = {"dialogue_in"};
    b.external_c_type_name = "CompanyPlatformAuditInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyPlatformAuditInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyPlatformAuditInput*>(ptr);
      if (!in->user_text) {
        if (err) *err = "user_text pointer is null";
        return -3;
      }
      int ret = ValidateCompanyString(in->user_text, limits.max_text_bytes,
                                      "user_text", err);
      if (ret != 0) return ret;
      if (in->channel_name) {
        ret = ValidateCompanyString(in->channel_name, limits.max_text_bytes,
                                    "channel_name", err);
        if (ret != 0) return ret;
      }
      return 0;
    };
    RegisterBinding(b);
  }

  // 13. audit_out -> CompanyPlatformAuditOutput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "audit_out";
    b.aliases = {"verdict_out"};
    b.external_c_type_name = "CompanyPlatformAuditOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = new CompanyPlatformAuditOutput();
      raw->request_id = 0;
      raw->risk_score = 0.0f;
      raw->status_code = 0;
      raw->risk_level = AllocateNestedCompanyString(
          spec.GetCapacity("risk_level", 31), &out_block->owned_nested_buffers,
          &out_block->nested_company_strings);
      raw->matched_policy_clause = AllocateNestedCompanyString(
          spec.GetCapacity("matched_policy_clause", 255),
          &out_block->owned_nested_buffers, &out_block->nested_company_strings);
      raw->audit_verdict_json = AllocateNestedCompanyString(
          spec.GetCapacity("audit_verdict_json", 1023),
          &out_block->owned_nested_buffers, &out_block->nested_company_strings);
      out_block->raw_struct = raw;
      out_block->spec = spec;
      return 0;
    };
    b.reset_external = [](void* ptr,
                          const ResolvedOutputPoolSpec& /*spec*/) noexcept {
      if (!ptr) return;
      auto* raw = static_cast<CompanyPlatformAuditOutput*>(ptr);
      raw->request_id = 0;
      raw->risk_score = 0.0f;
      raw->status_code = 0;
      ResetNestedCompanyString(raw->risk_level);
      ResetNestedCompanyString(raw->matched_policy_clause);
      ResetNestedCompanyString(raw->audit_verdict_json);
    };
    b.destroy_external = [](OwnedExternalBlock* block) noexcept {
      if (!block) return;
      for (void* buf : block->owned_nested_buffers) {
        delete[] static_cast<char*>(buf);
      }
      block->owned_nested_buffers.clear();
      for (CompanyString* cs : block->nested_company_strings) {
        delete cs;
      }
      block->nested_company_strings.clear();
      delete static_cast<CompanyPlatformAuditOutput*>(block->raw_struct);
      block->raw_struct = nullptr;
    };
    RegisterBinding(b);
  }

  // 14. audio_in -> CompanyPlatformAudioInput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "audio_in";
    b.aliases = {"pcm_stream"};
    b.external_c_type_name = "CompanyPlatformAudioInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyPlatformAudioInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyPlatformAudioInput*>(ptr);
      if (in->pcm_length < 0) {
        if (err) *err = "pcm_length is negative";
        return -3;
      }
      if (in->pcm_length > limits.max_audio_pcm_samples) {
        if (err) *err = "pcm_length exceeds max audio samples";
        return -3;
      }
      if (in->pcm_length > 0 && !in->pcm_buffer) {
        if (err) *err = "pcm_length > 0 but pcm_buffer is null";
        return -3;
      }
      if (in->sample_rate < limits.min_sample_rate ||
          in->sample_rate > limits.max_sample_rate) {
        if (err)
          *err = "sample_rate " + std::to_string(in->sample_rate) +
                 " out of bounds [" + std::to_string(limits.min_sample_rate) +
                 ".." + std::to_string(limits.max_sample_rate) + "]";
        return -3;
      }
      return 0;
    };
    RegisterBinding(b);
  }

  // 15. audio_out -> CompanyPlatformAudioOutput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "audio_out";
    b.aliases = {"asr_out"};
    b.external_c_type_name = "CompanyPlatformAudioOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = new CompanyPlatformAudioOutput();
      raw->request_id = 0;
      raw->status_code = 0;
      raw->transcribed_text = AllocateNestedCompanyString(
          spec.GetCapacity("transcribed_text", 511),
          &out_block->owned_nested_buffers, &out_block->nested_company_strings);
      raw->intent_slot_json = AllocateNestedCompanyString(
          spec.GetCapacity("intent_slot_json", 1023),
          &out_block->owned_nested_buffers, &out_block->nested_company_strings);
      out_block->raw_struct = raw;
      out_block->spec = spec;
      return 0;
    };
    b.reset_external = [](void* ptr,
                          const ResolvedOutputPoolSpec& /*spec*/) noexcept {
      if (!ptr) return;
      auto* raw = static_cast<CompanyPlatformAudioOutput*>(ptr);
      raw->request_id = 0;
      raw->status_code = 0;
      ResetNestedCompanyString(raw->transcribed_text);
      ResetNestedCompanyString(raw->intent_slot_json);
    };
    b.destroy_external = [](OwnedExternalBlock* block) noexcept {
      if (!block) return;
      for (void* buf : block->owned_nested_buffers) {
        delete[] static_cast<char*>(buf);
      }
      block->owned_nested_buffers.clear();
      for (CompanyString* cs : block->nested_company_strings) {
        delete cs;
      }
      block->nested_company_strings.clear();
      delete static_cast<CompanyPlatformAudioOutput*>(block->raw_struct);
      block->raw_struct = nullptr;
    };
    RegisterBinding(b);
  }

  // 16. rerank_in -> CompanyPlatformRerankInput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "rerank_in";
    b.aliases = {"pair_in"};
    b.external_c_type_name = "CompanyPlatformRerankInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyPlatformRerankInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyPlatformRerankInput*>(ptr);
      if (!in->query_text) {
        if (err) *err = "query_text pointer is null";
        return -3;
      }
      int ret = ValidateCompanyString(in->query_text, limits.max_text_bytes,
                                      "query_text", err);
      if (ret != 0) return ret;
      if (in->candidate_count < 1 ||
          in->candidate_count > limits.max_rerank_candidates) {
        if (err)
          *err = "candidate_count " + std::to_string(in->candidate_count) +
                 " out of range [1.." +
                 std::to_string(limits.max_rerank_candidates) + "]";
        return -3;
      }
      for (int32_t i = 0; i < in->candidate_count; ++i) {
        if (!in->candidate_passages[i]) {
          if (err)
            *err = "candidate_passages[" + std::to_string(i) + "] is null";
          return -3;
        }
        ret = ValidateCompanyString(
            in->candidate_passages[i], limits.max_text_bytes,
            ("candidate_passages[" + std::to_string(i) + "]").c_str(), err);
        if (ret != 0) return ret;
      }
      return 0;
    };
    RegisterBinding(b);
  }

  // 17. rerank_out -> CompanyPlatformRerankOutput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "rerank_out";
    b.aliases = {"scores_out"};
    b.external_c_type_name = "CompanyPlatformRerankOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = new CompanyPlatformRerankOutput();
      raw->request_id = 0;
      raw->count = 0;
      raw->status_code = 0;
      std::memset(raw->scores, 0, sizeof(raw->scores));
      std::memset(raw->sorted_indices, 0, sizeof(raw->sorted_indices));
      out_block->raw_struct = raw;
      out_block->spec = spec;
      return 0;
    };
    b.reset_external = [](void* ptr,
                          const ResolvedOutputPoolSpec& /*spec*/) noexcept {
      if (!ptr) return;
      auto* raw = static_cast<CompanyPlatformRerankOutput*>(ptr);
      raw->request_id = 0;
      raw->count = 0;
      raw->status_code = 0;
      std::memset(raw->scores, 0, sizeof(raw->scores));
      std::memset(raw->sorted_indices, 0, sizeof(raw->sorted_indices));
    };
    b.destroy_external = [](OwnedExternalBlock* block) noexcept {
      if (!block) return;
      delete static_cast<CompanyPlatformRerankOutput*>(block->raw_struct);
      block->raw_struct = nullptr;
    };
    RegisterBinding(b);
  }
}

}  // namespace alg_framework
