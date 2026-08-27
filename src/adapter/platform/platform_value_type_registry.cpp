#include "adapter/platform/platform_value_type_registry.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <unordered_set>

namespace alg_framework {

namespace {

std::atomic<int> g_alloc_failure_countdown{-1};

void CheckAllocFailureProbe() {
  if (g_alloc_failure_countdown.load() >= 0) {
    if (g_alloc_failure_countdown.fetch_sub(1) == 0) {
      throw std::bad_alloc();
    }
  }
}

const CompanyAnyTypeDescriptor kBuiltinAnyTypes[] = {
    {0, 0, 1, "none"},
    {1, sizeof(float), alignof(float), "float32"},
    {2, sizeof(int32_t), alignof(int32_t), "int32"},
    {3, sizeof(uint8_t), alignof(uint8_t), "uint8"},
    {4, sizeof(int64_t), alignof(int64_t), "int64"},
    {5, sizeof(double), alignof(double), "float64"},
};

CompanyString* AllocateNestedCompanyString(uint32_t capacity,
                                           OwnedExternalBlock* out_block) {
  CheckAllocFailureProbe();
  char* data = new char[capacity + 1];
  out_block->cleanups.push_back([data]() { delete[] data; });
  data[0] = '\0';

  CheckAllocFailureProbe();
  auto* cs = new CompanyString();
  out_block->cleanups.push_back([cs]() { delete cs; });
  cs->length = 0;
  cs->data = data;
  return cs;
}

CompanyAny* AllocateNestedCompanyAny(uint32_t meta_num,
                                     int32_t metadata_type_id,
                                     OwnedExternalBlock* out_block) {
  if (meta_num == 0 || metadata_type_id == 0) {
    return nullptr;
  }
  const auto* desc = FindCompanyAnyType(metadata_type_id);
  if (!desc || desc->element_size == 0) {
    return nullptr;
  }

  size_t total_bytes = 0;
  if (!CheckedMultiply(meta_num, desc->element_size, &total_bytes)) {
    throw std::bad_alloc();
  }

  CheckAllocFailureProbe();
  uint8_t* mdata = new uint8_t[total_bytes]();
  out_block->cleanups.push_back([mdata]() { delete[] mdata; });

  CheckAllocFailureProbe();
  auto* meta = new CompanyAny();
  out_block->cleanups.push_back([meta]() { delete meta; });
  meta->type_id = metadata_type_id;
  meta->element_count = 0;
  meta->byte_length = 0;
  meta->data = mdata;
  return meta;
}

void ResetNestedCompanyString(CompanyString* cs) noexcept {
  if (cs) {
    cs->length = 0;
    if (cs->data) {
      cs->data[0] = '\0';
    }
  }
}

void ResetNestedCompanyAny(CompanyAny* any) noexcept {
  if (any) {
    any->element_count = 0;
    any->byte_length = 0;
  }
}

}  // namespace

void PlatformValueTypeRegistry::SetAllocationFailureCountdown(
    int count) noexcept {
  g_alloc_failure_countdown.store(count);
}

int PlatformValueTypeRegistry::GetAllocationFailureCountdown() noexcept {
  return g_alloc_failure_countdown.load();
}

const CompanyAnyTypeDescriptor* FindCompanyAnyType(int32_t type_id) noexcept {
  for (const auto& item : kBuiltinAnyTypes) {
    if (item.type_id == type_id) {
      return &item;
    }
  }
  return nullptr;
}

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

int PlatformValueTypeRegistry::ValidateCompanyBuffer(
    const CompanyBuffer* buf, size_t max_bytes, const char* field_name,
    std::string* err) noexcept {
  if (!buf) {
    if (err) *err = std::string(field_name) + " pointer is null";
    return -3;
  }
  if (buf->length < 0) {
    if (err)
      *err = std::string(field_name) + " has negative length " +
             std::to_string(buf->length);
    return -3;
  }
  if (static_cast<size_t>(buf->length) > max_bytes) {
    if (err)
      *err = std::string(field_name) + " length exceeds max limit " +
             std::to_string(max_bytes);
    return -3;
  }
  if (buf->length > 0 && !buf->data) {
    if (err) *err = std::string(field_name) + " data pointer is null";
    return -3;
  }
  return 0;
}

int PlatformValueTypeRegistry::ValidateCompanyAnyPayload(
    const CompanyAny* any, size_t max_any_bytes, const char* field_name,
    std::string* err) noexcept {
  if (!any) {
    if (err) *err = std::string(field_name) + " pointer is null";
    return -3;
  }
  if (any->element_count < 0 || any->byte_length < 0) {
    if (err) {
      *err = std::string(field_name) + " has negative count or length: count=" +
             std::to_string(any->element_count) +
             ", length=" + std::to_string(any->byte_length);
    }
    return -3;
  }
  if (static_cast<size_t>(any->byte_length) > max_any_bytes) {
    if (err) {
      *err = std::string(field_name) + " byte_length " +
             std::to_string(any->byte_length) + " exceeds max limit " +
             std::to_string(max_any_bytes);
    }
    return -3;
  }
  if (any->type_id == 0) {
    if (any->element_count != 0 || any->byte_length != 0) {
      if (err) {
        *err = std::string(field_name) +
               " has type_id=0 but non-zero count/length";
      }
      return -3;
    }
    return 0;
  }

  const auto* desc = FindCompanyAnyType(any->type_id);
  if (!desc) {
    if (err) {
      *err = std::string(field_name) +
             " has unknown or unwhitelisted type_id " +
             std::to_string(any->type_id);
    }
    return -3;
  }

  size_t expected_bytes = 0;
  if (!CheckedMultiply(static_cast<size_t>(any->element_count),
                       desc->element_size, &expected_bytes)) {
    if (err) {
      *err =
          std::string(field_name) + " element_count multiplication overflowed";
    }
    return -3;
  }
  if (expected_bytes != static_cast<size_t>(any->byte_length)) {
    if (err) {
      *err = std::string(field_name) + " size equation mismatch: expected " +
             std::to_string(expected_bytes) + " bytes for " +
             std::to_string(any->element_count) + " elements of type " +
             desc->debug_name + ", but byte_length is " +
             std::to_string(any->byte_length);
    }
    return -3;
  }
  if (any->byte_length > 0 && !any->data) {
    if (err) {
      *err = std::string(field_name) + " non-empty payload has null data";
    }
    return -3;
  }
  return 0;
}

int PlatformValueTypeRegistry::GlobalInit() {
  if (has_conflict_) {
    return -6;
  }
  audited_ = true;
  return 0;
}

bool PlatformValueTypeRegistry::RegisterBinding(
    PlatformValueTypeBinding binding) {
  if (audited_) {
    has_conflict_ = true;
    return false;
  }
  if (binding.canonical_suffix.empty()) {
    has_conflict_ = true;
    return false;
  }

  // 1. 检查 canonical 是否与已有 canonical 冲突
  if (bindings_by_canonical_.find(binding.canonical_suffix) !=
      bindings_by_canonical_.end()) {
    has_conflict_ = true;
    return false;
  }

  // 2. 检查 canonical 是否与已有 alias 冲突
  if (alias_to_canonical_.find(binding.canonical_suffix) !=
      alias_to_canonical_.end()) {
    has_conflict_ = true;
    return false;
  }

  // 3. 检查本次别名集内部无重复且不等于 canonical
  std::unordered_set<std::string> current_aliases;
  for (const auto& a : binding.aliases) {
    if (a.empty() || a == binding.canonical_suffix) {
      has_conflict_ = true;
      return false;
    }
    if (!current_aliases.insert(a).second) {
      has_conflict_ = true;
      return false;
    }
    // 检查 alias 是否与已有 canonical 冲突
    if (bindings_by_canonical_.find(a) != bindings_by_canonical_.end()) {
      has_conflict_ = true;
      return false;
    }
    // 检查 alias 是否与已有 alias 冲突
    if (alias_to_canonical_.find(a) != alias_to_canonical_.end()) {
      has_conflict_ = true;
      return false;
    }
  }

  // 4. 原子预检通过后，一次性提交
  for (const auto& a : binding.aliases) {
    alias_to_canonical_[a] = binding.canonical_suffix;
  }
  bindings_by_canonical_[binding.canonical_suffix] = std::move(binding);
  return true;
}

const PlatformValueTypeBinding* PlatformValueTypeRegistry::GetBindingBySuffix(
    const std::string& suffix) const {
  auto it_can = bindings_by_canonical_.find(suffix);
  if (it_can != bindings_by_canonical_.end()) {
    return &it_can->second;
  }
  auto it_alias = alias_to_canonical_.find(suffix);
  if (it_alias != alias_to_canonical_.end()) {
    auto it_target = bindings_by_canonical_.find(it_alias->second);
    if (it_target != bindings_by_canonical_.end()) {
      return &it_target->second;
    }
  }
  return nullptr;
}

PlatformValueTypeRegistry::PlatformValueTypeRegistry() {
  RegisterBuiltinBindings();
}

void PlatformValueTypeRegistry::RegisterBuiltinBindings() {
  // 1. string -> CompanyString
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "string";
    b.aliases = {"text", "str"};
    b.external_c_type_name = "CompanyString";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyString pointer is null";
        return -3;
      }
      return ValidateCompanyString(static_cast<const CompanyString*>(ptr),
                                   limits.max_text_bytes, "string", err);
    };
    RegisterBinding(b);
  }

  // 2. buffer -> CompanyBuffer
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "buffer";
    b.aliases = {"bin", "binary", "raw_buf"};
    b.external_c_type_name = "CompanyBuffer";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyBuffer pointer is null";
        return -3;
      }
      return ValidateCompanyBuffer(static_cast<const CompanyBuffer*>(ptr),
                                   limits.max_buffer_bytes, "buffer", err);
    };
    RegisterBinding(b);
  }

  // 3. any -> CompanyAny
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "any";
    b.aliases = {"generic_any", "metadata"};
    b.external_c_type_name = "CompanyAny";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyAny pointer is null";
        return -3;
      }
      return ValidateCompanyAnyPayload(static_cast<const CompanyAny*>(ptr),
                                       limits.max_any_bytes, "any", err);
    };
    RegisterBinding(b);
  }

  // 4. frame -> CompanyFrame
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "frame";
    b.aliases = {"image", "image_frame"};
    b.external_c_type_name = "CompanyFrame";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyFrame pointer is null";
        return -3;
      }
      const auto* f = static_cast<const CompanyFrame*>(ptr);
      if (!f->image_uri) {
        if (err) *err = "CompanyFrame.image_uri is null";
        return -3;
      }
      int ret = ValidateCompanyString(f->image_uri, limits.max_image_uri_bytes,
                                      "CompanyFrame.image_uri", err);
      if (ret != 0) return ret;
      if (f->metadata) {
        ret = ValidateCompanyAnyPayload(f->metadata, limits.max_any_bytes,
                                        "CompanyFrame.metadata", err);
        if (ret != 0) return ret;
      }
      return 0;
    };
    RegisterBinding(b);
  }

  // 5. od_out -> CompanyOdOutput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "od_out";
    b.aliases = {"ocr_out", "detect_out"};
    b.external_c_type_name = "CompanyOdOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      CheckAllocFailureProbe();
      auto* raw = new CompanyOdOutput();
      out_block->cleanups.push_back([raw]() { delete raw; });
      raw->request_id = 0;
      raw->detected_box_count = 0;
      raw->status_code = 0;
      raw->result_json = AllocateNestedCompanyString(
          spec.GetCapacity("result_json", 2047), out_block);
      raw->metadata = AllocateNestedCompanyAny(
          spec.meta_num, spec.metadata_type_id, out_block);
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
      ResetNestedCompanyAny(raw->metadata);
    };
    b.destroy_external = [](OwnedExternalBlock* block) noexcept {
      if (block) block->Destroy();
    };
    RegisterBinding(b);
  }

  // 6. keyword_in -> CompanyPlatformKeywordInput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "keyword_in";
    b.aliases = {"kw_in"};
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
    b.aliases = {"kw_out"};
    b.external_c_type_name = "CompanyPlatformKeywordOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      CheckAllocFailureProbe();
      auto* raw = new CompanyPlatformKeywordOutput();
      out_block->cleanups.push_back([raw]() { delete raw; });
      raw->request_id = 0;
      raw->is_hit = 0;
      raw->match_result_json = AllocateNestedCompanyString(
          spec.GetCapacity("match_result_json", 2047), out_block);
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
      ResetNestedCompanyString(raw->match_result_json);
    };
    b.destroy_external = [](OwnedExternalBlock* block) noexcept {
      if (block) block->Destroy();
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
      CheckAllocFailureProbe();
      auto* raw = new CompanyPlatformEntityOutput();
      out_block->cleanups.push_back([raw]() { delete raw; });
      raw->request_id = 0;
      raw->status_code = 0;
      raw->entities_json = AllocateNestedCompanyString(
          spec.GetCapacity("entities_json", 2047), out_block);
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
      if (block) block->Destroy();
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
      CheckAllocFailureProbe();
      auto* raw = new CompanyPlatformDocOutput();
      out_block->cleanups.push_back([raw]() { delete raw; });
      raw->request_id = 0;
      raw->confidence = 0.0f;
      raw->chunk_count = 0;
      raw->status_code = 0;
      raw->intent_name = AllocateNestedCompanyString(
          spec.GetCapacity("intent_name", 63), out_block);
      raw->answer_text = AllocateNestedCompanyString(
          spec.GetCapacity("answer_text", 1023), out_block);
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
      if (block) block->Destroy();
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
        ret = ValidateCompanyString(in->channel_name, 256, "channel_name", err);
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
    b.aliases = {"dialogue_out"};
    b.external_c_type_name = "CompanyPlatformAuditOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      CheckAllocFailureProbe();
      auto* raw = new CompanyPlatformAuditOutput();
      out_block->cleanups.push_back([raw]() { delete raw; });
      raw->request_id = 0;
      raw->risk_score = 0.0f;
      raw->status_code = 0;
      raw->risk_level = AllocateNestedCompanyString(
          spec.GetCapacity("risk_level", 31), out_block);
      raw->matched_policy_clause = AllocateNestedCompanyString(
          spec.GetCapacity("matched_policy_clause", 255), out_block);
      raw->audit_verdict_json = AllocateNestedCompanyString(
          spec.GetCapacity("audit_verdict_json", 1023), out_block);
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
      if (block) block->Destroy();
    };
    RegisterBinding(b);
  }

  // 14. audio_in -> CompanyPlatformAudioInput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "audio_in";
    b.aliases = {"asr_in"};
    b.external_c_type_name = "CompanyPlatformAudioInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyPlatformAudioInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyPlatformAudioInput*>(ptr);
      if (in->sample_rate < limits.min_sample_rate ||
          in->sample_rate > limits.max_sample_rate) {
        if (err) {
          *err = "sample_rate " + std::to_string(in->sample_rate) +
                 " out of valid range [" +
                 std::to_string(limits.min_sample_rate) + ", " +
                 std::to_string(limits.max_sample_rate) + "]";
        }
        return -3;
      }
      if (in->pcm_length <= 0 ||
          in->pcm_length > limits.max_audio_pcm_samples) {
        if (err)
          *err = "pcm_length " + std::to_string(in->pcm_length) +
                 " invalid or exceeds limit " +
                 std::to_string(limits.max_audio_pcm_samples);
        return -3;
      }
      if (!in->pcm_buffer) {
        if (err) *err = "pcm_buffer pointer is null";
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
      CheckAllocFailureProbe();
      auto* raw = new CompanyPlatformAudioOutput();
      out_block->cleanups.push_back([raw]() { delete raw; });
      raw->request_id = 0;
      raw->status_code = 0;
      raw->transcribed_text = AllocateNestedCompanyString(
          spec.GetCapacity("transcribed_text", 511), out_block);
      raw->intent_slot_json = AllocateNestedCompanyString(
          spec.GetCapacity("intent_slot_json", 1023), out_block);
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
      if (block) block->Destroy();
    };
    RegisterBinding(b);
  }

  // 16. rerank_in -> CompanyPlatformRerankInput
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "rerank_in";
    b.aliases = {"rank_in"};
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
      if (in->candidate_count <= 0 ||
          in->candidate_count > COMPANY_PLATFORM_MAX_RERANK_CANDIDATES) {
        if (err) {
          *err = "candidate_count " + std::to_string(in->candidate_count) +
                 " out of valid range [1, " +
                 std::to_string(COMPANY_PLATFORM_MAX_RERANK_CANDIDATES) + "]";
        }
        return -3;
      }
      for (int i = 0; i < in->candidate_count; ++i) {
        if (!in->candidate_passages[i]) {
          if (err)
            *err = "candidate_passages[" + std::to_string(i) + "] is null";
          return -3;
        }
        ret = ValidateCompanyString(
            in->candidate_passages[i], limits.max_doc_text_bytes,
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
    b.aliases = {"rank_out"};
    b.external_c_type_name = "CompanyPlatformRerankOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      CheckAllocFailureProbe();
      auto* raw = new CompanyPlatformRerankOutput();
      out_block->cleanups.push_back([raw]() { delete raw; });
      raw->request_id = 0;
      raw->count = 0;
      raw->status_code = 0;
      for (int i = 0; i < COMPANY_PLATFORM_MAX_RERANK_CANDIDATES; ++i) {
        raw->scores[i] = 0.0f;
        raw->sorted_indices[i] = -1;
      }
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
      for (int i = 0; i < COMPANY_PLATFORM_MAX_RERANK_CANDIDATES; ++i) {
        raw->scores[i] = 0.0f;
        raw->sorted_indices[i] = -1;
      }
    };
    b.destroy_external = [](OwnedExternalBlock* block) noexcept {
      if (block) block->Destroy();
    };
    RegisterBinding(b);
  }
}

}  // namespace alg_framework
