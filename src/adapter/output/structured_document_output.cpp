#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_values.h"
#include "adapter/result_validation.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

constexpr auto kEntities =
    MakeBlackboardKey<StructuredDocumentBatch>("entities");

constexpr const char* kOutputSlot = "entity_out";

struct Params {
  int64_t entities_json_max_bytes = 0;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>({
      MaxBytes("entities_json", &Params::entities_json_max_bytes).Default(2047),
  });
}

AdapterStatus EncodeDocument(const JsonDocumentItem& result,
                             EntityOutputValue* output) {
  if (!IsSuccessfulDocument(result)) {
    return AdapterStatus::InvalidInput(
        "Structured result failed or used fallback", "res");
  }
  output->status_code = 0;
  output->entities_json = result.json_payload;
  return AdapterStatus::Ok();
}

int EncodeOperatorStructuredDocument(AlgContext* context,
                                     const OutputEncodeOptions& options,
                                     ExternalOutputBatchView* destination,
                                     size_t* written_count,
                                     AdapterStatus* status) {
  return EncodeResultRows<EntityOutputValue>(context, options, destination,
                                             written_count, status, kOutputSlot,
                                             kEntities, &EncodeDocument);
}

OutputConverterDefinition MakeOperatorStructuredDocumentOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "entity_extract";
  def.slot = ExternalOutputSlot<EntityOutputValue>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kEntities)};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorStructuredDocument;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorStructuredDocumentOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
