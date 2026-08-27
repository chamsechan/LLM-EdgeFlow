#include "adapter/platform/platform_value_type_registry.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <unordered_set>

#include "company_alg_interface.h"
#include "platform/company_platform_types.h"

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

CompanyString* AllocateNestedCompanyString(uint32_t capacity,
                                           OwnedExternalBlock* block) {
  CheckAllocFailureProbe();
  std::unique_ptr<CompanyString> str(new CompanyString());
  CheckAllocFailureProbe();
  size_t alloc_bytes = static_cast<size_t>(capacity) + 1;
  std::unique_ptr<char[]> data(new char[alloc_bytes]);
  std::memset(data.get(), 0, alloc_bytes);

  str->length = 0;
  str->data = data.get();

  block->cleanups.push_back({data.release(), DeleteCharArray});
  block->cleanups.push_back({str.get(), DeleteCompanyString});
  return str.release();
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
  CheckAllocFailureProbe();
  std::unique_ptr<CompanyAny> any(new CompanyAny());
  size_t total_bytes = 0;
  if (!CheckedMultiply(meta_num, desc->element_size, &total_bytes)) {
    return nullptr;
  }
  CheckAllocFailureProbe();
  std::unique_ptr<uint8_t[]> data(new uint8_t[total_bytes]);
  std::memset(data.get(), 0, total_bytes);

  any->type_id = type_id;
  any->element_count = 0;
  any->byte_length = 0;
  any->data = data.get();

  block->cleanups.push_back({data.release(), DeleteAnyPayload});
  block->cleanups.push_back({any.get(), DeleteCompanyAny});
  return any.release();
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

}  // namespace

bool ComputeOutputPoolBytes(const std::string& suffix,
                            const ResolvedOutputPoolSpec& spec, uint32_t depth,
                            size_t* out_bytes, std::string* err) noexcept {
  if (!out_bytes) {
    if (err) *err = "Null out_bytes pointer";
    return false;
  }
  *out_bytes = 0;

  // 1. 校验 spec.type 与 suffix 必须严格一致 (R9-005)
  if (!spec.type.empty() && spec.type != suffix) {
    if (err) {
      *err = "Output pool spec type '" + spec.type +
             "' does not match requested suffix '" + suffix + "'";
    }
    return false;
  }

  // 2. 深度校验与归一化 (depth == 0 归一化为默认 25，上限 1024)
  uint32_t effective_depth = (depth == 0) ? kDefaultOutputPoolDepth : depth;
  if (effective_depth > kMaxOutputPoolDepth) {
    if (err) {
      *err = "Output pool depth " + std::to_string(effective_depth) +
             " exceeds max limit " + std::to_string(kMaxOutputPoolDepth);
    }
    return false;
  }

  // Pool management overhead per block:
  // - sizeof(OwnedExternalBlock)
  // - sizeof(void*) in all_blocks_
  // - sizeof(void*) in free_ring_
  // - sizeof(void*) + sizeof(uint64_t) in block_states_
  constexpr size_t kPoolBookkeepingPerBlock =
      sizeof(OwnedExternalBlock) + sizeof(void*) * 3 + sizeof(uint64_t);

  size_t single_block_bytes = 0;
  size_t cleanup_count = 0;

  if (suffix == "doc_out") {
    single_block_bytes = sizeof(CompanyPlatformDocOutput);
    uint32_t cap_intent = spec.GetCapacity("intent_name", 63);
    uint32_t cap_answer = spec.GetCapacity("answer_text", 1023);
    size_t str_bytes = 0;
    if (!CheckedAdd(static_cast<size_t>(cap_intent), 1, &str_bytes) ||
        !CheckedAdd(str_bytes, static_cast<size_t>(cap_answer) + 1,
                    &str_bytes) ||
        !CheckedAdd(str_bytes, sizeof(CompanyString) * 2, &str_bytes) ||
        !CheckedAdd(single_block_bytes, str_bytes, &single_block_bytes)) {
      if (err) *err = "doc_out capacity calculation overflowed";
      return false;
    }
    cleanup_count = 4;
  } else if (suffix == "keyword_out") {
    single_block_bytes = sizeof(CompanyPlatformKeywordOutput);
    uint32_t cap_match = spec.GetCapacity("match_result_json", 2047);
    size_t str_bytes = 0;
    if (!CheckedAdd(static_cast<size_t>(cap_match), 1, &str_bytes) ||
        !CheckedAdd(str_bytes, sizeof(CompanyString), &str_bytes) ||
        !CheckedAdd(single_block_bytes, str_bytes, &single_block_bytes)) {
      if (err) *err = "keyword_out capacity calculation overflowed";
      return false;
    }
    cleanup_count = 2;
  } else if (suffix == "entity_out") {
    single_block_bytes = sizeof(CompanyPlatformEntityOutput);
    uint32_t cap_entities = spec.GetCapacity("entities_json", 2047);
    size_t str_bytes = 0;
    if (!CheckedAdd(static_cast<size_t>(cap_entities), 1, &str_bytes) ||
        !CheckedAdd(str_bytes, sizeof(CompanyString), &str_bytes) ||
        !CheckedAdd(single_block_bytes, str_bytes, &single_block_bytes)) {
      if (err) *err = "entity_out capacity calculation overflowed";
      return false;
    }
    cleanup_count = 2;
  } else if (suffix == "audit_out") {
    single_block_bytes = sizeof(CompanyPlatformAuditOutput);
    uint32_t cap_risk = spec.GetCapacity("risk_level", 31);
    uint32_t cap_clause = spec.GetCapacity("matched_policy_clause", 255);
    uint32_t cap_verdict = spec.GetCapacity("audit_verdict_json", 1023);
    size_t str_bytes = 0;
    if (!CheckedAdd(static_cast<size_t>(cap_risk), 1, &str_bytes) ||
        !CheckedAdd(str_bytes, static_cast<size_t>(cap_clause) + 1,
                    &str_bytes) ||
        !CheckedAdd(str_bytes, static_cast<size_t>(cap_verdict) + 1,
                    &str_bytes) ||
        !CheckedAdd(str_bytes, sizeof(CompanyString) * 3, &str_bytes) ||
        !CheckedAdd(single_block_bytes, str_bytes, &single_block_bytes)) {
      if (err) *err = "audit_out capacity calculation overflowed";
      return false;
    }
    cleanup_count = 6;
  } else if (suffix == "od_out") {
    single_block_bytes = sizeof(CompanyOdOutput);
    uint32_t cap_res = spec.GetCapacity("result_json", 2047);
    size_t str_bytes = 0;
    if (!CheckedAdd(static_cast<size_t>(cap_res), 1, &str_bytes) ||
        !CheckedAdd(str_bytes, sizeof(CompanyString), &str_bytes) ||
        !CheckedAdd(single_block_bytes, str_bytes, &single_block_bytes)) {
      if (err) *err = "od_out capacity calculation overflowed";
      return false;
    }
    cleanup_count = 2;
    if (spec.meta_num > 0 && spec.metadata_type_id > 0) {
      const auto* mdesc = FindCompanyAnyType(spec.metadata_type_id);
      if (mdesc && mdesc->element_size > 0) {
        size_t meta_payload = 0;
        if (!CheckedMultiply(spec.meta_num, mdesc->element_size,
                             &meta_payload)) {
          if (err) *err = "Metadata payload calculation overflowed";
          return false;
        }
        size_t meta_total = 0;
        if (!CheckedAdd(meta_payload, sizeof(CompanyAny), &meta_total) ||
            !CheckedAdd(single_block_bytes, meta_total, &single_block_bytes)) {
          if (err) *err = "Metadata total calculation overflowed";
          return false;
        }
        cleanup_count += 2;
      }
    }
  } else if (suffix == "audio_out") {
    single_block_bytes = sizeof(CompanyPlatformAudioOutput);
    uint32_t cap_trans = spec.GetCapacity("transcribed_text", 511);
    uint32_t cap_intent = spec.GetCapacity("intent_slot_json", 1023);
    size_t str_bytes = 0;
    if (!CheckedAdd(static_cast<size_t>(cap_trans), 1, &str_bytes) ||
        !CheckedAdd(str_bytes, static_cast<size_t>(cap_intent) + 1,
                    &str_bytes) ||
        !CheckedAdd(str_bytes, sizeof(CompanyString) * 2, &str_bytes) ||
        !CheckedAdd(single_block_bytes, str_bytes, &single_block_bytes)) {
      if (err) *err = "audio_out capacity calculation overflowed";
      return false;
    }
    cleanup_count = 4;
  } else if (suffix == "rerank_out") {
    single_block_bytes = sizeof(CompanyPlatformRerankOutput);
    cleanup_count = 0;
  } else {
    if (err) *err = "Unknown output suffix '" + suffix + "' for pool budgeting";
    return false;
  }

  // Add cleanup actions footprint + pool bookkeeping per block
  size_t cleanup_bytes = 0;
  if (!CheckedMultiply(cleanup_count, sizeof(CleanupAction), &cleanup_bytes) ||
      !CheckedAdd(single_block_bytes, cleanup_bytes, &single_block_bytes) ||
      !CheckedAdd(single_block_bytes, kPoolBookkeepingPerBlock,
                  &single_block_bytes)) {
    if (err) *err = "Block footprint calculation overflowed";
    return false;
  }

  size_t total_pool_bytes = 0;
  if (!CheckedMultiply(effective_depth, single_block_bytes,
                       &total_pool_bytes) ||
      !CheckedAdd(total_pool_bytes, 1024, &total_pool_bytes)) {
    if (err) *err = "Total pool memory calculation overflowed";
    return false;
  }

  if (total_pool_bytes > kMaxHandlePoolMemoryBytes) {
    if (err) {
      *err = "Total pool memory (" + std::to_string(total_pool_bytes) +
             " bytes) exceeds maximum allowed budget " +
             std::to_string(kMaxHandlePoolMemoryBytes) + " bytes (64 MiB)";
    }
    return false;
  }

  *out_bytes = total_pool_bytes;
  return true;
}

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

bool PlatformValueTypeRegistry::HasConflict() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_conflict_;
}

int PlatformValueTypeRegistry::GlobalInit() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (has_conflict_) {
    return -6;
  }
  if (audited_) {
    return 0;
  }
  for (const auto& [suffix, binding] : bindings_by_canonical_) {
    if (binding.canonical_suffix.empty() ||
        binding.external_c_type_name.empty()) {
      has_conflict_ = true;
      return -6;
    }
    bool is_output =
        (suffix.size() >= 4 && suffix.rfind("_out") == suffix.size() - 4);
    if (is_output) {
      if (!binding.allocate_external || !binding.reset_external ||
          !binding.destroy_external) {
        has_conflict_ = true;
        return -6;
      }
    } else {
      if (!binding.validate_external) {
        has_conflict_ = true;
        return -6;
      }
    }
    for (const auto& a : binding.aliases) {
      auto it = alias_to_canonical_.find(a);
      if (it == alias_to_canonical_.end() || it->second != suffix) {
        has_conflict_ = true;
        return -6;
      }
    }
  }
  audited_ = true;
  return 0;
}

bool PlatformValueTypeRegistry::RegisterBinding(
    PlatformValueTypeBinding binding) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (audited_) {
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

  // 4. 原子预检通过后，通过 Copy-and-Swap 一次性提交
  try {
    auto temp_bindings = bindings_by_canonical_;
    auto temp_aliases = alias_to_canonical_;

    for (const auto& a : binding.aliases) {
      temp_aliases[a] = binding.canonical_suffix;
    }
    temp_bindings[binding.canonical_suffix] = std::move(binding);

    bindings_by_canonical_ = std::move(temp_bindings);
    alias_to_canonical_ = std::move(temp_aliases);
  } catch (...) {
    // 资源分配或复制异常不污染契约冲突状态，原快照保持不变
    return false;
  }
  return true;
}

std::string PlatformValueTypeRegistry::NormalizeSuffix(
    const std::string& suffix) const {
  const auto* b = GetBindingBySuffix(suffix);
  return b ? b->canonical_suffix : "";
}

const PlatformValueTypeBinding* PlatformValueTypeRegistry::GetBindingBySuffix(
    const std::string& suffix) const {
  std::lock_guard<std::mutex> lock(mutex_);
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
  // 1. string -> CompanyString (RFC 6.3: aliases: {})
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "string";
    b.aliases = {};
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

  // 2. buffer -> CompanyBuffer (RFC 6.3: aliases: {})
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "buffer";
    b.aliases = {};
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

  // 3. any -> CompanyAny (RFC 6.3: aliases: {})
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "any";
    b.aliases = {};
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

  // 4. frame -> CompanyFrame (RFC 6.3: aliases: {"image_in"})
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

  // 5. od_out -> CompanyOdOutput (RFC 6.3: aliases: {"ocr_out"})
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "od_out";
    b.aliases = {"ocr_out"};
    b.external_c_type_name = "CompanyOdOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      out_block->cleanups.reserve(5);
      CheckAllocFailureProbe();
      std::unique_ptr<CompanyOdOutput> raw_holder(new CompanyOdOutput());
      auto* raw = raw_holder.get();
      out_block->cleanups.push_back({raw, DeleteTypedObject<CompanyOdOutput>});
      raw_holder.release();

      raw->request_id = 0;
      raw->detected_box_count = 0;
      raw->status_code = 0;
      raw->result_json = AllocateNestedCompanyString(
          spec.GetCapacity("result_json", 2047), out_block);
      raw->metadata = AllocateNestedCompanyAny(
          spec.meta_num, spec.metadata_type_id, out_block);
      out_block->raw_struct = raw;
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

  // 6. keyword_in -> CompanyPlatformKeywordInput (RFC 6.3: aliases:
  // {"sentence_in"})
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

  // 7. keyword_out -> CompanyPlatformKeywordOutput (RFC 6.3: aliases:
  // {"match_out"})
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "keyword_out";
    b.aliases = {"match_out"};
    b.external_c_type_name = "CompanyPlatformKeywordOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      out_block->cleanups.reserve(3);
      CheckAllocFailureProbe();
      std::unique_ptr<CompanyPlatformKeywordOutput> raw_holder(
          new CompanyPlatformKeywordOutput());
      auto* raw = raw_holder.get();
      out_block->cleanups.push_back(
          {raw, DeleteTypedObject<CompanyPlatformKeywordOutput>});
      raw_holder.release();

      raw->request_id = 0;
      raw->is_hit = 0;
      raw->match_result_json = AllocateNestedCompanyString(
          spec.GetCapacity("match_result_json", 2047), out_block);
      out_block->raw_struct = raw;
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

  // 8. entity_in -> CompanyPlatformEntityInput (RFC 6.3: aliases: {"text_in"})
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

  // 9. entity_out -> CompanyPlatformEntityOutput (RFC 6.3: aliases:
  // {"extracted_out"})
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "entity_out";
    b.aliases = {"extracted_out"};
    b.external_c_type_name = "CompanyPlatformEntityOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      out_block->cleanups.reserve(3);
      CheckAllocFailureProbe();
      std::unique_ptr<CompanyPlatformEntityOutput> raw_holder(
          new CompanyPlatformEntityOutput());
      auto* raw = raw_holder.get();
      out_block->cleanups.push_back(
          {raw, DeleteTypedObject<CompanyPlatformEntityOutput>});
      raw_holder.release();

      raw->request_id = 0;
      raw->status_code = 0;
      raw->entities_json = AllocateNestedCompanyString(
          spec.GetCapacity("entities_json", 2047), out_block);
      out_block->raw_struct = raw;
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

  // 10. doc_in -> CompanyPlatformDocInput (RFC 6.3: aliases: {"qa_in"})
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

  // 11. doc_out -> CompanyPlatformDocOutput (RFC 6.3: aliases: {"qa_out"})
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "doc_out";
    b.aliases = {"qa_out"};
    b.external_c_type_name = "CompanyPlatformDocOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      out_block->cleanups.reserve(5);
      CheckAllocFailureProbe();
      std::unique_ptr<CompanyPlatformDocOutput> raw_holder(
          new CompanyPlatformDocOutput());
      auto* raw = raw_holder.get();
      out_block->cleanups.push_back(
          {raw, DeleteTypedObject<CompanyPlatformDocOutput>});
      raw_holder.release();

      raw->request_id = 0;
      raw->confidence = 0.0f;
      raw->chunk_count = 0;
      raw->status_code = 0;
      raw->intent_name = AllocateNestedCompanyString(
          spec.GetCapacity("intent_name", 63), out_block);
      raw->answer_text = AllocateNestedCompanyString(
          spec.GetCapacity("answer_text", 1023), out_block);
      out_block->raw_struct = raw;
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

  // 12. audit_in -> CompanyPlatformAuditInput (RFC 6.3: aliases:
  // {"dialogue_in"})
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

  // 13. audit_out -> CompanyPlatformAuditOutput (RFC 6.3: aliases:
  // {"verdict_out"})
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "audit_out";
    b.aliases = {"verdict_out"};
    b.external_c_type_name = "CompanyPlatformAuditOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      out_block->cleanups.reserve(7);
      CheckAllocFailureProbe();
      std::unique_ptr<CompanyPlatformAuditOutput> raw_holder(
          new CompanyPlatformAuditOutput());
      auto* raw = raw_holder.get();
      out_block->cleanups.push_back(
          {raw, DeleteTypedObject<CompanyPlatformAuditOutput>});
      raw_holder.release();

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

  // 14. audio_in -> CompanyPlatformAudioInput (RFC 6.3: aliases:
  // {"pcm_stream"})
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

  // 15. audio_out -> CompanyPlatformAudioOutput (RFC 6.3: aliases: {"asr_out"})
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "audio_out";
    b.aliases = {"asr_out"};
    b.external_c_type_name = "CompanyPlatformAudioOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      out_block->cleanups.reserve(5);
      CheckAllocFailureProbe();
      std::unique_ptr<CompanyPlatformAudioOutput> raw_holder(
          new CompanyPlatformAudioOutput());
      auto* raw = raw_holder.get();
      out_block->cleanups.push_back(
          {raw, DeleteTypedObject<CompanyPlatformAudioOutput>});
      raw_holder.release();

      raw->request_id = 0;
      raw->status_code = 0;
      raw->transcribed_text = AllocateNestedCompanyString(
          spec.GetCapacity("transcribed_text", 511), out_block);
      raw->intent_slot_json = AllocateNestedCompanyString(
          spec.GetCapacity("intent_slot_json", 1023), out_block);
      out_block->raw_struct = raw;
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

  // 16. rerank_in -> CompanyPlatformRerankInput (RFC 6.3: aliases: {"pair_in"})
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

  // 17. rerank_out -> CompanyPlatformRerankOutput (RFC 6.3: aliases:
  // {"scores_out"})
  {
    PlatformValueTypeBinding b;
    b.canonical_suffix = "rerank_out";
    b.aliases = {"scores_out"};
    b.external_c_type_name = "CompanyPlatformRerankOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      out_block->cleanups.reserve(1);
      CheckAllocFailureProbe();
      std::unique_ptr<CompanyPlatformRerankOutput> raw_holder(
          new CompanyPlatformRerankOutput());
      auto* raw = raw_holder.get();
      out_block->cleanups.push_back(
          {raw, DeleteTypedObject<CompanyPlatformRerankOutput>});
      raw_holder.release();

      raw->request_id = 0;
      raw->count = 0;
      raw->status_code = 0;
      for (int i = 0; i < COMPANY_PLATFORM_MAX_RERANK_CANDIDATES; ++i) {
        raw->scores[i] = 0.0f;
        raw->sorted_indices[i] = -1;
      }
      out_block->raw_struct = raw;
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
