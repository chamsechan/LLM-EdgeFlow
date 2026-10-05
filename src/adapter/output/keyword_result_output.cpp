#include <cstdint>
#include <string>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/output/rule_match_response.h"
#include "adapter/result_validation.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kOutputSlot = "keyword_out";

AdapterStatus EncodeKeyword(const RuleMatchItem& result,
                            CompanyOperatorKeywordOutput* output,
                            const OutputStringWriter& writer) {
  output->is_hit = result.is_hit;
  output->status_code = result.status_code;
  return writer.Write(output->match_result_json, "match_result_json",
                      SerializeRuleMatchResponse(result));
}

int EncodeOperatorKeywordResult(AlgContext* context,
                                const OutputEncodeOptions& options,
                                ExternalOutputBatchView* destination,
                                size_t* written_count, AdapterStatus* status) {
  return EncodeResultRows<CompanyOperatorKeywordOutput>(
      context, options, destination, written_count, status, kOutputSlot,
      kRuleMatches, &EncodeKeyword);
}

OutputConverterDefinition MakeOperatorKeywordResultOutputConverter() {
  OutputConverterDefinition def;
  def.converter_id = "keyword.result";

  def.schema_id = "keyword.result.response";

  def.external_slots = {
      ExternalOutputSlot<CompanyOperatorKeywordOutput>(kOutputSlot)};
  def.logical_ports = {RequiredInputPort(kRuleMatches)};
  def.encode_fn = &EncodeOperatorKeywordResult;
  return def;
}

REGISTER_OUTPUT_CONVERTER(MakeOperatorKeywordResultOutputConverter());

}  // namespace
}  // namespace llm_edgeflow
