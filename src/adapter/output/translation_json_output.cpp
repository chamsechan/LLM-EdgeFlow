#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/json_output_parameters.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {
namespace {

constexpr auto kTranslation = MakeBlackboardKey<TextBatch>("translation");

constexpr const char* kOutputSlot = "json_out";

AdapterStatus EncodeTranslation(const std::string& result,
                                JsonOutputValue* output) {
  output->status_code = 0;
  const nlohmann::json response = {{"translated", result}};
  output->result_json = response.dump();
  return AdapterStatus::Ok();
}

int EncodeOperatorTranslationJson(AlgContext* context,
                                  const OutputEncodeOptions& options,
                                  ExternalOutputBatchView* destination,
                                  size_t* written_count,
                                  AdapterStatus* status) {
  return EncodeResultRows<JsonOutputValue>(context, options, destination,
                                           written_count, status, kOutputSlot,
                                           kTranslation, &EncodeTranslation);
}

OutputConverterDefinition MakeOperatorTranslationJsonOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "translate";
  def.slot = ExternalOutputSlot<JsonOutputValue>();
  def.logical_ports = {RequiredInputPort(kTranslation)};
  def.params = JsonOutputParameters();
  def.encode_fn = &EncodeOperatorTranslationJson;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorTranslationJsonOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
