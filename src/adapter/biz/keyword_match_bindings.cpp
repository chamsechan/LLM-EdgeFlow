#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

BizDefinition MakeKeywordMatchBizDefinition() {
  BizDefinition def;
  def.biz_name = "keyword_match_v1";
  def.demo_biz = "keyword_match";
  def.display_name = "关注词匹配";
  def.ingress = {
      BizPortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      BizPortDefinition("input_sentences", "TextBatch", true, "1:1")};
  def.egress = {
      BizPortDefinition("rule_matches", "RuleMatchBatch", true, "1:1")};
  return def;
}

const bool g_reg_keyword_match_biz = []() {
  auto def = MakeKeywordMatchBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

BizExposureDefinition MakeKeywordMatchBizExposure() {
  BizExposureDefinition def;
  def.biz_name = "keyword_match_v1";
  def.max_batch_size = 64;
  def.required_transports = {"operator"};
  return def;
}

IoBindingDefinition MakeKeywordMatchOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "keyword_match.operator.v1";
  def.biz_name = "keyword_match_v1";
  def.transport = "operator";
  def.input_converter_id = "keyword.plain.operator.v1";
  def.output_converter_id = "keyword.result.operator.v1";
  def.input_ports = {{"raw_request_ids", "raw_request_ids"},
                     {"input_sentences", "input_sentences"}};
  def.output_ports = {{"raw_request_ids", "raw_request_ids"},
                      {"rule_matches", "rule_matches"}};
  def.max_batch_size = 64;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeKeywordMatchBizExposure());
REGISTER_IO_BINDING(MakeKeywordMatchOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
