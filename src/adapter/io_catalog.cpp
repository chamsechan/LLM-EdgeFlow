#include "adapter/io_catalog.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "adapter/io_binding_registry.h"
#include "adapter/io_converter_registry.h"

namespace llm_edgeflow {
namespace {

nlohmann::json SlotJson(const ExternalSlotDefinition& slot) {
  return {{"slot_name", slot.slot_name},
          {"type_id", slot.type_id},
          {"type_suffix", slot.type_suffix},
          {"key_suffix", slot.KeySuffix()},
          {"direction",
           slot.direction == PortDirection::kInput ? "input" : "output"},
          {"value_type", slot.value_type},
          {"required", slot.required},
          {"capacity_fields", slot.capacity_fields}};
}

nlohmann::json InputConverterToJson(const InputConverterDefinition& conv) {
  nlohmann::json slots = nlohmann::json::array();
  for (const auto& s : conv.external_slots) slots.push_back(SlotJson(s));
  nlohmann::json ports = nlohmann::json::array();
  for (const auto& p : conv.logical_ports)
    ports.push_back(PipelineCatalog::PortToJson(p.logical_name, p));

  return {{"converter_id", conv.converter_id},
          {"transport", "operator"},
          {"schema_id", conv.schema_id},
          {"schema_version", conv.schema_version},
          {"external_type", conv.external_type},
          {"max_batch_size", conv.max_batch_size},
          {"external_slots", std::move(slots)},
          {"logical_ports", std::move(ports)}};
}

nlohmann::json OutputConverterToJson(const OutputConverterDefinition& conv) {
  nlohmann::json slots = nlohmann::json::array();
  for (const auto& s : conv.external_slots) slots.push_back(SlotJson(s));
  nlohmann::json ports = nlohmann::json::array();
  for (const auto& p : conv.logical_ports)
    ports.push_back(PipelineCatalog::PortToJson(p.logical_name, p));

  return {{"converter_id", conv.converter_id},
          {"transport", "operator"},
          {"schema_id", conv.schema_id},
          {"schema_version", conv.schema_version},
          {"external_type", conv.external_type},
          {"max_batch_size", conv.max_batch_size},
          {"cardinality", conv.cardinality},
          {"capacity_policy", conv.capacity_policy},
          {"external_slots", std::move(slots)},
          {"logical_ports", std::move(ports)}};
}

nlohmann::json IoBindingToJson(const IoBindingDefinition& b) {
  return {{"binding_id", b.binding_id},
          {"biz_name", b.biz_name},
          {"transport", "operator"},
          {"input_converter_id", b.input_converter_id},
          {"output_converter_id", b.output_converter_id},
          {"input_port_mapping", b.input_ports},
          {"output_port_mapping", b.output_ports},
          {"max_batch_size", b.max_batch_size}};
}

}  // namespace

nlohmann::json IoCatalog::ToJson(const PipelineCatalogSnapshot& snapshot,
                                 const std::string& biz_filter) {
  // 聚合 IO Bindings 与 Converters
  auto all_bindings = IoBindingRegistry::Instance().AllBindings();
  std::sort(all_bindings.begin(), all_bindings.end(),
            [](const IoBindingDefinition& a, const IoBindingDefinition& b) {
              return a.binding_id < b.binding_id;
            });

  std::set<std::string> active_input_converters;
  std::set<std::string> active_output_converters;

  nlohmann::json io_bindings = nlohmann::json::array();
  for (const auto& b : all_bindings) {
    if (!biz_filter.empty() && b.biz_name != biz_filter) continue;
    active_input_converters.insert(b.input_converter_id);
    active_output_converters.insert(b.output_converter_id);
    io_bindings.push_back(IoBindingToJson(b));
  }

  auto all_input_converters =
      IoConverterRegistry::Instance().AllInputConverters();
  std::sort(
      all_input_converters.begin(), all_input_converters.end(),
      [](const InputConverterDefinition& a, const InputConverterDefinition& b) {
        return a.converter_id < b.converter_id;
      });

  nlohmann::json input_converters = nlohmann::json::array();
  for (const auto& c : all_input_converters) {
    if (!biz_filter.empty() && active_input_converters.find(c.converter_id) ==
                                   active_input_converters.end()) {
      continue;
    }
    input_converters.push_back(InputConverterToJson(c));
  }

  auto all_output_converters =
      IoConverterRegistry::Instance().AllOutputConverters();
  std::sort(all_output_converters.begin(), all_output_converters.end(),
            [](const OutputConverterDefinition& a,
               const OutputConverterDefinition& b) {
              return a.converter_id < b.converter_id;
            });

  nlohmann::json output_converters = nlohmann::json::array();
  for (const auto& c : all_output_converters) {
    if (!biz_filter.empty() && active_output_converters.find(c.converter_id) ==
                                   active_output_converters.end()) {
      continue;
    }
    output_converters.push_back(OutputConverterToJson(c));
  }

  auto result = PipelineCatalog::ToJson(snapshot, biz_filter);
  result["schema_version"] = 4;
  result["input_converters"] = std::move(input_converters);
  result["output_converters"] = std::move(output_converters);
  result["io_bindings"] = std::move(io_bindings);
  return result;
}

nlohmann::json IoCatalog::ToJson(const std::string& biz_filter) {
  return ToJson(PipelineCatalog::Snapshot(), biz_filter);
}

}  // namespace llm_edgeflow
