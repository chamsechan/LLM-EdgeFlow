#include "adapter/operator/operator_biz_bridge_registry.h"

#include <iostream>
#include <unordered_set>

#include "adapter/biz_adapter_registry.h"

namespace alg_framework {

OperatorBizBridgeRegistry& OperatorBizBridgeRegistry::Instance() {
  static OperatorBizBridgeRegistry instance;
  return instance;
}

int OperatorBizBridgeRegistry::CopyToPooledString(const char* src,
                                                  CompanyString* dest,
                                                  uint32_t capacity,
                                                  const char* field_name,
                                                  std::string* err) noexcept {
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

bool OperatorBizBridgeRegistry::RegisterBridge(
    OperatorBizBridgeDescriptor desc) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (audited_) {
    return false;
  }

  int32_t key = static_cast<int32_t>(desc.biz_type);
  if (key == 0 || desc.biz_name.empty()) {
    has_conflict_ = true;
    return false;
  }
  auto it = bridges_by_biz_type_.find(key);
  if (it != bridges_by_biz_type_.end()) {
    if (it->second.registration_identity == desc.registration_identity &&
        it->second == desc) {
      return true;
    }
    has_conflict_ = true;
    return false;
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

const OperatorBizBridgeDescriptor* OperatorBizBridgeRegistry::GetBridge(
    CompanyAlgBizType biz_type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = bridges_by_biz_type_.find(static_cast<int32_t>(biz_type));
  if (it != bridges_by_biz_type_.end()) {
    return &it->second;
  }
  return nullptr;
}

int OperatorBizBridgeRegistry::GlobalInit() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (has_conflict_) {
    return -6;
  }
  if (audited_) {
    return 0;
  }

  // 校验全部 7 类核心业务均已就地自注册到位
  for (int biz_id = 1; biz_id <= 7; ++biz_id) {
    if (bridges_by_biz_type_.find(biz_id) == bridges_by_biz_type_.end()) {
      has_conflict_ = true;
      return -6;
    }
  }

  // 全面原子审计：BizType, Adapter, 内部 DTO
  // 类型，业务名一致性，槽位规范后缀与生命周期函数
  for (const auto& [biz_type, desc] : bridges_by_biz_type_) {
    auto adapter = BizAdapterRegistry::Instance().GetAdapter(
        static_cast<CompanyAlgBizType>(biz_type));
    if (!adapter) {
      has_conflict_ = true;
      return -6;
    }
    if (adapter->BizType() != static_cast<CompanyAlgBizType>(biz_type)) {
      has_conflict_ = true;
      return -6;
    }
    const auto& adapter_desc = adapter->GetDescriptor();
    if (desc.internal_input_type_name != adapter_desc.input_type_name) {
      has_conflict_ = true;
      return -6;
    }
    if (desc.internal_output_type_name != adapter_desc.output_type_name) {
      has_conflict_ = true;
      return -6;
    }
    // 校验业务名匹配 adapter->BizName() 或 pipeline business_name
    bool biz_name_matched = (desc.biz_name == adapter->BizName());
    if (!biz_name_matched) {
      for (const auto& p : adapter_desc.pipelines) {
        if (p.biz_name == desc.biz_name) {
          biz_name_matched = true;
          break;
        }
      }
    }
    if (!biz_name_matched) {
      has_conflict_ = true;
      return -6;
    }

    for (const auto& slot : desc.input_slots) {
      if (slot.direction != IoDirection::kInput) {
        has_conflict_ = true;
        return -6;
      }
      const auto* binding =
          OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
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
          OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
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

}  // namespace alg_framework
