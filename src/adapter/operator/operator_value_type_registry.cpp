#include "adapter/operator/operator_value_type_registry.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <unordered_set>

#include "adapter/operator/operator_output_pool.h"
#include "company_alg_interface.h"
#include "operator/company_operator_types.h"

namespace alg_framework {

namespace {

std::atomic<int> g_alloc_failure_countdown{-1};
std::atomic<OperatorValueTypeRegistry::RegistryExceptionInjectPoint>
    g_registry_inject_point{
        OperatorValueTypeRegistry::RegistryExceptionInjectPoint::kNone};

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
  OutputPoolState::RecordDestroyed();
  delete[] static_cast<char*>(p);
}

inline void DeleteCompanyString(void* p) noexcept {
  OutputPoolState::RecordDestroyed();
  delete static_cast<CompanyString*>(p);
}

inline void DeleteCompanyAny(void* p) noexcept {
  OutputPoolState::RecordDestroyed();
  delete static_cast<CompanyAny*>(p);
}

inline void DeleteAnyPayload(void* p) noexcept {
  OutputPoolState::RecordDestroyed();
  delete[] static_cast<uint8_t*>(p);
}

template <typename T>
inline void DeleteTypedObject(void* p) noexcept {
  OutputPoolState::RecordDestroyed();
  delete static_cast<T*>(p);
}

void RegisterCleanup(OwnedExternalBlock* block, CleanupAction action);

template <typename T>
T* AllocateRootOutput(size_t cleanup_capacity, OwnedExternalBlock* block) {
  if (OutputPoolState::GetFailureStageProbe() ==
      OutputPoolState::FailureStage::kRootStructAlloc) {
    return nullptr;
  }
  block->cleanups.reserve(cleanup_capacity);
  CheckAllocFailureProbe();
  std::unique_ptr<T, decltype(&DeleteTypedObject<T>)> holder(
      new T(), DeleteTypedObject<T>);
  OutputPoolState::RecordConstructed();
  T* raw = holder.get();
  RegisterCleanup(block, {raw, DeleteTypedObject<T>});
  holder.release();
  return raw;
}

void DestroyExternalBlock(OwnedExternalBlock* block) noexcept {
  if (block) block->Destroy();
}

void RegisterCleanup(OwnedExternalBlock* block, CleanupAction action) {
  if (OutputPoolState::GetFailureStageProbe() ==
          OutputPoolState::FailureStage::kCleanupRegister &&
      static_cast<int>(block->cleanups.size()) ==
          OutputPoolState::GetTargetBlockIndex()) {
    throw std::bad_alloc();
  }
  block->cleanups.push_back(action);
}

CompanyString* AllocateNestedCompanyString(uint32_t capacity,
                                           OwnedExternalBlock* block) {
  if (OutputPoolState::GetFailureStageProbe() ==
      OutputPoolState::FailureStage::kNestedStringAlloc) {
    throw std::bad_alloc();
  }
  CheckAllocFailureProbe();
  std::unique_ptr<CompanyString, decltype(&DeleteCompanyString)> str(
      new CompanyString(), DeleteCompanyString);
  OutputPoolState::RecordConstructed();
  CheckAllocFailureProbe();
  size_t alloc_bytes = static_cast<size_t>(capacity) + 1;
  std::unique_ptr<char[], decltype(&DeleteCharArray)> data(
      new char[alloc_bytes], DeleteCharArray);
  OutputPoolState::RecordConstructed();
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
  if (OutputPoolState::GetFailureStageProbe() ==
      OutputPoolState::FailureStage::kNestedAnyAlloc) {
    throw std::bad_alloc();
  }
  CheckAllocFailureProbe();
  std::unique_ptr<CompanyAny, decltype(&DeleteCompanyAny)> any(
      new CompanyAny(), DeleteCompanyAny);
  OutputPoolState::RecordConstructed();
  size_t total_bytes = 0;
  if (!CheckedMultiply(meta_num, desc->element_size, &total_bytes)) {
    return nullptr;
  }
  CheckAllocFailureProbe();
  std::unique_ptr<uint8_t[], decltype(&DeleteAnyPayload)> data(
      new uint8_t[total_bytes], DeleteAnyPayload);
  OutputPoolState::RecordConstructed();
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

}  // namespace

bool ComputeOutputPoolPayloadBytes(const std::string& suffix,
                                   const ResolvedOutputPoolSpec& spec,
                                   uint32_t depth, size_t* out_bytes,
                                   std::string* err) noexcept {
  if (!out_bytes) {
    if (err) *err = "Null out_bytes pointer";
    return false;
  }
  *out_bytes = 0;

  // 1. 校验 spec.type 与 suffix 必须严格一致 (空字符串同样拒绝，R9-005)
  if (spec.type.empty() || spec.type != suffix) {
    if (err) {
      *err = "Output pool spec type '" + spec.type +
             "' is invalid or does not match requested suffix '" + suffix + "'";
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

  if (suffix != "od_out" &&
      (spec.meta_num != 0 || spec.metadata_type_id != 0)) {
    if (err) {
      *err =
          "Metadata capacity is unsupported for output suffix '" + suffix + "'";
    }
    return false;
  }

  size_t single_block_bytes = 0;

  if (suffix == "doc_out") {
    single_block_bytes = sizeof(CompanyOperatorDocOutput);
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
  } else if (suffix == "keyword_out") {
    single_block_bytes = sizeof(CompanyOperatorKeywordOutput);
    uint32_t cap_match = spec.GetCapacity("match_result_json", 2047);
    size_t str_bytes = 0;
    if (!CheckedAdd(static_cast<size_t>(cap_match), 1, &str_bytes) ||
        !CheckedAdd(str_bytes, sizeof(CompanyString), &str_bytes) ||
        !CheckedAdd(single_block_bytes, str_bytes, &single_block_bytes)) {
      if (err) *err = "keyword_out capacity calculation overflowed";
      return false;
    }
  } else if (suffix == "entity_out") {
    single_block_bytes = sizeof(CompanyOperatorEntityOutput);
    uint32_t cap_entities = spec.GetCapacity("entities_json", 2047);
    size_t str_bytes = 0;
    if (!CheckedAdd(static_cast<size_t>(cap_entities), 1, &str_bytes) ||
        !CheckedAdd(str_bytes, sizeof(CompanyString), &str_bytes) ||
        !CheckedAdd(single_block_bytes, str_bytes, &single_block_bytes)) {
      if (err) *err = "entity_out capacity calculation overflowed";
      return false;
    }
  } else if (suffix == "audit_out") {
    single_block_bytes = sizeof(CompanyOperatorAuditOutput);
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
    if ((spec.meta_num == 0) != (spec.metadata_type_id == 0)) {
      if (err) *err = "Metadata count and type must both be zero or non-zero";
      return false;
    }
    if (spec.meta_num > 0) {
      const auto* mdesc = FindCompanyAnyType(spec.metadata_type_id);
      if (!mdesc || mdesc->element_size == 0) {
        if (err) *err = "Metadata type is not registered";
        return false;
      }
      size_t meta_payload = 0;
      if (!CheckedMultiply(spec.meta_num, mdesc->element_size, &meta_payload)) {
        if (err) *err = "Metadata payload calculation overflowed";
        return false;
      }
      size_t meta_total = 0;
      if (!CheckedAdd(meta_payload, sizeof(CompanyAny), &meta_total) ||
          !CheckedAdd(single_block_bytes, meta_total, &single_block_bytes)) {
        if (err) *err = "Metadata total calculation overflowed";
        return false;
      }
    }
  } else if (suffix == "audio_out") {
    single_block_bytes = sizeof(CompanyOperatorAudioOutput);
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
  } else if (suffix == "rerank_out") {
    single_block_bytes = sizeof(CompanyOperatorRerankOutput);
  } else {
    if (err) *err = "Unknown output suffix '" + suffix + "' for pool budgeting";
    return false;
  }

  size_t total_pool_bytes = 0;
  if (!CheckedMultiply(effective_depth, single_block_bytes,
                       &total_pool_bytes)) {
    if (err) *err = "Total pool payload calculation overflowed";
    return false;
  }

  if (total_pool_bytes > kMaxHandlePoolPayloadBytes) {
    if (err) {
      *err = "Total pool payload (" + std::to_string(total_pool_bytes) +
             " bytes) exceeds maximum allowed payload budget " +
             std::to_string(kMaxHandlePoolPayloadBytes) + " bytes (64 MiB)";
    }
    return false;
  }

  *out_bytes = total_pool_bytes;
  return true;
}

void OperatorValueTypeRegistry::SetAllocationFailureCountdown(
    int count) noexcept {
  g_alloc_failure_countdown.store(count);
}

int OperatorValueTypeRegistry::GetAllocationFailureCountdown() noexcept {
  return g_alloc_failure_countdown.load();
}

void OperatorValueTypeRegistry::SetExceptionInjectPoint(
    RegistryExceptionInjectPoint point) noexcept {
  g_registry_inject_point.store(point);
}

OperatorValueTypeRegistry::RegistryExceptionInjectPoint
OperatorValueTypeRegistry::GetExceptionInjectPoint() noexcept {
  return g_registry_inject_point.load();
}

const CompanyAnyTypeDescriptor* FindCompanyAnyType(int32_t type_id) noexcept {
  for (const auto& item : kBuiltinAnyTypes) {
    if (item.type_id == type_id) {
      return &item;
    }
  }
  return nullptr;
}

OperatorValueTypeRegistry& OperatorValueTypeRegistry::Instance() {
  static OperatorValueTypeRegistry instance;
  return instance;
}

bool OperatorValueTypeRegistry::ParseKey(const std::string& key,
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

int OperatorValueTypeRegistry::ValidateCompanyString(
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

int OperatorValueTypeRegistry::ValidateCompanyBuffer(
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

int OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
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

bool OperatorValueTypeRegistry::HasConflict() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_conflict_;
}

int OperatorValueTypeRegistry::GlobalInit() {
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

bool OperatorValueTypeRegistry::RegisterBinding(
    const OperatorValueTypeBinding& binding) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (audited_) {
    return false;
  }
  if (binding.canonical_suffix.empty() ||
      binding.external_c_type_name.empty()) {
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
  try {
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
  } catch (...) {
    return false;
  }

  // 4. 原子预检通过后，通过 Copy-and-Swap 一次性提交
  try {
    if (g_registry_inject_point.load() ==
        OperatorValueTypeRegistry::RegistryExceptionInjectPoint::
            kCopyCanonicalMap) {
      throw std::runtime_error("Injected failure copying canonical map");
    }
    auto temp_bindings = bindings_by_canonical_;

    if (g_registry_inject_point.load() ==
        OperatorValueTypeRegistry::RegistryExceptionInjectPoint::
            kCopyAliasMap) {
      throw std::runtime_error("Injected failure copying alias map");
    }
    auto temp_aliases = alias_to_canonical_;

    for (size_t i = 0; i < binding.aliases.size(); ++i) {
      if (g_registry_inject_point.load() ==
              OperatorValueTypeRegistry::RegistryExceptionInjectPoint::
                  kSecondAliasInsert &&
          i == 1) {
        throw std::runtime_error("Injected failure on second alias insert");
      }
      temp_aliases[binding.aliases[i]] = binding.canonical_suffix;
    }

    if (g_registry_inject_point.load() ==
        OperatorValueTypeRegistry::RegistryExceptionInjectPoint::
            kCanonicalInsert) {
      throw std::runtime_error("Injected failure on canonical insert");
    }
    temp_bindings[binding.canonical_suffix] = binding;

    if (g_registry_inject_point.load() ==
        OperatorValueTypeRegistry::RegistryExceptionInjectPoint::kPublish) {
      throw std::runtime_error("Injected failure before registry publish");
    }
    bindings_by_canonical_.swap(temp_bindings);
    alias_to_canonical_.swap(temp_aliases);
  } catch (...) {
    // 资源分配或复制异常不污染契约冲突状态，原快照保持不变
    return false;
  }
  return true;
}

std::string OperatorValueTypeRegistry::NormalizeSuffix(
    const std::string& suffix) const {
  const auto* b = GetBindingBySuffix(suffix);
  return b ? b->canonical_suffix : "";
}

const OperatorValueTypeBinding* OperatorValueTypeRegistry::GetBindingBySuffix(
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

OperatorValueTypeRegistry::OperatorValueTypeRegistry() {
  RegisterBuiltinBindings();
}

void OperatorValueTypeRegistry::RegisterBuiltinBindings() {
  // 1. string -> CompanyString (RFC 6.3: aliases: {})
  {
    OperatorValueTypeBinding b;
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
    OperatorValueTypeBinding b;
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
    OperatorValueTypeBinding b;
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
    OperatorValueTypeBinding b;
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
    OperatorValueTypeBinding b;
    b.canonical_suffix = "od_out";
    b.aliases = {"ocr_out"};
    b.external_c_type_name = "CompanyOdOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = AllocateRootOutput<CompanyOdOutput>(5, out_block);
      if (!raw) return -4;

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
    b.destroy_external = DestroyExternalBlock;
    RegisterBinding(b);
  }

  // 6. keyword_in -> CompanyOperatorKeywordInput (RFC 6.3: aliases:
  // {"sentence_in"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "keyword_in";
    b.aliases = {"sentence_in"};
    b.external_c_type_name = "CompanyOperatorKeywordInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyOperatorKeywordInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyOperatorKeywordInput*>(ptr);
      if (!in->sentence_text) {
        if (err) *err = "sentence_text pointer is null";
        return -3;
      }
      return ValidateCompanyString(in->sentence_text, limits.max_text_bytes,
                                   "sentence_text", err);
    };
    RegisterBinding(b);
  }

  // 7. keyword_out -> CompanyOperatorKeywordOutput (RFC 6.3: aliases:
  // {"match_out"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "keyword_out";
    b.aliases = {"match_out"};
    b.external_c_type_name = "CompanyOperatorKeywordOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw =
          AllocateRootOutput<CompanyOperatorKeywordOutput>(3, out_block);
      if (!raw) return -4;

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
      auto* raw = static_cast<CompanyOperatorKeywordOutput*>(ptr);
      raw->request_id = 0;
      raw->is_hit = 0;
      ResetNestedCompanyString(raw->match_result_json);
    };
    b.destroy_external = DestroyExternalBlock;
    RegisterBinding(b);
  }

  // 8. entity_in -> CompanyOperatorEntityInput (RFC 6.3: aliases: {"text_in"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "entity_in";
    b.aliases = {"text_in"};
    b.external_c_type_name = "CompanyOperatorEntityInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyOperatorEntityInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyOperatorEntityInput*>(ptr);
      if (!in->sentence_text) {
        if (err) *err = "sentence_text pointer is null";
        return -3;
      }
      return ValidateCompanyString(in->sentence_text, limits.max_text_bytes,
                                   "sentence_text", err);
    };
    RegisterBinding(b);
  }

  // 9. entity_out -> CompanyOperatorEntityOutput (RFC 6.3: aliases:
  // {"extracted_out"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "entity_out";
    b.aliases = {"extracted_out"};
    b.external_c_type_name = "CompanyOperatorEntityOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = AllocateRootOutput<CompanyOperatorEntityOutput>(3, out_block);
      if (!raw) return -4;

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
      auto* raw = static_cast<CompanyOperatorEntityOutput*>(ptr);
      raw->request_id = 0;
      raw->status_code = 0;
      ResetNestedCompanyString(raw->entities_json);
    };
    b.destroy_external = DestroyExternalBlock;
    RegisterBinding(b);
  }

  // 10. doc_in -> CompanyOperatorDocInput (RFC 6.3: aliases: {"qa_in"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "doc_in";
    b.aliases = {"qa_in"};
    b.external_c_type_name = "CompanyOperatorDocInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyOperatorDocInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyOperatorDocInput*>(ptr);
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

  // 11. doc_out -> CompanyOperatorDocOutput (RFC 6.3: aliases: {"qa_out"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "doc_out";
    b.aliases = {"qa_out"};
    b.external_c_type_name = "CompanyOperatorDocOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = AllocateRootOutput<CompanyOperatorDocOutput>(5, out_block);
      if (!raw) return -4;

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
      auto* raw = static_cast<CompanyOperatorDocOutput*>(ptr);
      raw->request_id = 0;
      raw->confidence = 0.0f;
      raw->chunk_count = 0;
      raw->status_code = 0;
      ResetNestedCompanyString(raw->intent_name);
      ResetNestedCompanyString(raw->answer_text);
    };
    b.destroy_external = DestroyExternalBlock;
    RegisterBinding(b);
  }

  // 12. audit_in -> CompanyOperatorAuditInput (RFC 6.3: aliases:
  // {"dialogue_in"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "audit_in";
    b.aliases = {"dialogue_in"};
    b.external_c_type_name = "CompanyOperatorAuditInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyOperatorAuditInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyOperatorAuditInput*>(ptr);
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

  // 13. audit_out -> CompanyOperatorAuditOutput (RFC 6.3: aliases:
  // {"verdict_out"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "audit_out";
    b.aliases = {"verdict_out"};
    b.external_c_type_name = "CompanyOperatorAuditOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = AllocateRootOutput<CompanyOperatorAuditOutput>(7, out_block);
      if (!raw) return -4;

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
      auto* raw = static_cast<CompanyOperatorAuditOutput*>(ptr);
      raw->request_id = 0;
      raw->risk_score = 0.0f;
      raw->status_code = 0;
      ResetNestedCompanyString(raw->risk_level);
      ResetNestedCompanyString(raw->matched_policy_clause);
      ResetNestedCompanyString(raw->audit_verdict_json);
    };
    b.destroy_external = DestroyExternalBlock;
    RegisterBinding(b);
  }

  // 14. audio_in -> CompanyOperatorAudioInput (RFC 6.3: aliases:
  // {"pcm_stream"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "audio_in";
    b.aliases = {"pcm_stream"};
    b.external_c_type_name = "CompanyOperatorAudioInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyOperatorAudioInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyOperatorAudioInput*>(ptr);
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

  // 15. audio_out -> CompanyOperatorAudioOutput (RFC 6.3: aliases: {"asr_out"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "audio_out";
    b.aliases = {"asr_out"};
    b.external_c_type_name = "CompanyOperatorAudioOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = AllocateRootOutput<CompanyOperatorAudioOutput>(5, out_block);
      if (!raw) return -4;

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
      auto* raw = static_cast<CompanyOperatorAudioOutput*>(ptr);
      raw->request_id = 0;
      raw->status_code = 0;
      ResetNestedCompanyString(raw->transcribed_text);
      ResetNestedCompanyString(raw->intent_slot_json);
    };
    b.destroy_external = DestroyExternalBlock;
    RegisterBinding(b);
  }

  // 16. rerank_in -> CompanyOperatorRerankInput (RFC 6.3: aliases: {"pair_in"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "rerank_in";
    b.aliases = {"pair_in"};
    b.external_c_type_name = "CompanyOperatorRerankInput";
    b.validate_external = [](const void* ptr, const ResolvedInputLimits& limits,
                             std::string* err) -> int {
      if (!ptr) {
        if (err) *err = "CompanyOperatorRerankInput pointer is null";
        return -3;
      }
      const auto* in = static_cast<const CompanyOperatorRerankInput*>(ptr);
      if (!in->query_text) {
        if (err) *err = "query_text pointer is null";
        return -3;
      }
      int ret = ValidateCompanyString(in->query_text, limits.max_text_bytes,
                                      "query_text", err);
      if (ret != 0) return ret;
      if (in->candidate_count <= 0 ||
          in->candidate_count > COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) {
        if (err) {
          *err = "candidate_count " + std::to_string(in->candidate_count) +
                 " out of valid range [1, " +
                 std::to_string(COMPANY_OPERATOR_MAX_RERANK_CANDIDATES) + "]";
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

  // 17. rerank_out -> CompanyOperatorRerankOutput (RFC 6.3: aliases:
  // {"scores_out"})
  {
    OperatorValueTypeBinding b;
    b.canonical_suffix = "rerank_out";
    b.aliases = {"scores_out"};
    b.external_c_type_name = "CompanyOperatorRerankOutput";
    b.allocate_external = [](const ResolvedOutputPoolSpec& /*spec*/,
                             OwnedExternalBlock* out_block,
                             std::string* /*err*/) -> int {
      auto* raw = AllocateRootOutput<CompanyOperatorRerankOutput>(1, out_block);
      if (!raw) return -4;

      raw->request_id = 0;
      raw->count = 0;
      raw->status_code = 0;
      for (int i = 0; i < COMPANY_OPERATOR_MAX_RERANK_CANDIDATES; ++i) {
        raw->scores[i] = 0.0f;
        raw->sorted_indices[i] = -1;
      }
      out_block->raw_struct = raw;
      return 0;
    };
    b.reset_external = [](void* ptr,
                          const ResolvedOutputPoolSpec& /*spec*/) noexcept {
      if (!ptr) return;
      auto* raw = static_cast<CompanyOperatorRerankOutput*>(ptr);
      raw->request_id = 0;
      raw->count = 0;
      raw->status_code = 0;
      for (int i = 0; i < COMPANY_OPERATOR_MAX_RERANK_CANDIDATES; ++i) {
        raw->scores[i] = 0.0f;
        raw->sorted_indices[i] = -1;
      }
    };
    b.destroy_external = DestroyExternalBlock;
    RegisterBinding(b);
  }
}

}  // namespace alg_framework
