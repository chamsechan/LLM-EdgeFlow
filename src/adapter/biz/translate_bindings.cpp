#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

BizDefinition MakeTranslateBizDefinition() {
  BizDefinition def;
  def.biz_name = "translate_v1";
  def.demo_biz = "translate";
  def.display_name = "JSON 字符串翻译";
  def.ingress = {
      BizPortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      BizPortDefinition("input_sentences", "TextBatch", true, "1:1")};
  def.egress = {BizPortDefinition("llm_answers", "TextBatch", true, "1:1")};
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
  def.biz_name = "translate_v1";
  def.max_batch_size = 64;
  def.required_transports = {"cabi", "operator"};
  return def;
}

IoBindingDefinition MakeTranslateCAbiBinding() {
  IoBindingDefinition def;
  def.binding_id = "translate.cabi.v1";
  def.biz_name = "translate_v1";
  def.transport = "cabi";
  def.input_converter_id = "translate.json.cabi.v1";
  def.output_converter_id = "translate.json.cabi.v1";
  def.input_ports = {{"raw_request_ids", "raw_request_ids"},
                     {"input_sentences", "input_sentences"}};
  def.output_ports = {{"raw_request_ids", "raw_request_ids"},
                      {"llm_answers", "llm_answers"}};
  def.max_batch_size = 64;
  return def;
}

IoBindingDefinition MakeTranslateOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "translate.operator.v1";
  def.biz_name = "translate_v1";
  def.transport = "operator";
  def.input_converter_id = "translate.json.operator.v1";
  def.output_converter_id = "translate.json.operator.v1";
  def.input_ports = {{"raw_request_ids", "raw_request_ids"},
                     {"input_sentences", "input_sentences"}};
  def.output_ports = {{"raw_request_ids", "raw_request_ids"},
                      {"llm_answers", "llm_answers"}};
  def.max_batch_size = 64;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeTranslateBizExposure());
REGISTER_IO_BINDING(MakeTranslateCAbiBinding());
REGISTER_IO_BINDING(MakeTranslateOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
