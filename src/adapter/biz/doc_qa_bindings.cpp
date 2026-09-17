#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

BizDefinition MakeDocQaBizDefinition() {
  BizDefinition def;
  def.biz_name = "smart_doc_qa_v1";
  def.demo_biz = "doc_qa";
  def.display_name = "智能文档问答";
  def.ingress = {
      BizPortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      BizPortDefinition("raw_docs", "TextBatch", true, "1:1"),
      BizPortDefinition("raw_queries", "TextBatch", true, "1:1")};
  def.egress = {
      BizPortDefinition("llm_answers", "TextBatch", true, "1:1"),
      BizPortDefinition("intent_matches", "RuleMatchBatch", true, "1:1"),
      BizPortDefinition("doc_chunk_counts", "Int32Batch", true, "1:1")};
  return def;
}

const bool g_reg_doc_qa_biz = []() {
  auto def = MakeDocQaBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

BizExposureDefinition MakeDocQaBizExposure() {
  BizExposureDefinition def;
  def.biz_name = "smart_doc_qa_v1";
  def.max_batch_size = 64;
  def.required_transports = {"operator"};
  return def;
}

IoBindingDefinition MakeDocQaOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "doc_qa.operator.v1";
  def.biz_name = "smart_doc_qa_v1";
  def.transport = "operator";
  def.input_converter_id = "doc_query.plain.operator.v1";
  def.output_converter_id = "doc_answer.plain.operator.v1";
  def.input_ports = {{"raw_request_ids", "raw_request_ids"},
                     {"raw_docs", "raw_docs"},
                     {"raw_queries", "raw_queries"}};
  def.output_ports = {{"raw_request_ids", "raw_request_ids"},
                      {"llm_answers", "llm_answers"},
                      {"intent_matches", "intent_matches"},
                      {"doc_chunk_counts", "doc_chunk_counts"}};
  def.max_batch_size = 64;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeDocQaBizExposure());
REGISTER_IO_BINDING(MakeDocQaOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
