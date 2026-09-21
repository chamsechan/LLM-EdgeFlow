#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "dialogue_compliance_audit_v1";
constexpr size_t kMaxBatchSize = 64;

BizDefinition MakeComplianceAuditBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.demo_biz = "dialogue_audit";
  def.display_name = "对话合规审核";
  def.ingress = {RequiredBizInput(kRawRequestIds), RequiredBizInput(kUserTexts),
                 RequiredBizInput(kChannelNames)};
  def.egress = {BizOutput(kStructuredVerdicts),
                BizPortDefinition(kMatchedPolicy.name, kMatchedPolicy.type_id,
                                  true, "N:1")};
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
  def.biz_name = kBizName;
  def.max_batch_size = kMaxBatchSize;

  return def;
}

IoBindingDefinition MakeComplianceAuditOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "compliance_audit.operator.v1";
  def.biz_name = kBizName;

  def.input_converter_id = "audit.plain.operator.v1";
  def.output_converter_id = "audit_result.plain.operator.v1";
  def.input_ports = {BindIoPort(kRawRequestIds), BindIoPort(kUserTexts),
                     BindIoPort(kChannelNames)};
  def.output_ports = {BindIoPort(kRawRequestIds),
                      BindIoPort(kStructuredVerdicts),
                      BindIoPort(kMatchedPolicies, kMatchedPolicy)};
  def.max_batch_size = kMaxBatchSize;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeComplianceAuditBizExposure());
REGISTER_IO_BINDING(MakeComplianceAuditOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
