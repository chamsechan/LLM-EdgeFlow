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

AdapterStatus EncodeKeyword(const RuleMatchItem& result,
                            CompanyOperatorKeywordOutput* output,
                            const OutputStringWriter& writer) {
  output->is_hit = result.is_hit;
  output->status_code = result.status_code;
  return writer.Write(output->match_result_json, "match_result_json",
                      result.match_result_json);
}

int EncodeOperatorKeywordResult(AlgContext* context,
                                const OutputPortBindings& bindings,
                                const OutputEncodeOptions& options,
                                ExternalOutputBatchView* destination,
                                size_t* written_count, AdapterStatus* status) {
  return EncodeResultRows<CompanyOperatorKeywordOutput>(
      context, bindings, options, destination, written_count, status,
      "keyword_out", kRawRequestIds, kRuleMatches, &EncodeKeyword);
}

OutputConverterDefinition MakeOperatorKeywordResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "keyword.result.operator.v1";

  def.schema_id = "keyword.result.response";
  def.external_type = "CompanyOperatorKeywordOutput";
  def.max_batch_size = 64;

  def.external_slots = {ExternalOutputSlot<CompanyOperatorKeywordOutput>(
      "keyword_out", {"match_result_json"})};
  def.logical_ports = {RequiredInputPort(kRawRequestIds),
                       RequiredInputPort(kRuleMatches)};
  def.encode_fn = &EncodeOperatorKeywordResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorKeywordResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
