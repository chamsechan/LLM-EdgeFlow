#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "translate";

BizDefinition MakeTranslateBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.display_name = "JSON 字符串翻译";
  def.ingress = {RequiredBizInput(kInputSentences)};
  def.egress = {BizOutput(kLlmAnswers)};
  return def;
}

const bool g_reg_translate_biz = []() {
  auto def = MakeTranslateBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

IoBindingDefinition MakeTranslateOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "translate.operator.v1";
  def.biz_name = kBizName;

  def.input_converter_id = "translate.json.operator.v1";
  def.output_converter_id = "translate.json.operator.v1";
  return def;
}

REGISTER_IO_BINDING(MakeTranslateOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
