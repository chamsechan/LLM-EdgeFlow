#include "adapter/io_converter_registry.h"

#include <algorithm>
#include <set>

#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/config_schema_validation.h"

namespace llm_edgeflow {
namespace {

template <typename Definition>
void AuditDefinition(const Definition& def,
                     const OperatorValueTypeBinding* value,
                     IoDirection direction, std::vector<std::string>* errors) {
  const auto fail = [&](const std::string& message) {
    errors->push_back(def.Label() + ": " + message);
  };
  if (def.slot.type_suffix != def.type)
    fail("Slot type_suffix must equal converter type");
  if (!value || value->direction != direction) {
    fail("No registered platform binding for the slot");
  } else {
    if (value->external_c_type_name != def.slot.type_id)
      fail("Slot type_id does not match the platform struct");
    const bool needs_service =
        value->read_service_type && def.name != kCommonIoName;
    if (needs_service != def.service_type.has_value())
      fail(
          "service_type must be set only for a named service on a struct that "
          "declares it");
    if (direction == IoDirection::kInput && !value->validate_external)
      fail("Input platform binding has no validation callback");
    if (direction == IoDirection::kOutput && def.service_type &&
        !value->write_service_type)
      fail("Output platform binding has no service_type writer");
  }
  std::string error;
  if (!ValidateConfigFieldDefinitions(def.params.Fields(), &error)) fail(error);
  if (def.logical_ports.empty()) fail("Logical ports cannot be empty");
  std::set<std::string> names;
  for (const auto& port : def.logical_ports) {
    if (port.logical_name.empty() || port.type_id.empty())
      fail("Logical port name and type must be nonempty");
    if (!names.insert(port.logical_name).second)
      fail("Duplicate logical port: " + port.logical_name);
  }
}

template <typename Map, typename Definition>
bool RegisterDefinition(Map* entries, const Definition& def, bool has_callback,
                        RegistryConflicts* conflicts) {
  if (def.type.empty() || def.name.empty() || !has_callback) {
    conflicts->Record("Invalid converter registration: " + def.Label() +
                      " (type, name and callback are required)");
    return false;
  }
  const auto key = std::make_pair(def.type, def.name);
  if (entries->count(key)) {
    conflicts->Record("Duplicate converter registration: " + def.Label());
    return false;
  }
  if (def.service_type) {
    for (const auto& [registered_key, registered] : *entries) {
      if (registered.type == def.type &&
          registered.service_type == def.service_type) {
        conflicts->Record("Duplicate service_type for converter type: " +
                          def.type);
        return false;
      }
    }
  }
  entries->emplace(key, def);
  return true;
}

}  // namespace

std::vector<ConfigFieldDefinition> OutputConverterParameterFields(
    const OutputConverterDefinition& def) {
  auto fields = def.params.Fields();
  const auto* binding = OperatorValueTypeRegistry::Instance().GetOutputBinding(
      def.type, def.slot.allocator);
  if (!binding) return fields;
  for (auto& parameter : fields) {
    for (const auto& [field, limit] :
         binding->output_layout.string_capacity_fields) {
      if (parameter.name == field + "_max_bytes")
        parameter.maximum =
            std::min(parameter.maximum.value_or(limit.max_capacity),
                     static_cast<double>(limit.max_capacity));
    }
  }
  return fields;
}

IoConverterRegistry& IoConverterRegistry::Instance() {
  static IoConverterRegistry instance;
  return instance;
}

bool IoConverterRegistry::RegisterInputConverter(
    const InputConverterDefinition& def) {
  std::lock_guard<std::mutex> lock(mutex_);
  return RegisterDefinition(&input_converters_, def, def.decode_fn != nullptr,
                            &conflicts_);
}

bool IoConverterRegistry::RegisterOutputConverter(
    const OutputConverterDefinition& def) {
  std::lock_guard<std::mutex> lock(mutex_);
  return RegisterDefinition(&output_converters_, def, def.encode_fn != nullptr,
                            &conflicts_);
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

std::vector<InputConverterDefinition> IoConverterRegistry::AllInputConverters()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<InputConverterDefinition> result;
  for (const auto& [key, def] : input_converters_) result.push_back(def);
  return result;
}

std::vector<OutputConverterDefinition>
IoConverterRegistry::AllOutputConverters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<OutputConverterDefinition> result;
  for (const auto& [key, def] : output_converters_) result.push_back(def);
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
  std::lock_guard<std::mutex> audit_lock(audit_mutex_);
  const auto inputs = AllInputConverters();
  const auto outputs = AllOutputConverters();
  auto errors = GetConflictErrors();
  const auto& values = OperatorValueTypeRegistry::Instance();
  for (const auto& def : inputs) {
    AuditDefinition(def, values.GetBindingBySuffix(def.type),
                    IoDirection::kInput, &errors);
  }
  for (const auto& def : outputs) {
    const auto* value = values.GetOutputBinding(def.type, def.slot.allocator);
    AuditDefinition(def, value, IoDirection::kOutput, &errors);
    if (!value) continue;
    const auto fail = [&](const std::string& message) {
      errors.push_back(def.Label() + ": " + message);
    };
    if (def.slot.metadata_count > value->output_layout.max_metadata_elements ||
        (def.slot.metadata_count == 0) != (def.slot.metadata_type_id == 0) ||
        (def.slot.metadata_count &&
         !FindCompanyAnyType(def.slot.metadata_type_id)))
      fail("Invalid fixed metadata count or type");
    for (const auto& [field, capacity] :
         value->output_layout.string_capacity_fields) {
      const auto name = field + "_max_bytes";
      const auto& fields = def.params.Fields();
      const auto it =
          std::find_if(fields.begin(), fields.end(),
                       [&](const auto& f) { return f.name == name; });
      if (it == fields.end() || it->kind != ConfigValueKind::kInteger ||
          !it->default_value.is_number_integer() || it->default_value < 1 ||
          it->default_value > capacity.max_capacity)
        fail("Missing valid integer size default: " + name);
    }
    // Layout parameters are normalized once per registration, before Create.
    bool cached;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cached = output_parameters_.count({def.type, def.name}) != 0;
    }
    if (!cached) {
      std::shared_ptr<const OutputAllocationParameters> parameters;
      std::string error;
      if (!NormalizeOutputParameters(*value,
                                     def.slot.allocator_params.empty()
                                         ? "{}"
                                         : def.slot.allocator_params,
                                     &parameters, &error)) {
        fail(error);
      } else {
        std::lock_guard<std::mutex> lock(mutex_);
        output_parameters_.emplace(std::make_pair(def.type, def.name),
                                   std::move(parameters));
      }
    }
  }
  if (out_errors) *out_errors = errors;
  return errors.empty();
}

std::shared_ptr<const OutputAllocationParameters>
IoConverterRegistry::OutputParameters(const std::string& type,
                                      const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = output_parameters_.find({type, name});
  if (it == output_parameters_.end())
    throw std::logic_error(
        "Output converter has not passed registration audit");
  return it->second;
}

void IoConverterRegistry::ClearForTesting() {
  std::lock_guard<std::mutex> audit_lock(audit_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  input_converters_.clear();
  output_converters_.clear();
  output_parameters_.clear();
  conflicts_.Clear();
}

void IoConverterRegistry::ResetConflictForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  conflicts_.Clear();
}

}  // namespace llm_edgeflow
