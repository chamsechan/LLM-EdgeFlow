#include "adapter/io_binding_registry.h"

#include <algorithm>
#include <tuple>

#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

bool SameExternalSlots(const std::vector<ExternalSlotDefinition>& left,
                       const std::vector<ExternalSlotDefinition>& right) {
  const auto signature = [](const auto& slots) {
    using Slot =
        std::tuple<std::string, std::string, std::string, PortDirection, bool,
                   std::string, std::vector<std::string>>;
    std::vector<Slot> result;
    for (const auto& slot : slots) {
      auto capacities = slot.capacity_fields;
      std::sort(capacities.begin(), capacities.end());
      result.emplace_back(slot.KeySuffix(), slot.type_id, slot.type_suffix,
                          slot.direction, slot.required, slot.value_type,
                          std::move(capacities));
    }
    std::sort(result.begin(), result.end());
    return result;
  };
  return signature(left) == signature(right);
}

template <typename Converter>
bool SameExternalContract(const Converter& left, const Converter& right) {
  return left.schema_id == right.schema_id &&
         left.schema_version == right.schema_version &&
         left.external_type == right.external_type &&
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
    if (first_input &&
        (!SameExternalContract(*first_input, *input) ||
         !SameExternalContract(*first_output, *output) ||
         first_output->cardinality != output->cardinality ||
         first_output->capacity_policy != output->capacity_policy)) {
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

IoBindingRegistry& IoBindingRegistry::Instance() {
  static IoBindingRegistry instance;
  return instance;
}

bool IoBindingRegistry::RegisterBinding(const IoBindingDefinition& def) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (def.binding_id.empty()) {
    conflict_errors_.push_back("Empty binding_id in IoBindingDefinition");
    return false;
  }
  if (def.biz_name.empty()) {
    conflict_errors_.push_back("Empty biz_name in IoBindingDefinition: " +
                               def.binding_id);
    return false;
  }

  if (def.input_converter_id.empty()) {
    conflict_errors_.push_back(
        "Empty input_converter_id in IoBindingDefinition: " + def.binding_id);
    return false;
  }
  if (def.output_converter_id.empty()) {
    conflict_errors_.push_back(
        "Empty output_converter_id in IoBindingDefinition: " + def.binding_id);
    return false;
  }

  auto it = bindings_.find(def.binding_id);
  if (it != bindings_.end()) {
    conflict_errors_.push_back("Duplicate IoBinding registration: " +
                               def.binding_id);
    return false;
  }

  bindings_[def.binding_id] = def;
  return true;
}

bool IoBindingRegistry::RegisterExposure(const BizExposureDefinition& def) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (def.biz_name.empty()) {
    conflict_errors_.push_back("Empty biz_name in BizExposureDefinition");
    return false;
  }

  auto it = exposures_.find(def.biz_name);
  if (it != exposures_.end()) {
    conflict_errors_.push_back("Duplicate BizExposure registration: " +
                               def.biz_name);
    return false;
  }

  exposures_[def.biz_name] = def;
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

const BizExposureDefinition* IoBindingRegistry::FindExposure(
    const std::string& biz_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = exposures_.find(biz_name);
  if (it != exposures_.end()) {
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

std::vector<BizExposureDefinition> IoBindingRegistry::AllExposures() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<BizExposureDefinition> result;
  result.reserve(exposures_.size());
  for (const auto& [_, def] : exposures_) {
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
  return !conflict_errors_.empty();
}

std::vector<std::string> IoBindingRegistry::GetConflictErrors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return conflict_errors_;
}

bool IoBindingRegistry::Audit(std::vector<std::string>* out_errors) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> errors = conflict_errors_;

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

    // 2. 检查 input converter
    const auto* in_conv =
        conv_reg.FindInputConverter(binding.input_converter_id);
    if (!in_conv) {
      errors.push_back("Binding '" + binding_id +
                       "' references unregistered input_converter: " +
                       binding.input_converter_id);
    } else {
      if (in_conv->max_batch_size == 0) {
        errors.push_back(
            "Binding '" + binding_id +
            "' references input converter with max_batch_size 0: " +
            binding.input_converter_id);
      }
      for (const auto& [logical_name, target_key] : binding.input_ports) {
        auto port_it = std::find_if(
            in_conv->logical_ports.begin(), in_conv->logical_ports.end(),
            [&](const auto& p) { return p.logical_name == logical_name; });
        if (port_it == in_conv->logical_ports.end()) {
          errors.push_back(
              "Binding '" + binding_id +
              "' maps unadvertised input logical port: " + logical_name);
        } else if (biz_def) {
          auto ingress_it = std::find_if(
              biz_def->ingress.begin(), biz_def->ingress.end(),
              [&](const auto& p) { return p.blackboard_key == target_key; });
          if (ingress_it == biz_def->ingress.end()) {
            errors.push_back(
                "Binding '" + binding_id + "' input port '" + logical_name +
                "' maps to non-existent biz ingress key: " + target_key);
          } else if (port_it->type_id != ingress_it->type_id) {
            errors.push_back("Binding '" + binding_id + "' input port '" +
                             logical_name + "' type '" + port_it->type_id +
                             "' does not match biz ingress key '" + target_key +
                             "' type '" + ingress_it->type_id + "'");
          }
        }
      }

      if (biz_def) {
        for (const auto& ingress_port : biz_def->ingress) {
          if (!ingress_port.required) continue;
          bool covered =
              std::any_of(binding.input_ports.begin(),
                          binding.input_ports.end(), [&](const auto& kv) {
                            return kv.second == ingress_port.blackboard_key;
                          });
          if (!covered) {
            errors.push_back("Binding '" + binding_id +
                             "' missing required biz ingress port: " +
                             ingress_port.blackboard_key);
          }
        }
      }

      for (const auto& port : in_conv->logical_ports) {
        if (!port.required) continue;
        if (binding.input_ports.find(port.logical_name) ==
            binding.input_ports.end()) {
          errors.push_back("Binding '" + binding_id +
                           "' missing required input converter logical port "
                           "mapping: " +
                           port.logical_name);
        }
      }
    }

    // 3. 检查 output converter
    const auto* out_conv =
        conv_reg.FindOutputConverter(binding.output_converter_id);
    if (!out_conv) {
      errors.push_back("Binding '" + binding_id +
                       "' references unregistered output_converter: " +
                       binding.output_converter_id);
    } else {
      if (out_conv->max_batch_size == 0) {
        errors.push_back(
            "Binding '" + binding_id +
            "' references output converter with max_batch_size 0: " +
            binding.output_converter_id);
      }
      for (const auto& [logical_name, target_key] : binding.output_ports) {
        auto port_it = std::find_if(
            out_conv->logical_ports.begin(), out_conv->logical_ports.end(),
            [&](const auto& p) { return p.logical_name == logical_name; });
        if (port_it == out_conv->logical_ports.end()) {
          errors.push_back(
              "Binding '" + binding_id +
              "' maps unadvertised output logical port: " + logical_name);
        } else if (biz_def) {
          auto egress_it = std::find_if(
              biz_def->egress.begin(), biz_def->egress.end(),
              [&](const auto& p) { return p.blackboard_key == target_key; });
          if (egress_it != biz_def->egress.end()) {
            if (port_it->type_id != egress_it->type_id) {
              errors.push_back("Binding '" + binding_id + "' output port '" +
                               logical_name + "' type '" + port_it->type_id +
                               "' does not match biz egress key '" +
                               target_key + "' type '" + egress_it->type_id +
                               "'");
            }
          } else {
            auto ingress_it = std::find_if(
                biz_def->ingress.begin(), biz_def->ingress.end(),
                [&](const auto& p) { return p.blackboard_key == target_key; });
            if (ingress_it != biz_def->ingress.end()) {
              if (port_it->type_id != ingress_it->type_id) {
                errors.push_back("Binding '" + binding_id + "' output port '" +
                                 logical_name + "' type '" + port_it->type_id +
                                 "' does not match biz ingress key '" +
                                 target_key + "' type '" + ingress_it->type_id +
                                 "'");
              }
            } else {
              errors.push_back("Binding '" + binding_id + "' output port '" +
                               logical_name +
                               "' maps to non-existent biz key: " + target_key);
            }
          }
        }
      }

      if (biz_def) {
        for (const auto& egress_port : biz_def->egress) {
          if (!egress_port.required) continue;
          bool covered =
              std::any_of(binding.output_ports.begin(),
                          binding.output_ports.end(), [&](const auto& kv) {
                            return kv.second == egress_port.blackboard_key;
                          });
          if (!covered) {
            errors.push_back("Binding '" + binding_id +
                             "' missing required biz egress port: " +
                             egress_port.blackboard_key);
          }
        }
      }

      for (const auto& port : out_conv->logical_ports) {
        if (!port.required) continue;
        if (binding.output_ports.find(port.logical_name) ==
            binding.output_ports.end()) {
          errors.push_back("Binding '" + binding_id +
                           "' missing required output converter logical port "
                           "mapping: " +
                           port.logical_name);
        }
      }
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

  // 4. 检查生产曝光集合是否都有可用绑定
  for (const auto& [biz_name, exposure] : exposures_) {
    bool found = false;
    for (const auto& [_, binding] : bindings_) {
      if (binding.biz_name == biz_name) {
        found = true;
        break;
      }
    }
    if (!found) {
      errors.push_back("Production exposure for biz '" + biz_name +
                       "' lacks a valid Operator binding");
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
  exposures_.clear();
  conflict_errors_.clear();
}

void IoBindingRegistry::ResetConflictForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  conflict_errors_.clear();
}

}  // namespace llm_edgeflow
