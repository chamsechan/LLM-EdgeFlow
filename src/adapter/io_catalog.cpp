#include "adapter/io_catalog.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/config_schema.h"

namespace llm_edgeflow {
namespace {

nlohmann::json SlotJson(const ExternalSlotDefinition& slot) {
  return {{"type_id", slot.type_id},
          {"type_suffix", slot.type_suffix},
          {"required", slot.required},
          {"allocator", slot.allocator}};
}

// 尺寸参数的 maximum 由框架按平台上限补齐。
std::vector<ConfigFieldDefinition> FieldsWithPlatformLimits(
    const ParameterSet& params, const ExternalSlotDefinition& slot,
    bool output) {
  auto fields = params.Fields();
  if (!output) return fields;
  const auto* binding = OperatorValueTypeRegistry::Instance().GetOutputBinding(
      slot.type_suffix, slot.allocator);
  if (!binding) return fields;
  for (auto& field : fields) {
    for (const auto& [name, config] :
         binding->output_layout.string_capacity_fields) {
      if (field.name == name + "_max_bytes") {
        field.maximum = static_cast<double>(config.max_capacity);
      }
    }
  }
  return fields;
}

template <typename Definition>
nlohmann::json ConverterToJson(const Definition& conv, bool output) {
  nlohmann::json ports = nlohmann::json::array();
  for (const auto& p : conv.logical_ports)
    ports.push_back(PipelineCatalog::PortToJson(p.logical_name, p));
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& field :
       FieldsWithPlatformLimits(conv.params, conv.slot, output)) {
    fields.push_back(ConfigFieldToJson(field));
  }
  nlohmann::json result = {{"type", conv.type},
                           {"name", conv.name},
                           {"external_type", conv.slot.type_id},
                           {"slot", SlotJson(conv.slot)},
                           {"config_fields", std::move(fields)},
                           {"logical_ports", std::move(ports)}};
  if (conv.service_type.has_value()) {
    result["service_type"] = *conv.service_type;
  }
  return result;
}

}  // namespace

nlohmann::json IoCatalog::ToJson(const PipelineCatalogSnapshot& snapshot) {
  nlohmann::json input_converters = nlohmann::json::array();
  for (const auto& c : IoConverterRegistry::Instance().AllInputConverters()) {
    input_converters.push_back(ConverterToJson(c, false));
  }
  nlohmann::json output_converters = nlohmann::json::array();
  for (const auto& c : IoConverterRegistry::Instance().AllOutputConverters()) {
    output_converters.push_back(ConverterToJson(c, true));
  }

  auto result = PipelineCatalog::ToJson(snapshot);
  result["input_converters"] = std::move(input_converters);
  result["output_converters"] = std::move(output_converters);
  return result;
}

nlohmann::json IoCatalog::ToJson() {
  return ToJson(PipelineCatalog::Snapshot());
}

}  // namespace llm_edgeflow
