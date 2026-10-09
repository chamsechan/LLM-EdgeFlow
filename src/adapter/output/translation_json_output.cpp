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

struct Params {
  int64_t entities_json_max_bytes = 0;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>({
      MaxBytes("entities_json", &Params::entities_json_max_bytes).Default(8191),
  });
}

AdapterStatus EncodeTranslation(const std::string& result,
                                CompanyOperatorEntityOutput* output,
                                const OutputStringWriter& writer) {
  output->status_code = 0;
  const nlohmann::json response = {{"translated", result}};
  return writer.Write(output->entities_json, "entities_json", response.dump());
}

int EncodeOperatorTranslationJson(AlgContext* context,
                                  const OutputEncodeOptions& options,
                                  ExternalOutputBatchView* destination,
                                  size_t* written_count,
                                  AdapterStatus* status) {
  return EncodeResultRows<CompanyOperatorEntityOutput>(
      context, options, destination, written_count, status, kOutputSlot,
      kLlmAnswers, &EncodeTranslation);
}

OutputConverterDefinition MakeOperatorTranslationJsonOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "translate";
  def.service_type = kMockServiceTranslate;
  def.slot = ExternalOutputSlot<CompanyOperatorEntityOutput>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kLlmAnswers)};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorTranslationJson;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorTranslationJsonOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
