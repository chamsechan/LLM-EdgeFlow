#include "adapter/platform/platform_business_bridge_registry.h"

#include <iostream>
#include <unordered_set>

#include "adapter/business_adapter_registry.h"

namespace alg_framework {

// Declarations of bridge registration helper functions
void RegisterKeywordMatchBridge(PlatformBusinessBridgeRegistry& reg);
void RegisterEntityExtractBridge(PlatformBusinessBridgeRegistry& reg);
void RegisterDocQaBridge(PlatformBusinessBridgeRegistry& reg);
void RegisterComplianceAuditBridge(PlatformBusinessBridgeRegistry& reg);
void RegisterOcrDocQaBridge(PlatformBusinessBridgeRegistry& reg);
void RegisterAudioAsrIntentBridge(PlatformBusinessBridgeRegistry& reg);
void RegisterCrossRerankBridge(PlatformBusinessBridgeRegistry& reg);

PlatformBusinessBridgeRegistry& PlatformBusinessBridgeRegistry::Instance() {
  static PlatformBusinessBridgeRegistry instance;
  return instance;
}

int PlatformBusinessBridgeRegistry::CopyToPooledString(
    const char* src, CompanyString* dest, uint32_t capacity,
    const char* field_name, std::string* err) noexcept {
  if (!dest || !dest->data) {
    if (err)
      *err = std::string(field_name) + " in destination pool block is null";
    return -4;
  }
  if (!src) {
    dest->length = 0;
    dest->data[0] = '\0';
    return 0;
  }
  size_t len = std::strlen(src);
  if (len > capacity) {
    if (err)
      *err = std::string(field_name) + " output length (" +
             std::to_string(len) + ") exceeds pool capacity (" +
             std::to_string(capacity) + ")";
    return -4;
  }
  std::memcpy(dest->data, src, len);
  dest->data[len] = '\0';
  dest->length = static_cast<int32_t>(len);
  return 0;
}

PlatformBusinessBridgeRegistry::PlatformBusinessBridgeRegistry() {
  RegisterBuiltinBridges();
}

bool PlatformBusinessBridgeRegistry::RegisterBridge(
    PlatformBusinessBridgeDescriptor desc) {
  int32_t key = static_cast<int32_t>(desc.biz_type);
  if (key == 0 || desc.biz_name.empty()) {
    has_conflict_ = true;
    return false;
  }
  auto it = bridges_by_biz_type_.find(key);
  if (it != bridges_by_biz_type_.end()) {
    if (it->second.biz_name != desc.biz_name) {
      has_conflict_ = true;
      return false;
    }
    return true;
  }

  // 校验槽位命名唯一性与方向
  std::unordered_set<std::string> in_names, in_suffixes;
  for (const auto& s : desc.input_slots) {
    if (s.logical_name.empty() || s.type_suffix.empty()) {
      has_conflict_ = true;
      return false;
    }
    if (s.direction != IoDirection::kInput) {
      has_conflict_ = true;
      return false;
    }
    if (!in_names.insert(s.logical_name).second ||
        !in_suffixes.insert(s.type_suffix).second) {
      has_conflict_ = true;
      return false;
    }
  }

  std::unordered_set<std::string> out_names, out_suffixes;
  for (const auto& s : desc.output_slots) {
    if (s.logical_name.empty() || s.type_suffix.empty()) {
      has_conflict_ = true;
      return false;
    }
    if (s.direction != IoDirection::kOutput) {
      has_conflict_ = true;
      return false;
    }
    if (!out_names.insert(s.logical_name).second ||
        !out_suffixes.insert(s.type_suffix).second) {
      has_conflict_ = true;
      return false;
    }
  }

  if (!desc.convert_sample_input || !desc.convert_sample_output ||
      !desc.create_shadow_output_dto) {
    has_conflict_ = true;
    return false;
  }

  bridges_by_biz_type_[key] = std::move(desc);
  return true;
}

const PlatformBusinessBridgeDescriptor*
PlatformBusinessBridgeRegistry::GetBridge(CompanyAlgBizType biz_type) const {
  auto it = bridges_by_biz_type_.find(static_cast<int32_t>(biz_type));
  if (it != bridges_by_biz_type_.end()) {
    return &it->second;
  }
  return nullptr;
}

int PlatformBusinessBridgeRegistry::GlobalInit() {
  if (has_conflict_) {
    return -6;
  }
  // 全面原子审计：BizType, Adapter, 槽位规范后缀与生命周期函数
  for (const auto& [biz_type, desc] : bridges_by_biz_type_) {
    auto adapter = BusinessAdapterRegistry::Instance().GetAdapter(
        static_cast<CompanyAlgBizType>(biz_type));
    if (!adapter) {
      has_conflict_ = true;
      return -6;
    }
    if (adapter->BizType() != static_cast<CompanyAlgBizType>(biz_type)) {
      has_conflict_ = true;
      return -6;
    }
    for (const auto& slot : desc.input_slots) {
      if (slot.direction != IoDirection::kInput) {
        has_conflict_ = true;
        return -6;
      }
      const auto* binding =
          PlatformValueTypeRegistry::Instance().GetBindingBySuffix(
              slot.type_suffix);
      if (!binding || binding->canonical_suffix != slot.type_suffix ||
          !binding->validate_external) {
        has_conflict_ = true;
        return -6;
      }
    }
    for (const auto& slot : desc.output_slots) {
      if (slot.direction != IoDirection::kOutput) {
        has_conflict_ = true;
        return -6;
      }
      const auto* binding =
          PlatformValueTypeRegistry::Instance().GetBindingBySuffix(
              slot.type_suffix);
      if (!binding || binding->canonical_suffix != slot.type_suffix ||
          !binding->allocate_external || !binding->reset_external ||
          !binding->destroy_external) {
        has_conflict_ = true;
        return -6;
      }
    }
  }
  audited_ = true;
  return 0;
}

void PlatformBusinessBridgeRegistry::RegisterBuiltinBridges() {
  RegisterKeywordMatchBridge(*this);
  RegisterEntityExtractBridge(*this);
  RegisterDocQaBridge(*this);
  RegisterComplianceAuditBridge(*this);
  RegisterOcrDocQaBridge(*this);
  RegisterAudioAsrIntentBridge(*this);
  RegisterCrossRerankBridge(*this);
}

}  // namespace alg_framework
