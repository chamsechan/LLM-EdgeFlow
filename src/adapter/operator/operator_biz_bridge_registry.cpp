#include "adapter/operator/operator_biz_bridge_registry.h"

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
  // 当前 conf 只有一个 data.mem_que，因而每个业务只能声明一个输出池。
  if (desc.output_slots.size() != 1) {
    has_conflict_ = true;
    return false;
  }
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

  // 以实际 Adapter 注册快照为完整性事实源，新业务无需维护中央 ID 范围。
  const auto adapters = BizAdapterRegistry::Instance().GetAdaptersSnapshot();
  if (adapters.empty()) {
    has_conflict_ = true;
    return -6;
  }

  std::unordered_set<int32_t> adapter_biz_types;
  adapter_biz_types.reserve(adapters.size());
  for (const auto& adapter : adapters) {
    if (!adapter) {
      has_conflict_ = true;
      return -6;
    }
    const int32_t biz_type = static_cast<int32_t>(adapter->BizType());
    if (biz_type == static_cast<int32_t>(ALG_BIZ_TYPE_UNKNOWN) ||
        !adapter_biz_types.insert(biz_type).second) {
      has_conflict_ = true;
      return -6;
    }

    auto bridge_it = bridges_by_biz_type_.find(biz_type);
    if (bridge_it == bridges_by_biz_type_.end()) {
      has_conflict_ = true;
      return -6;
    }
    const auto& desc = bridge_it->second;
    if (desc.biz_type != adapter->BizType()) {
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
    // 校验业务名匹配 adapter->BizName() 或 pipeline biz_name
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
          binding->direction != IoDirection::kInput ||
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
          binding->direction != IoDirection::kOutput ||
          !binding->output_layout.compute_block_payload_bytes ||
          !binding->allocate_external || !binding->reset_external ||
          !binding->destroy_external) {
        has_conflict_ = true;
        return -6;
      }
    }
  }

  // 反向拒绝没有 Adapter 的孤儿 Bridge。
  for (const auto& [biz_type, desc] : bridges_by_biz_type_) {
    (void)desc;
    if (adapter_biz_types.find(biz_type) == adapter_biz_types.end()) {
      has_conflict_ = true;
      return -6;
    }
  }
  audited_ = true;
  return 0;
}

}  // namespace alg_framework
