#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "dense_cross_rerank_scoring";
constexpr size_t kMaxBatchSize = 64;

BizDefinition MakeCrossRerankBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.display_name = "Cross-Encoder 精排";
  def.ingress = {
      RequiredBizInput(kRawRequestIds), RequiredBizInput(kRerankQueries),
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

BizExposureDefinition MakeCrossRerankBizExposure() {
  BizExposureDefinition def;
  def.biz_name = kBizName;
  def.max_batch_size = kMaxBatchSize;

  return def;
}

IoBindingDefinition MakeCrossRerankOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "cross_rerank.operator.v1";
  def.biz_name = kBizName;

  def.input_converter_id = "rerank.plain.operator.v1";
  def.output_converter_id = "rerank_result.plain.operator.v1";
  def.input_ports = {BindIoPort(kRawRequestIds), BindIoPort(kRerankQueries),
                     BindIoPort(kRerankCandidates), BindIoPort(kRerankPairs)};
  def.output_ports = {BindIoPort(kRawRequestIds), BindIoPort(kRankedResults)};
  def.max_batch_size = kMaxBatchSize;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeCrossRerankBizExposure());
REGISTER_IO_BINDING(MakeCrossRerankOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
