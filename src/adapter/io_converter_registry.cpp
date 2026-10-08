#include "adapter/io_converter_registry.h"

#include <algorithm>
#include <set>

#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/config_schema_validation.h"

namespace llm_edgeflow {
namespace {

// 登记的公共结构检查，输入与输出共用。返回空串表示通过。
template <typename Definition>
std::string CheckStructure(const Definition& def, const char* kind,
                           bool has_callback, const char* callback) {
  if (def.type.empty() || def.name.empty()) {
    return std::string("Empty type or name in ") + kind +
           "ConverterDefinition (type='" + def.type + "', name='" + def.name +
           "')";
  }
  if (!has_callback) {
    return std::string("Missing ") + callback + " in " + kind +
           "ConverterDefinition for: " + def.Label();
  }
  if (def.slot.type_id.empty() || def.slot.type_suffix.empty()) {
    return std::string("Invalid slot (empty type_id or type_suffix) in ") +
           kind + "ConverterDefinition for: " + def.Label();
  }
  if (def.logical_ports.empty()) {
    return std::string("Empty logical_ports in ") + kind +
           "ConverterDefinition for: " + def.Label();
  }
  for (const auto& port : def.logical_ports) {
    if (port.logical_name.empty() || port.type_id.empty()) {
      return std::string(
                 "Invalid logical_port (empty logical_name or type_id) in ") +
             kind + "ConverterDefinition for: " + def.Label();
    }
  }
  return {};
}

template <typename Map>
std::vector<std::string> NamesOfType(const Map& map, const std::string& type) {
  std::vector<std::string> names;
  for (const auto& [key, def] : map) {
    if (key.first == type) names.push_back(key.second);
  }
  return names;  // std::map 已按字典序
}

template <typename Map>
std::vector<std::string> TypesOf(const Map& map) {
  std::vector<std::string> types;
  for (const auto& [key, def] : map) {
    if (types.empty() || types.back() != key.first) types.push_back(key.first);
  }
  return types;
}

template <typename Map>
bool ServiceTypeTaken(const Map& map, const std::string& type,
                      const std::optional<int32_t>& service_type) {
  if (!service_type.has_value()) return false;
  for (const auto& [key, def] : map) {
    if (key.first == type && def.service_type == service_type) return true;
  }
  return false;
}

// 检查 service_type 与平台结构登记的成员声明是否一致。
void CheckServiceType(const std::string& label, const std::string& name,
                      const std::optional<int32_t>& service_type,
                      bool struct_has_service_type,
                      std::vector<std::string>* errors) {
  const bool common = name == kCommonIoName;
  if (struct_has_service_type && !common && !service_type.has_value()) {
    errors->push_back("Converter '" + label +
                      "' must declare service_type: its host struct has a "
                      "service_type member and the registration is not '" +
                      std::string(kCommonIoName) + "'");
  } else if ((!struct_has_service_type || common) && service_type.has_value()) {
    errors->push_back("Converter '" + label +
                      "' must not declare service_type: " +
                      (common ? "'common' does not check service_type"
                              : "its host struct has no service_type member"));
  }
}

void CheckPorts(const std::string& label,
                const std::vector<NodePortDefinition>& ports,
                std::vector<std::string>* errors) {
  if (ports.empty()) {
    errors->push_back("Converter '" + label + "' declares no logical ports");
  }
  std::set<std::string> names;
  for (const auto& port : ports) {
    if (port.logical_name.empty() || port.type_id.empty()) {
      errors->push_back("Converter '" + label +
                        "' has a logical port without name or type");
    } else if (!names.insert(port.logical_name).second) {
      errors->push_back("Converter '" + label +
                        "' declares logical port twice: " + port.logical_name);
    }
  }
}

void CheckParams(const std::string& label, const ParameterSet& params,
                 std::vector<std::string>* errors) {
  std::string error;
  if (!ValidateConfigFieldDefinitions(params.Fields(), &error)) {
    errors->push_back("Converter '" + label +
                      "' has invalid parameter declarations: " + error);
  }
}

// 每个字符串字段都要有 <field>_max_bytes 整数参数，默认值在 1 到平台上限之间。
void CheckSizeParams(const std::string& label, const ParameterSet& params,
                     const OperatorValueTypeBinding& binding,
                     std::vector<std::string>* errors) {
  std::vector<std::string> fields;
  for (const auto& [field, config] :
       binding.output_layout.string_capacity_fields) {
    fields.push_back(field);
  }
  std::sort(fields.begin(), fields.end());
  for (const auto& field : fields) {
    const std::string name = field + "_max_bytes";
    const auto& max =
        binding.output_layout.string_capacity_fields.at(field).max_capacity;
    const auto it = std::find_if(
        params.Fields().begin(), params.Fields().end(),
        [&](const ConfigFieldDefinition& f) { return f.name == name; });
    if (it == params.Fields().end() || it->kind != ConfigValueKind::kInteger) {
      errors->push_back("Converter '" + label + "' lacks integer parameter '" +
                        name + "' for output string field '" + field + "'");
      continue;
    }
    if (!it->default_value.is_number_integer()) {
      errors->push_back("Converter '" + label + "' parameter '" + name +
                        "' must declare an integer default");
      continue;
    }
    const auto value = it->default_value.get<int64_t>();
    if (value < 1 || value > static_cast<int64_t>(max)) {
      errors->push_back("Converter '" + label + "' parameter '" + name +
                        "' default " + std::to_string(value) +
                        " is outside [1, " + std::to_string(max) +
                        "] (platform limit)");
    }
  }
}

}  // namespace

IoConverterRegistry& IoConverterRegistry::Instance() {
  static IoConverterRegistry instance;
  return instance;
}

bool IoConverterRegistry::RegisterInputConverter(
    const InputConverterDefinition& def) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto problem =
      CheckStructure(def, "Input", def.decode_fn != nullptr, "decode_fn");
  if (!problem.empty()) {
    conflicts_.Record(problem);
    return false;
  }
  const Key key{def.type, def.name};
  if (input_converters_.count(key)) {
    conflicts_.Record("Duplicate InputConverter registration: " + def.Label());
    return false;
  }
  if (ServiceTypeTaken(input_converters_, def.type, def.service_type)) {
    conflicts_.Record("Duplicate service_type " +
                      std::to_string(*def.service_type) +
                      " for InputConverter type: " + def.type);
    return false;
  }
  input_converters_.emplace(key, def);
  return true;
}

bool IoConverterRegistry::RegisterOutputConverter(
    const OutputConverterDefinition& def) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto problem =
      CheckStructure(def, "Output", def.encode_fn != nullptr, "encode_fn");
  if (!problem.empty()) {
    conflicts_.Record(problem);
    return false;
  }
  const Key key{def.type, def.name};
  if (output_converters_.count(key)) {
    conflicts_.Record("Duplicate OutputConverter registration: " + def.Label());
    return false;
  }
  if (ServiceTypeTaken(output_converters_, def.type, def.service_type)) {
    conflicts_.Record("Duplicate service_type " +
                      std::to_string(*def.service_type) +
                      " for OutputConverter type: " + def.type);
    return false;
  }
  output_converters_.emplace(key, def);
  return true;
}

const InputConverterDefinition* IoConverterRegistry::FindInputConverter(
    const std::string& type, const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = input_converters_.find({type, name});
  return it == input_converters_.end() ? nullptr : &it->second;
}

const OutputConverterDefinition* IoConverterRegistry::FindOutputConverter(
    const std::string& type, const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = output_converters_.find({type, name});
  return it == output_converters_.end() ? nullptr : &it->second;
}

std::vector<std::string> IoConverterRegistry::InputNamesOfType(
    const std::string& type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return NamesOfType(input_converters_, type);
}

std::vector<std::string> IoConverterRegistry::OutputNamesOfType(
    const std::string& type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return NamesOfType(output_converters_, type);
}

std::vector<std::string> IoConverterRegistry::InputTypes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return TypesOf(input_converters_);
}

std::vector<std::string> IoConverterRegistry::OutputTypes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return TypesOf(output_converters_);
}

std::vector<InputConverterDefinition> IoConverterRegistry::AllInputConverters()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<InputConverterDefinition> result;
  result.reserve(input_converters_.size());
  for (const auto& [_, def] : input_converters_) result.push_back(def);
  return result;
}

std::vector<OutputConverterDefinition>
IoConverterRegistry::AllOutputConverters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<OutputConverterDefinition> result;
  result.reserve(output_converters_.size());
  for (const auto& [_, def] : output_converters_) result.push_back(def);
  return result;
}

bool IoConverterRegistry::HasConflict() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return conflicts_.HasConflict();
}

std::vector<std::string> IoConverterRegistry::GetConflictErrors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return conflicts_.Messages();
}

bool IoConverterRegistry::Audit(std::vector<std::string>* out_errors) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> errors = conflicts_.Messages();
  const auto& values = OperatorValueTypeRegistry::Instance();

  for (const auto& [key, def] : input_converters_) {
    const std::string label = def.Label();
    if (def.slot.type_suffix != def.type) {
      errors.push_back("Converter '" + label + "' slot type_suffix '" +
                       def.slot.type_suffix + "' differs from its type");
    }
    const auto* binding = values.GetBindingBySuffix(def.slot.type_suffix);
    if (!binding || binding->direction != IoDirection::kInput) {
      errors.push_back("Converter '" + label +
                       "' input slot uses unregistered ValueType suffix: " +
                       def.slot.type_suffix);
    } else if (binding->external_c_type_name != def.slot.type_id) {
      errors.push_back("Converter '" + label + "' input slot declares " +
                       def.slot.type_id + " but ValueType suffix '" +
                       def.slot.type_suffix + "' is registered for " +
                       binding->external_c_type_name);
    } else {
      if (!binding->validate_external) {
        errors.push_back("Converter '" + label + "' input slot ValueType '" +
                         def.slot.type_suffix + "' missing validate_external");
      }
      CheckServiceType(label, def.name, def.service_type,
                       static_cast<bool>(binding->read_service_type), &errors);
    }
    CheckParams(label, def.params, &errors);
    CheckPorts(label, def.logical_ports, &errors);
  }

  for (const auto& [key, def] : output_converters_) {
    const std::string label = def.Label();
    if (def.slot.type_suffix != def.type) {
      errors.push_back("Converter '" + label + "' slot type_suffix '" +
                       def.slot.type_suffix + "' differs from its type");
    }
    const auto* binding =
        values.GetOutputBinding(def.slot.type_suffix, def.slot.allocator);
    if (!binding) {
      errors.push_back(
          "Converter '" + label +
          "' output slot has no registered ValueType for suffix '" +
          def.slot.type_suffix + "' and allocator '" + def.slot.allocator +
          "'");
    } else if (binding->external_c_type_name != def.slot.type_id) {
      errors.push_back("Converter '" + label + "' output slot declares " +
                       def.slot.type_id + " but ValueType suffix '" +
                       def.slot.type_suffix + "' is registered for " +
                       binding->external_c_type_name);
    } else {
      // 布局参数能被所选布局解析；元数据声明不超过结构体上限。
      std::shared_ptr<const OutputAllocationParameters> normalized;
      std::string error;
      if (!NormalizeOutputParameters(*binding, def.slot.AllocatorParamsText(),
                                     &normalized, &error)) {
        errors.push_back("Converter '" + label +
                         "' allocator_params rejected by allocator: " + error);
      }
      ResolvedOutputPoolSpec probe;
      probe.type = def.slot.type_suffix;
      probe.allocator = def.slot.allocator;
      probe.params = normalized;
      probe.meta_num = def.slot.metadata_count;
      probe.metadata_type_id = def.slot.metadata_type_id;
      for (const auto& [field, config] :
           binding->output_layout.string_capacity_fields) {
        probe.capacities[field] = 1;
      }
      ResolvedOutputPoolSpec resolved;
      if (!ResolveOutputPoolSpec(*binding, probe, &resolved, &error)) {
        errors.push_back("Converter '" + label +
                         "' output slot declaration is invalid: " + error);
      }
      CheckSizeParams(label, def.params, *binding, &errors);
      CheckServiceType(label, def.name, def.service_type,
                       static_cast<bool>(binding->write_service_type), &errors);
    }
    CheckParams(label, def.params, &errors);
    CheckPorts(label, def.logical_ports, &errors);
  }

  if (out_errors) *out_errors = errors;
  return errors.empty();
}

void IoConverterRegistry::ClearForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  input_converters_.clear();
  output_converters_.clear();
  conflicts_.Clear();
}

void IoConverterRegistry::ResetConflictForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  conflicts_.Clear();
}

}  // namespace llm_edgeflow
