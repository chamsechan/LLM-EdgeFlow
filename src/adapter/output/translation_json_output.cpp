#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "contracts/inference_payloads.h"
#include "edgeflow/operator/types.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {
namespace {

constexpr const char* kOutputSlot = "entity_out";

AdapterStatus EncodeTranslation(const std::string& result,
                                CompanyOperatorEntityOutput* output,
                                const OutputStringWriter& writer) {
  output->status_code = 0;
  const nlohmann::json response = {{"translated", result}};
  return writer.Write(output->entities_json, "entities_json", response.dump());
}

int EncodeOperatorTranslationJson(AlgContext* context,
                                  const OutputPortBindings& bindings,
                                  const OutputEncodeOptions& options,
                                  ExternalOutputBatchView* destination,
                                  size_t* written_count,
                                  AdapterStatus* status) {
  return EncodeResultRows<CompanyOperatorEntityOutput>(
      context, bindings, options, destination, written_count, status,
      kOutputSlot, kRawRequestIds, kLlmAnswers, &EncodeTranslation);
}

OutputConverterDefinition MakeOperatorTranslationJsonOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "translate.json.operator.v1";

  def.schema_id = "translate.json.response";
  def.external_type = "CompanyOperatorEntityOutput";

  def.external_slots = {ExternalOutputSlot<CompanyOperatorEntityOutput>(
      kOutputSlot, {"entities_json"})};
  def.logical_ports = {RequiredInputPort(kRawRequestIds),
                       RequiredInputPort(kLlmAnswers)};
  def.encode_fn = &EncodeOperatorTranslationJson;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorTranslationJsonOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
