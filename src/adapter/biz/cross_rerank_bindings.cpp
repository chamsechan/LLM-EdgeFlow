#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "dense_cross_rerank_scoring";

BizDefinition MakeCrossRerankBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.display_name = "Cross-Encoder 精排";
  def.ingress = {
      RequiredBizInput(kRerankQueries),
      BizPortDefinition(kRerankCandidates.name, kRerankCandidates.type_id, true,
                        "N:1"),
      BizPortDefinition(kRerankPairs.name, kRerankPairs.type_id, true, "N:1")};
  def.egress = {BizPortDefinition(kRankedResults.name, kRankedResults.type_id,
                                  true, "N:1")};
  return def;
}

const bool g_reg_cross_rerank_biz = []() {
  auto def = MakeCrossRerankBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

IoBindingDefinition MakeCrossRerankOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "cross_rerank.operator.v1";
  def.biz_name = kBizName;

  def.input_converter_id = "rerank.plain.operator.v1";
  def.output_converter_id = "rerank_result.plain.operator.v1";
  return def;
}

REGISTER_IO_BINDING(MakeCrossRerankOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
