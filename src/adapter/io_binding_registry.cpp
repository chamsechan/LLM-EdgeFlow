#include "adapter/io_binding_registry.h"

#include <algorithm>

#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {

std::vector<std::string> EffectiveCapacityFields(
    const ExternalSlotDefinition& slot) {
  if (slot.direction != PortDirection::kOutput) return {};
  const auto* binding = OperatorValueTypeRegistry::Instance().GetOutputBinding(
      slot.type_suffix, "");
  if (!binding) return {};
  std::vector<std::string> fields;
  for (const auto& [name, config] :
       binding->output_layout.string_capacity_fields)
    fields.push_back(name);
  std::sort(fields.begin(), fields.end());
  return fields;
}

size_t EffectiveMaxBatchSize(const IoBindingDefinition& binding,
                             const InputConverterDefinition& input,
                             const OutputConverterDefinition& output) {
  size_t limit = 0;
  for (size_t candidate :
       {binding.max_batch_size, input.max_batch_size, output.max_batch_size}) {
    if (candidate != 0 && (limit == 0 || candidate < limit)) limit = candidate;
  }
  return limit;
}

IoBindingRegistry& IoBindingRegistry::Instance() {
  static IoBindingRegistry instance;
  return instance;
}

bool IoBindingRegistry::RegisterBinding(const IoBindingDefinition& def) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (def.biz_name.empty()) {
    conflicts_.Record("Empty biz_name in IoBindingDefinition");
    return false;
  }
  if (def.input_converter_id.empty()) {
    conflicts_.Record("Empty input_converter_id in IoBindingDefinition: " +
                      def.biz_name);
    return false;
  }
  if (def.output_converter_id.empty()) {
    conflicts_.Record("Empty output_converter_id in IoBindingDefinition: " +
                      def.biz_name);
    return false;
  }
  // 一个业务只有一份外部契约，因此只登记一个 binding。
  if (!bindings_.emplace(def.biz_name, def).second) {
    conflicts_.Record("Duplicate IoBinding for biz_name: " + def.biz_name);
    return false;
  }
  return true;
}

const IoBindingDefinition* IoBindingRegistry::FindBinding(
    const std::string& biz_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = bindings_.find(biz_name);
  if (it != bindings_.end()) {
    return &it->second;
  }
  return nullptr;
}

std::vector<IoBindingDefinition> IoBindingRegistry::AllBindings() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<IoBindingDefinition> result;
  result.reserve(bindings_.size());
  for (const auto& [_, def] : bindings_) {
    result.push_back(def);
  }
  return result;
}

bool IoBindingRegistry::HasConflict() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return conflicts_.HasConflict();
}

std::vector<std::string> IoBindingRegistry::GetConflictErrors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return conflicts_.Messages();
}

bool IoBindingRegistry::Audit(std::vector<std::string>* out_errors) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> errors = conflicts_.Messages();

  const auto& conv_reg = IoConverterRegistry::Instance();
  if (conv_reg.HasConflict()) {
    auto conv_errs = conv_reg.GetConflictErrors();
    errors.insert(errors.end(), conv_errs.begin(), conv_errs.end());
  }

  const auto catalog_snapshot = PipelineCatalog::Snapshot();

  for (const auto& [biz_name, binding] : bindings_) {
    // 1. 检查 biz_name 是否在 PipelineCatalog 中已注册
    const auto* biz_def = catalog_snapshot.FindBiz(biz_name);
    if (!biz_def) {
      errors.push_back("Binding '" + biz_name +
                       "' references unregistered biz_name");
    }

    // 2. 检查 input converter：逻辑端口名即业务入口键
    const auto* in_conv =
        conv_reg.FindInputConverter(binding.input_converter_id);
    if (!in_conv) {
      errors.push_back("Binding '" + biz_name +
                       "' references unregistered input_converter: " +
                       binding.input_converter_id);
    } else if (biz_def) {
      for (const auto& port : in_conv->logical_ports) {
        const auto& target_key = port.logical_name;
        auto ingress_it =
            std::find_if(biz_def->ingress.begin(), biz_def->ingress.end(),
                         [&target_key](const auto& p) {
                           return p.blackboard_key == target_key;
                         });
        if (ingress_it == biz_def->ingress.end()) {
          errors.push_back("Binding '" + biz_name + "' input port '" +
                           port.logical_name + "' is not a biz ingress key");
        } else if (port.type_id != ingress_it->type_id) {
          errors.push_back("Binding '" + biz_name + "' input port '" +
                           port.logical_name + "' type '" + port.type_id +
                           "' does not match biz ingress key '" + target_key +
                           "' type '" + ingress_it->type_id + "'");
        }
      }

      for (const auto& ingress_port : biz_def->ingress) {
        if (!ingress_port.required) continue;
        const bool covered = std::any_of(
            in_conv->logical_ports.begin(), in_conv->logical_ports.end(),
            [&ingress_port](const auto& port) {
              return port.logical_name == ingress_port.blackboard_key;
            });
        if (!covered) {
          errors.push_back("Binding '" + biz_name +
                           "' missing required biz ingress port: " +
                           ingress_port.blackboard_key);
        }
      }
    }

    // 3. 检查 output converter：逻辑端口名即业务出口（或入口）键
    const auto* out_conv =
        conv_reg.FindOutputConverter(binding.output_converter_id);
    if (!out_conv) {
      errors.push_back("Binding '" + biz_name +
                       "' references unregistered output_converter: " +
                       binding.output_converter_id);
    } else if (biz_def) {
      for (const auto& port : out_conv->logical_ports) {
        const auto& target_key = port.logical_name;
        const auto matches_key = [&target_key](const auto& p) {
          return p.blackboard_key == target_key;
        };
        auto egress_it = std::find_if(biz_def->egress.begin(),
                                      biz_def->egress.end(), matches_key);
        auto ingress_it = std::find_if(biz_def->ingress.begin(),
                                       biz_def->ingress.end(), matches_key);
        if (egress_it != biz_def->egress.end()) {
          if (port.type_id != egress_it->type_id) {
            errors.push_back("Binding '" + biz_name + "' output port '" +
                             port.logical_name + "' type '" + port.type_id +
                             "' does not match biz egress key '" + target_key +
                             "' type '" + egress_it->type_id + "'");
          }
        } else if (ingress_it != biz_def->ingress.end()) {
          if (port.type_id != ingress_it->type_id) {
            errors.push_back("Binding '" + biz_name + "' output port '" +
                             port.logical_name + "' type '" + port.type_id +
                             "' does not match biz ingress key '" + target_key +
                             "' type '" + ingress_it->type_id + "'");
          }
        } else {
          errors.push_back("Binding '" + biz_name + "' output port '" +
                           port.logical_name +
                           "' is not a biz egress or ingress key");
        }
      }

      for (const auto& egress_port : biz_def->egress) {
        if (!egress_port.required) continue;
        const bool covered = std::any_of(
            out_conv->logical_ports.begin(), out_conv->logical_ports.end(),
            [&egress_port](const auto& port) {
              return port.logical_name == egress_port.blackboard_key;
            });
        if (!covered) {
          errors.push_back("Binding '" + biz_name +
                           "' missing required biz egress port: " +
                           egress_port.blackboard_key);
        }
      }
    }

    if (in_conv && out_conv &&
        EffectiveMaxBatchSize(binding, *in_conv, *out_conv) == 0) {
      errors.push_back("Binding '" + biz_name +
                       "' declares no batch limit: set max_batch_size on the "
                       "binding or one of its converters");
    }

    // 4. 检查对应槽位的 ValueType 绑定

    if (in_conv) {
      for (const auto& slot : in_conv->external_slots) {
        if (slot.direction != PortDirection::kInput) continue;
        const auto* val_binding =
            OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
                slot.type_suffix);
        if (!val_binding) {
          errors.push_back(
              "Binding '" + biz_name + "' input slot '" + slot.slot_name +
              "' uses unregistered ValueType suffix: " + slot.type_suffix);
        } else if (!val_binding->validate_external) {
          errors.push_back("Binding '" + biz_name + "' input slot '" +
                           slot.slot_name + "' ValueType suffix '" +
                           slot.type_suffix + "' missing validate_external");
        }
      }
    }
    if (out_conv) {
      for (const auto& slot : out_conv->external_slots) {
        if (slot.direction != PortDirection::kOutput) continue;
        const auto* val_binding =
            OperatorValueTypeRegistry::Instance().GetOutputBinding(
                slot.type_suffix, "");
        if (!val_binding) {
          errors.push_back(
              "Binding '" + biz_name + "' output slot '" + slot.slot_name +
              "' uses unregistered ValueType suffix: " + slot.type_suffix);
        }
      }
    }
  }

  if (out_errors) {
    *out_errors = errors;
  }
  return errors.empty();
}

void IoBindingRegistry::ClearForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  bindings_.clear();
  conflicts_.Clear();
}

void IoBindingRegistry::ResetConflictForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  conflicts_.Clear();
}

}  // namespace llm_edgeflow
