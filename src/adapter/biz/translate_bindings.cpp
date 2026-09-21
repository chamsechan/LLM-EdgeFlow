#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "translate_v1";
constexpr size_t kMaxBatchSize = 64;

BizDefinition MakeTranslateBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.demo_biz = "translate";
  def.display_name = "JSON 字符串翻译";
  def.ingress = {RequiredBizInput(kRawRequestIds),
                 RequiredBizInput(kInputSentences)};
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

BizExposureDefinition MakeTranslateBizExposure() {
  BizExposureDefinition def;
  def.biz_name = kBizName;
  def.max_batch_size = kMaxBatchSize;

  return def;
}

IoBindingDefinition MakeTranslateOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "translate.operator.v1";
  def.biz_name = kBizName;

  def.input_converter_id = "translate.json.operator.v1";
  def.output_converter_id = "translate.json.operator.v1";
  def.input_ports = {BindIoPort(kRawRequestIds), BindIoPort(kInputSentences)};
  def.output_ports = {BindIoPort(kRawRequestIds), BindIoPort(kLlmAnswers)};
  def.max_batch_size = kMaxBatchSize;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeTranslateBizExposure());
REGISTER_IO_BINDING(MakeTranslateOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
