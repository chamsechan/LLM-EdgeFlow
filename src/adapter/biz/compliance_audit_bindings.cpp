#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

BizDefinition MakeComplianceAuditBizDefinition() {
  BizDefinition def;
  def.biz_name = "dialogue_compliance_audit_v1";
  def.demo_biz = "dialogue_audit";
  def.display_name = "对话合规审核";
  def.ingress = {
      BizPortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      BizPortDefinition("user_texts", "TextBatch", true, "1:1"),
      BizPortDefinition("channel_names", "TextBatch", true, "1:1")};
  def.egress = {
      BizPortDefinition("structured_verdicts", "StructuredDocumentBatch", true,
                        "1:1"),
      BizPortDefinition("matched_policy", "RankedTextBatch", true, "N:1")};
  return def;
}

const bool g_reg_compliance_audit_biz = []() {
  auto def = MakeComplianceAuditBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

BizExposureDefinition MakeComplianceAuditBizExposure() {
  BizExposureDefinition def;
  def.biz_name = "dialogue_compliance_audit_v1";
  def.max_batch_size = 64;
  def.required_transports = {"operator"};
  return def;
}

IoBindingDefinition MakeComplianceAuditOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "compliance_audit.operator.v1";
  def.biz_name = "dialogue_compliance_audit_v1";
  def.transport = "operator";
  def.input_converter_id = "audit.plain.operator.v1";
  def.output_converter_id = "audit_result.plain.operator.v1";
  def.input_ports = {{"raw_request_ids", "raw_request_ids"},
                     {"user_texts", "user_texts"},
                     {"channel_names", "channel_names"}};
  def.output_ports = {{"raw_request_ids", "raw_request_ids"},
                      {"structured_verdicts", "structured_verdicts"},
                      {"matched_policies", "matched_policy"}};
  def.max_batch_size = 64;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeComplianceAuditBizExposure());
REGISTER_IO_BINDING(MakeComplianceAuditOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
