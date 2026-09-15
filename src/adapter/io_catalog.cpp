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
          {"direction",
           slot.direction == PortDirection::kInput ? "input" : "output"},
          {"value_type", slot.value_type},
          {"required", slot.required},
          {"capacity_fields", slot.capacity_fields}};
}

nlohmann::json LogicalPortJson(const NodePortDefinition& port) {
  nlohmann::json res = {{"key", port.logical_name},
                        {"type_id", port.type_id},
                        {"required", port.required},
                        {"cardinality", port.cardinality},
                        {"provenance_policy", port.provenance_policy},
                        {"lifetime", port.lifetime}};
  if (!port.lifetime_config_field.empty()) {
    res["lifetime_config_field"] = port.lifetime_config_field;
  }
  return res;
}

nlohmann::json BizPortJson(const BizPortDefinition& port) {
  nlohmann::json res = {{"key", port.blackboard_key},
                        {"type_id", port.type_id},
                        {"required", port.required},
                        {"cardinality", port.cardinality},
                        {"provenance_policy", port.provenance_policy},
                        {"lifetime", port.lifetime}};
  if (!port.lifetime_config_field.empty()) {
    res["lifetime_config_field"] = port.lifetime_config_field;
  }
  return res;
}

nlohmann::json InputConverterToJson(const InputConverterDefinition& conv) {
  nlohmann::json slots = nlohmann::json::array();
  for (const auto& s : conv.external_slots) slots.push_back(SlotJson(s));
  nlohmann::json ports = nlohmann::json::array();
  for (const auto& p : conv.logical_ports) ports.push_back(LogicalPortJson(p));

  return {{"converter_id", conv.converter_id},
          {"transport", conv.transport},
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
  for (const auto& p : conv.logical_ports) ports.push_back(LogicalPortJson(p));

  return {{"converter_id", conv.converter_id},
          {"transport", conv.transport},
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
          {"transport", b.transport},
          {"input_converter_id", b.input_converter_id},
          {"output_converter_id", b.output_converter_id},
          {"input_port_mapping", b.input_ports},
          {"output_port_mapping", b.output_ports},
          {"max_batch_size", b.max_batch_size}};
}

}  // namespace

nlohmann::json IoCatalog::ToJson(const PipelineCatalogSnapshot& snapshot,
                                 const std::string& biz_filter) {
  nlohmann::json nodes = nlohmann::json::array();
  for (const auto& item : snapshot.nodes) {
    if (!biz_filter.empty() && !item.biz_names.empty() &&
        std::find(item.biz_names.begin(), item.biz_names.end(), biz_filter) ==
            item.biz_names.end()) {
      continue;
    }
    nodes.push_back(PipelineCatalog::NodeToJson(item));
  }

  nlohmann::json models = nlohmann::json::array();
  for (const auto& item : PipelineCatalog::Models()) {
    models.push_back(PipelineCatalog::ModelToJson(item));
  }

  nlohmann::json backends = nlohmann::json::array();
  for (const auto& item : PipelineCatalog::Backends()) {
    backends.push_back(PipelineCatalog::BackendToJson(item));
  }

  nlohmann::json bizs = nlohmann::json::array();
  for (const auto& item : snapshot.bizs) {
    if (!biz_filter.empty() && item.biz_name != biz_filter) continue;
    nlohmann::json ingress = nlohmann::json::array();
    nlohmann::json egress = nlohmann::json::array();
    for (const auto& port : item.ingress) ingress.push_back(BizPortJson(port));
    for (const auto& port : item.egress) egress.push_back(BizPortJson(port));
    bizs.push_back({{"biz_name", item.biz_name},
                    {"demo_biz", item.demo_biz},
                    {"display_name", item.display_name},
                    {"ingress", std::move(ingress)},
                    {"egress", std::move(egress)}});
  }

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
  std::sort(all_input_converters.begin(), all_input_converters.end(),
            [](const InputConverterDefinition& a,
               const InputConverterDefinition& b) {
              return a.converter_id < b.converter_id;
            });

  nlohmann::json input_converters = nlohmann::json::array();
  for (const auto& c : all_input_converters) {
    if (!biz_filter.empty() &&
        active_input_converters.find(c.converter_id) ==
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
    if (!biz_filter.empty() &&
        active_output_converters.find(c.converter_id) ==
            active_output_converters.end()) {
      continue;
    }
    output_converters.push_back(OutputConverterToJson(c));
  }

  return {{"schema_version", 4},
          {"nodes", std::move(nodes)},
          {"models", std::move(models)},
          {"backends", std::move(backends)},
          {"bizs", std::move(bizs)},
          {"input_converters", std::move(input_converters)},
          {"output_converters", std::move(output_converters)},
          {"io_bindings", std::move(io_bindings)}};
}

nlohmann::json IoCatalog::ToJson(const std::string& biz_filter) {
  return ToJson(PipelineCatalog::Snapshot(), biz_filter);
}

}  // namespace llm_edgeflow
