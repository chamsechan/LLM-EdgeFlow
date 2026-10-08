#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/result_validation.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kOutputSlot = "entity_out";

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
                                     const OutputEncodeOptions& options,
                                     ExternalOutputBatchView* destination,
                                     size_t* written_count,
                                     AdapterStatus* status) {
  return EncodeResultRows<CompanyOperatorEntityOutput>(
      context, options, destination, written_count, status, kOutputSlot,
      kExtractedEntities, &EncodeDocument);
}

// 输出字符串字段的尺寸参数；默认值按该业务的载荷设定，上限由平台结构登记决定。
struct Params {
  int64_t entities_json_max_bytes{};
};

auto ParamSpec() {
  return Parameters<Params>(
      {MaxBytes("entities_json", &Params::entities_json_max_bytes)
           .Default(2047)});
}

OutputConverterDefinition MakeOperatorStructuredDocumentOutputConverter() {
  OutputConverterDefinition def;
  def.type = kOutputSlot;
  def.name = "entity_extract";
  def.service_type =
      COMPANY_MOCK_SERVICE_ENTITY_EXTRACT;  // 占位取值，进内网核对
  def.slot = ExternalOutputSlot<CompanyOperatorEntityOutput>(kOutputSlot);
  def.logical_ports = {RequiredInputPort(kExtractedEntities)};
  def.params = ParamSpec();
  def.encode_fn = &EncodeOperatorStructuredDocument;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorStructuredDocumentOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
