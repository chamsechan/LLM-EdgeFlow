#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

BizDefinition MakeEntityExtractBizDefinition() {
  BizDefinition def;
  def.biz_name = "entity_extract_v1";
  def.demo_biz = "entity_extract";
  def.display_name = "实体抽取";
  def.ingress = {
      BizPortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      BizPortDefinition("input_sentences", "TextBatch", true, "1:1")};
  def.egress = {BizPortDefinition("extracted_entities",
                                  "StructuredDocumentBatch", true, "1:1")};
  return def;
}

const bool g_reg_entity_extract_biz = []() {
  auto def = MakeEntityExtractBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

BizExposureDefinition MakeEntityExtractBizExposure() {
  BizExposureDefinition def;
  def.biz_name = "entity_extract_v1";
  def.max_batch_size = 64;

  return def;
}

IoBindingDefinition MakeEntityExtractOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "entity_extract.operator.v1";
  def.biz_name = "entity_extract_v1";

  def.input_converter_id = "text.plain.operator.v1";
  def.output_converter_id = "document.structured.operator.v1";
  def.input_ports = {{"raw_request_ids", "raw_request_ids"},
                     {"input_sentences", "input_sentences"}};
  def.output_ports = {{"raw_request_ids", "raw_request_ids"},
                      {"extracted_entities", "extracted_entities"}};
  def.max_batch_size = 64;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeEntityExtractBizExposure());
REGISTER_IO_BINDING(MakeEntityExtractOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
