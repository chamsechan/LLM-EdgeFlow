#include "adapter/io_binding_registry.h"

#include <algorithm>
#include <tuple>

#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

// 容量字段由 type_suffix 对应的 ValueType 决定，无需单独比较。
bool SameExternalSlots(const std::vector<ExternalSlotDefinition>& left,
                       const std::vector<ExternalSlotDefinition>& right) {
  const auto signature = [](const auto& slots) {
    using Slot =
        std::tuple<std::string, std::string, std::string, PortDirection, bool>;
    std::vector<Slot> result;
    for (const auto& slot : slots) {
      result.emplace_back(slot.KeySuffix(), slot.type_id, slot.type_suffix,
                          slot.direction, slot.required);
    }
    std::sort(result.begin(), result.end());
    return result;
  };
  return signature(left) == signature(right);
}

// 载体与槽位相同的 Converter 仍可能按不同协议解析，须同时比较 schema_id。
template <typename Converter>
bool SameExternalContract(const Converter& left, const Converter& right) {
  return left.schema_id == right.schema_id &&
         SameExternalSlots(left.external_slots, right.external_slots);
}

bool CheckBizContract(std::vector<IoBindingDefinition> bindings,
                      const std::string& biz_name, std::string* error) {
  std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) {
    return a.binding_id < b.binding_id;
  });
  const auto& converters = IoConverterRegistry::Instance();
  const InputConverterDefinition* first_input = nullptr;
  const OutputConverterDefinition* first_output = nullptr;
  std::string first_binding;
  for (const auto& binding : bindings) {
    if (binding.biz_name != biz_name) continue;
    const auto* input =
        converters.FindInputConverter(binding.input_converter_id);
    const auto* output =
        converters.FindOutputConverter(binding.output_converter_id);
    if (!input || !output) {
      if (error)
        *error = "Binding '" + binding.binding_id +
                 "' references an unregistered converter";
      return false;
    }
    if (first_input && (!SameExternalContract(*first_input, *input) ||
                        !SameExternalContract(*first_output, *output))) {
      if (error)
        *error = "Bindings '" + first_binding + "' and '" + binding.binding_id +
                 "' for biz_name '" + biz_name +
                 "' declare different external I/O contracts";
      return false;
    }
    first_input = input;
    first_output = output;
    first_binding = binding.binding_id;
  }
  return true;
}

}  // namespace

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
  if (def.binding_id.empty()) {
    conflicts_.Record("Empty binding_id in IoBindingDefinition");
    return false;
  }
  if (def.biz_name.empty()) {
    conflicts_.Record("Empty biz_name in IoBindingDefinition: " +
                      def.binding_id);
    return false;
  }

  if (def.input_converter_id.empty()) {
    conflicts_.Record("Empty input_converter_id in IoBindingDefinition: " +
                      def.binding_id);
    return false;
  }
  if (def.output_converter_id.empty()) {
    conflicts_.Record("Empty output_converter_id in IoBindingDefinition: " +
                      def.binding_id);
    return false;
  }

  auto it = bindings_.find(def.binding_id);
  if (it != bindings_.end()) {
    conflicts_.Record("Duplicate IoBinding registration: " + def.binding_id);
    return false;
  }

  bindings_[def.binding_id] = def;
  return true;
}

const IoBindingDefinition* IoBindingRegistry::FindBinding(
    const std::string& binding_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = bindings_.find(binding_id);
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

bool IoBindingRegistry::ValidateBizContract(const std::string& biz_name,
                                            std::string* error) const {
  return CheckBizContract(AllBindings(), biz_name, error);
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

  std::vector<IoBindingDefinition> all_bindings;
  std::vector<std::string> biz_names;
  for (const auto& [id, binding] : bindings_) {
    all_bindings.push_back(binding);
    biz_names.push_back(binding.biz_name);
  }
  std::sort(biz_names.begin(), biz_names.end());
  biz_names.erase(std::unique(biz_names.begin(), biz_names.end()),
                  biz_names.end());
  for (const auto& biz_name : biz_names) {
    std::string error;
    if (!CheckBizContract(all_bindings, biz_name, &error))
      errors.push_back(error);
  }

  for (const auto& [binding_id, binding] : bindings_) {
    // 1. 检查 biz_name 是否在 PipelineCatalog 中已注册
    const auto* biz_def = catalog_snapshot.FindBiz(binding.biz_name);
    if (!biz_def) {
      errors.push_back(
          "Binding '" + binding_id +
          "' references unregistered biz_name: " + binding.biz_name);
    }

    // 2. 检查 input converter：逻辑端口名即业务入口键
    const auto* in_conv =
        conv_reg.FindInputConverter(binding.input_converter_id);
    if (!in_conv) {
      errors.push_back("Binding '" + binding_id +
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
          errors.push_back("Binding '" + binding_id + "' input port '" +
                           port.logical_name + "' is not a biz ingress key");
        } else if (port.type_id != ingress_it->type_id) {
          errors.push_back("Binding '" + binding_id + "' input port '" +
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
          errors.push_back("Binding '" + binding_id +
                           "' missing required biz ingress port: " +
                           ingress_port.blackboard_key);
        }
      }
    }

    // 3. 检查 output converter：逻辑端口名即业务出口（或入口）键
    const auto* out_conv =
        conv_reg.FindOutputConverter(binding.output_converter_id);
    if (!out_conv) {
      errors.push_back("Binding '" + binding_id +
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
            errors.push_back("Binding '" + binding_id + "' output port '" +
                             port.logical_name + "' type '" + port.type_id +
                             "' does not match biz egress key '" + target_key +
                             "' type '" + egress_it->type_id + "'");
          }
        } else if (ingress_it != biz_def->ingress.end()) {
          if (port.type_id != ingress_it->type_id) {
            errors.push_back("Binding '" + binding_id + "' output port '" +
                             port.logical_name + "' type '" + port.type_id +
                             "' does not match biz ingress key '" + target_key +
                             "' type '" + ingress_it->type_id + "'");
          }
        } else {
          errors.push_back("Binding '" + binding_id + "' output port '" +
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
          errors.push_back("Binding '" + binding_id +
                           "' missing required biz egress port: " +
                           egress_port.blackboard_key);
        }
      }
    }

    if (in_conv && out_conv &&
        EffectiveMaxBatchSize(binding, *in_conv, *out_conv) == 0) {
      errors.push_back("Binding '" + binding_id +
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
              "Binding '" + binding_id + "' input slot '" + slot.slot_name +
              "' uses unregistered ValueType suffix: " + slot.type_suffix);
        } else if (!val_binding->validate_external) {
          errors.push_back("Binding '" + binding_id + "' input slot '" +
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
              "Binding '" + binding_id + "' output slot '" + slot.slot_name +
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
