#include "adapter/operator/operator_biz_bridge_registry.h"

#include <cstring>
#include <exception>
#include <unordered_set>

#include "adapter/biz_adapter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/diagnostic.h"

namespace llm_edgeflow {

bool RegisterOperatorBizBridge(OperatorBizBridgeDescriptor descriptor) {
  return OperatorBizBridgeRegistry::Instance().RegisterBridge(
      std::move(descriptor));
}

int CopyToOperatorString(const char* source, CompanyString* destination,
                         uint32_t capacity, const char* field_name,
                         std::string* diagnostic) noexcept {
  return OperatorBizBridgeRegistry::CopyToPooledString(
      source, destination, capacity, field_name, diagnostic);
}

OperatorBizBridgeRegistry& OperatorBizBridgeRegistry::Instance() {
  static OperatorBizBridgeRegistry instance;
  return instance;
}

int OperatorBizBridgeRegistry::CopyToPooledString(const char* src,
                                                  CompanyString* dest,
                                                  uint32_t capacity,
                                                  const char* field_name,
                                                  std::string* err) noexcept {
  try {
    if (!dest || !dest->data) {
      if (err)
        *err = std::string(field_name ? field_name : "string") +
               " in destination pool block is null";
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
        *err = std::string(field_name ? field_name : "string") +
               " output length (" + std::to_string(len) +
               ") exceeds pool capacity (" + std::to_string(capacity) + ")";
      return -4;
    }
    std::memcpy(dest->data, src, len);
    dest->data[len] = '\0';
    dest->length = static_cast<int32_t>(len);
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
  } catch (...) {
    SetDiagnosticNoexcept(err, "Failed to copy pooled string");
  }
  return -4;
}

bool OperatorBizBridgeRegistry::RegisterBridge(
    OperatorBizBridgeDescriptor desc) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (audited_) {
    return false;
  }
  const auto reject = [&](std::initializer_list<std::string_view> reason) {
    RecordConflict(desc.biz_type, desc.adapter_name, reason);
    return false;
  };

  int32_t key = static_cast<int32_t>(desc.biz_type);
  if (key == 0 || desc.adapter_name.empty()) {
    return reject(
        {"Bridge requires a nonzero BizType and nonempty adapter_name"});
  }
  auto it = bridges_by_biz_type_.find(key);
  if (it != bridges_by_biz_type_.end()) {
    if (it->second.registration_identity == desc.registration_identity &&
        it->second == desc) {
      return true;
    }
    return reject({"Conflicting bridge registration '",
                   desc.registration_identity, "'; already registered by '",
                   it->second.registration_identity, "'"});
  }

  // 校验槽位命名唯一性与方向
  std::unordered_set<std::string> in_names, in_suffixes;
  for (const auto& s : desc.input_slots) {
    if (s.logical_name.empty() || s.type_suffix.empty()) {
      return reject(
          {"Input slot requires nonempty logical_name and type_suffix; slot '",
           s.logical_name, "', type '", s.type_suffix, "'"});
    }
    if (!s.key_suffix.empty() || s.convert_output) {
      return reject(
          {"Input slot '", s.logical_name,
           "' cannot declare output-only key_suffix or convert_output"});
    }
    if (s.direction != IoDirection::kInput) {
      return reject({"Input slot '", s.logical_name, "' (", s.type_suffix,
                     ") must have input direction"});
    }
    if (!in_names.insert(s.logical_name).second ||
        !in_suffixes.insert(s.type_suffix).second) {
      return reject({"Duplicate input slot name or type suffix: '",
                     s.logical_name, "' (", s.type_suffix, ")"});
    }
  }

  std::unordered_set<std::string> out_names, out_suffixes;
  if (desc.output_slots.empty()) {
    return reject({"Bridge must declare at least one output slot"});
  }
  for (const auto& s : desc.output_slots) {
    if (s.logical_name.empty() || s.type_suffix.empty()) {
      return reject(
          {"Output slot requires nonempty logical_name and type_suffix; slot '",
           s.logical_name, "', type '", s.type_suffix, "'"});
    }
    if (s.KeySuffix().find('.') != std::string::npos) {
      return reject(
          {"Output key suffix must not contain a dot: '", s.KeySuffix(), "'"});
    }
    if (s.direction != IoDirection::kOutput) {
      return reject({"Output slot '", s.logical_name, "' (", s.type_suffix,
                     ") must have output direction"});
    }
    if (!out_names.insert(s.logical_name).second ||
        !out_suffixes.insert(s.KeySuffix()).second) {
      return reject({"Duplicate output slot name or key suffix: '",
                     s.logical_name, "' (", s.KeySuffix(), ")"});
    }
    if (!s.convert_output &&
        (desc.output_slots.size() != 1 || !desc.convert_sample_output)) {
      return reject({"Output slot '", s.logical_name,
                     "' requires its own convert_output callback"});
    }
  }

  if (!desc.convert_sample_input || !desc.create_shadow_output_dto) {
    return reject(
        {"Bridge requires convert_sample_input and create_shadow_output_dto "
         "callbacks"});
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

void OperatorBizBridgeRegistry::RecordConflict(
    CompanyAlgBizType biz_type, std::string_view adapter_name,
    std::initializer_list<std::string_view> reason) noexcept {
  if (has_conflict_) return;
  has_conflict_ = true;
  // A diagnostic allocation failure must not change a rejected registration
  // into an exception or erase the first conflict with a later audit failure.
  try {
    conflict_diagnostic_ = "OperatorBizBridgeRegistry: adapter '";
    conflict_diagnostic_.append(adapter_name);
    conflict_diagnostic_ +=
        "' (BizType " + std::to_string(static_cast<int32_t>(biz_type)) + "): ";
    for (const auto part : reason) conflict_diagnostic_.append(part);
  } catch (...) {
    conflict_diagnostic_.clear();
  }
}

int OperatorBizBridgeRegistry::ReportConflict(
    std::string* diagnostic) const noexcept {
  SetDiagnosticNoexcept(
      diagnostic,
      conflict_diagnostic_.empty()
          ? std::string_view(
                "OperatorBizBridgeRegistry registration or audit failed")
          : std::string_view(conflict_diagnostic_));
  return -6;
}

int OperatorBizBridgeRegistry::GlobalInit(std::string* diagnostic) {
  std::lock_guard<std::mutex> lock(mutex_);
  SetDiagnosticNoexcept(diagnostic, "");
  if (has_conflict_) {
    return ReportConflict(diagnostic);
  }
  if (audited_) {
    return 0;
  }
  const auto reject = [&](CompanyAlgBizType biz_type,
                          std::string_view adapter_name,
                          std::initializer_list<std::string_view> reason) {
    RecordConflict(biz_type, adapter_name, reason);
    return ReportConflict(diagnostic);
  };

  // 以实际 Adapter 注册快照为完整性事实源，新业务无需维护中央 ID 范围。
  const auto adapters = BizAdapterRegistry::Instance().GetAdaptersSnapshot();
  if (adapters.empty()) {
    return reject(ALG_BIZ_TYPE_UNKNOWN, "", {"No registered BizAdapters"});
  }

  std::unordered_set<int32_t> adapter_biz_types;
  adapter_biz_types.reserve(adapters.size());
  for (const auto& adapter : adapters) {
    if (!adapter) {
      return reject(ALG_BIZ_TYPE_UNKNOWN, "", {"Null registered BizAdapter"});
    }
    const int32_t biz_type = static_cast<int32_t>(adapter->BizType());
    if (biz_type == static_cast<int32_t>(ALG_BIZ_TYPE_UNKNOWN) ||
        !adapter_biz_types.insert(biz_type).second) {
      return reject(adapter->BizType(), adapter->AdapterName(),
                    {"BizAdapter has an unknown or duplicate BizType"});
    }

    auto bridge_it = bridges_by_biz_type_.find(biz_type);
    if (bridge_it == bridges_by_biz_type_.end()) {
      return reject(adapter->BizType(), adapter->AdapterName(),
                    {"Missing Operator bridge for registered BizAdapter"});
    }
    const auto& desc = bridge_it->second;
    if (desc.biz_type != adapter->BizType()) {
      return reject(adapter->BizType(), adapter->AdapterName(),
                    {"Bridge BizType does not match its BizAdapter"});
    }

    const auto& adapter_desc = adapter->GetDescriptor();
    if (desc.internal_input_type_name != adapter_desc.input_type_name) {
      return reject(desc.biz_type, desc.adapter_name,
                    {"Internal input type '", desc.internal_input_type_name,
                     "' does not match BizAdapter type '",
                     adapter_desc.input_type_name, "'"});
    }
    if (desc.internal_output_type_name != adapter->ResultTypeName()) {
      return reject(desc.biz_type, desc.adapter_name,
                    {"Internal output type '", desc.internal_output_type_name,
                     "' does not match BizAdapter result '",
                     adapter->ResultTypeName(), "'"});
    }
    // Adapter 标识匹配；兼容旧 bridge 使用已声明的 Pipeline biz_name。
    bool adapter_name_matched = (desc.adapter_name == adapter->AdapterName());
    if (!adapter_name_matched) {
      for (const auto& p : adapter_desc.biz_definitions) {
        if (p.biz_name == desc.adapter_name) {
          adapter_name_matched = true;
          break;
        }
      }
    }
    if (!adapter_name_matched) {
      return reject(
          desc.biz_type, desc.adapter_name,
          {"Bridge adapter_name does not match BizAdapter '",
           adapter->AdapterName(), "' or any of its Pipeline biz_names"});
    }

    for (const auto& slot : desc.input_slots) {
      if (slot.direction != IoDirection::kInput) {
        return reject(
            desc.biz_type, desc.adapter_name,
            {"Input slot '", slot.logical_name, "' must have input direction"});
      }
      const auto* binding =
          OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
              slot.type_suffix);
      if (!binding || binding->canonical_suffix != slot.type_suffix ||
          binding->direction != IoDirection::kInput ||
          !binding->validate_external) {
        return reject(desc.biz_type, desc.adapter_name,
                      {"Input slot '", slot.logical_name,
                       "' requires canonical value type '", slot.type_suffix,
                       "' with input direction and validate_external"});
      }
    }
    for (const auto& slot : desc.output_slots) {
      if (slot.direction != IoDirection::kOutput) {
        return reject(desc.biz_type, desc.adapter_name,
                      {"Output slot '", slot.logical_name,
                       "' must have output direction"});
      }
      const auto* binding =
          OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
              slot.type_suffix);
      if (!binding || binding->canonical_suffix != slot.type_suffix ||
          binding->direction != IoDirection::kOutput ||
          !binding->output_layout.compute_block_payload_bytes ||
          !binding->allocate_external || !binding->reset_external ||
          !binding->destroy_external) {
        return reject(desc.biz_type, desc.adapter_name,
                      {"Output slot '", slot.logical_name,
                       "' requires canonical value type '", slot.type_suffix,
                       "' with output direction, layout and "
                       "allocation/reset/destroy callbacks"});
      }
    }
  }

  // 反向拒绝没有 Adapter 的孤儿 Bridge。
  for (const auto& [biz_type, desc] : bridges_by_biz_type_) {
    if (adapter_biz_types.find(biz_type) == adapter_biz_types.end()) {
      return reject(desc.biz_type, desc.adapter_name,
                    {"Bridge has no registered BizAdapter"});
    }
  }
  audited_ = true;
  return 0;
}

}  // namespace llm_edgeflow
