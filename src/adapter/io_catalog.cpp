#include "adapter/io_catalog.h"

#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"

namespace llm_edgeflow {
namespace {
template <typename Definition>
nlohmann::json ConverterJson(const Definition& def,
                             const std::vector<ConfigFieldDefinition>& fields) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix(def.type);
  auto ports = nlohmann::json::array();
  for (const auto& port : def.logical_ports)
    ports.push_back(PipelineCatalog::PortToJson(port.logical_name, port));
  auto parameters = nlohmann::json::array();
  for (const auto& field : fields)
    parameters.push_back(ConfigFieldToJson(field));
  nlohmann::json result = {
      {"type", def.type},
      {"name", def.name},
      {"slot",
       {{"type_id", binding ? binding->external_c_type_name : ""},
        {"type_suffix", def.slot.type_suffix},
        {"required", def.slot.required},
        {"allocator", def.slot.allocator},
        {"allocator_params", def.slot.allocator_params},
        {"metadata_count", def.slot.metadata_count},
        {"metadata_type_id", def.slot.metadata_type_id}}},
      {"logical_ports", std::move(ports)},
      {"config_fields", std::move(parameters)}};
  if (binding) {
    if (auto service = binding->ServiceType(def.name))
      result["service_type"] = *service;
  }
  return result;
}
}  // namespace

nlohmann::json IoCatalog::ToJson(const PipelineCatalogSnapshot& snapshot) {
  auto result = PipelineCatalog::ToJson(snapshot);
  auto inputs = nlohmann::json::array();
  auto outputs = nlohmann::json::array();
  const auto& registry = IoConverterRegistry::Instance();
  for (const auto& def : registry.AllInputConverters())
    inputs.push_back(ConverterJson(def, def.params.Fields()));
  for (const auto& def : registry.AllOutputConverters())
    outputs.push_back(ConverterJson(def, OutputConverterParameterFields(def)));
  result["input_converters"] = std::move(inputs);
  result["output_converters"] = std::move(outputs);
  return result;
}

nlohmann::json IoCatalog::ToJson() {
  return ToJson(PipelineCatalog::Snapshot());
}
}  // namespace llm_edgeflow
