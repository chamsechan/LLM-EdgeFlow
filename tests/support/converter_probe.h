#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "adapter/io_values.h"

namespace llm_edgeflow::test_support {

template <typename Value>
int DecodeConverter(const std::string& type, const std::string& name,
                    const std::vector<std::shared_ptr<Value>>& rows,
                    AlgContext* context, AdapterStatus* status) {
  const auto* definition =
      IoConverterRegistry::Instance().FindInputConverter(type, name);
  EXPECT_NE(definition, nullptr);
  if (!definition) return -99;
  std::shared_ptr<const ParameterValues> parameters;
  std::string error;
  EXPECT_TRUE(
      definition->params.Parse(nlohmann::json::object(), &parameters, &error))
      << error;
  if (!parameters) return -99;
  OperatorValueTypeBinding binding;
  binding.external_c_type_name = "OwnedCarrier";
  binding.validate_external = [](const void*, const InputLimits&,
                                 std::string*) { return 0; };
  SetInputValue<Value, Value>(&binding,
                              [](const Value& value) { return value; });
  ExternalInputBatchView source;
  source.count = rows.size();
  source.binding = &binding;
  source.slot_types[type] = "OwnedCarrier";
  for (const auto& row : rows) source.slots[type].push_back(row);
  IoPortBindings ports;
  for (const auto& port : definition->logical_ports)
    ports.emplace(port.logical_name, "decoded." + port.logical_name);
  const InputDecodeOptions options{type, name, parameters.get(), &ports};
  return definition->decode_fn(source, options, context, status);
}

inline int EncodeJsonConverter(const std::string& name, AlgContext* context,
                               std::vector<JsonOutputValue>* rows,
                               AdapterStatus* status) {
  const auto* definition =
      IoConverterRegistry::Instance().FindOutputConverter("json_out", name);
  EXPECT_NE(definition, nullptr);
  if (!definition) return -99;
  std::shared_ptr<const ParameterValues> parameters;
  std::string error;
  EXPECT_TRUE(definition->params.Parse({{"result_json_max_bytes", 4096}},
                                       &parameters, &error))
      << error;
  if (!parameters) return -99;
  OperatorValueTypeBinding binding;
  binding.external_c_type_name = "JsonCarrier";
  SetOutputValue<JsonOutputValue, JsonOutputValue>(
      &binding, [](JsonOutputValue& host, const JsonOutputValue& value,
                   const ResolvedOutputPoolSpec&) {
        host = value;
        return AdapterStatus::Ok();
      });
  ExternalOutputBatchView destination;
  destination.count = rows->size();
  destination.binding = &binding;
  destination.slot_types["json_out"] = "JsonCarrier";
  ResolvedOutputPoolSpec pool;
  destination.pool_specs["json_out"] = &pool;
  for (auto& row : *rows) destination.leased_slots["json_out"].push_back(&row);
  IoPortBindings ports;
  for (const auto& port : definition->logical_ports)
    ports.emplace(port.logical_name, "result." + port.logical_name);
  const OutputEncodeOptions options{"json_out", name, parameters.get(), &ports};
  size_t written = 0;
  const int code =
      definition->encode_fn(context, options, &destination, &written, status);
  if (code == 0) {
    EXPECT_EQ(written, rows->size());
  }
  return code;
}

}  // namespace llm_edgeflow::test_support
