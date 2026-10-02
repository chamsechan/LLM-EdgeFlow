#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "dialogue_audit";

BizDefinition MakeDialogueAuditBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.display_name = "对话合规审核";
  def.ingress = {RequiredBizInput(kUserTexts), RequiredBizInput(kChannelNames)};
  def.egress = {BizOutput(kStructuredVerdicts),
                BizPortDefinition(kMatchedPolicy.name, kMatchedPolicy.type_id,
                                  true, "N:1")};
  return def;
}

const bool g_reg_dialogue_audit_biz = []() {
  auto def = MakeDialogueAuditBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

IoBindingDefinition MakeDialogueAuditOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "dialogue_audit.operator.v1";
  def.biz_name = kBizName;

  def.input_converter_id = "audit.plain.operator.v1";
  def.output_converter_id = "audit_result.plain.operator.v1";
  def.output_ports = {BindIoPort(kMatchedPolicies, kMatchedPolicy)};
  return def;
}

REGISTER_IO_BINDING(MakeDialogueAuditOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
