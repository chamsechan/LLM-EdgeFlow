#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

AdapterStatus EncodeDocument(const JsonDocumentItem& result,
                             CompanyOperatorEntityOutput* output,
                             const OutputStringWriter& writer) {
  if (!IsSuccessfulDocument(result)) {
    return AdapterStatus::InvalidInput(
        "Structured result failed or used fallback", "res");
  }
  output->status_code = 0;
  return writer.Write(output->entities_json, "entities_json",
                      result.json_payload);
}

int EncodeOperatorStructuredDocument(AlgContext* context,
                                     const OutputPortBindings& bindings,
                                     const OutputEncodeOptions& options,
                                     ExternalOutputBatchView* destination,
                                     size_t* written_count,
                                     AdapterStatus* status) {
  return EncodeResultRows<CompanyOperatorEntityOutput>(
      context, bindings, options, destination, written_count, status,
      "entity_out", kRawRequestIds, kExtractedEntities, &EncodeDocument);
}

OutputConverterDefinition MakeOperatorStructuredDocumentOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "document.structured.operator.v1";

  def.schema_id = "document.structured.response";
  def.external_type = "CompanyOperatorEntityOutput";
  def.max_batch_size = 64;

  def.external_slots = {ExternalOutputSlot<CompanyOperatorEntityOutput>(
      "entity_out", {"entities_json"})};
  def.logical_ports = {RequiredInputPort(kRawRequestIds),
                       RequiredInputPort(kExtractedEntities)};
  def.encode_fn = &EncodeOperatorStructuredDocument;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorStructuredDocumentOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
