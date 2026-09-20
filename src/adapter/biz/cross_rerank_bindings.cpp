#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

BizDefinition MakeCrossRerankBizDefinition() {
  BizDefinition def;
  def.biz_name = "dense_cross_rerank_scoring";
  def.demo_biz = "cross_rerank";
  def.display_name = "Cross-Encoder 精排";
  def.ingress = {
      BizPortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      BizPortDefinition("rerank_queries", "TextBatch", true, "1:1"),
      BizPortDefinition("rerank_candidates", "RankedTextBatch", true, "N:1"),
      BizPortDefinition("rerank_pairs", "QueryCandidatesBatch", true, "N:1")};
  def.egress = {
      BizPortDefinition("ranked_results", "RankedTextBatch", true, "N:1")};
  return def;
}

const bool g_reg_cross_rerank_biz = []() {
  auto def = MakeCrossRerankBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

BizExposureDefinition MakeCrossRerankBizExposure() {
  BizExposureDefinition def;
  def.biz_name = "dense_cross_rerank_scoring";
  def.max_batch_size = 64;

  return def;
}

IoBindingDefinition MakeCrossRerankOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "cross_rerank.operator.v1";
  def.biz_name = "dense_cross_rerank_scoring";

  def.input_converter_id = "rerank.plain.operator.v1";
  def.output_converter_id = "rerank_result.plain.operator.v1";
  def.input_ports = {{"raw_request_ids", "raw_request_ids"},
                     {"rerank_queries", "rerank_queries"},
                     {"rerank_candidates", "rerank_candidates"},
                     {"rerank_pairs", "rerank_pairs"}};
  def.output_ports = {{"raw_request_ids", "raw_request_ids"},
                      {"ranked_results", "ranked_results"}};
  def.max_batch_size = 64;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeCrossRerankBizExposure());
REGISTER_IO_BINDING(MakeCrossRerankOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
