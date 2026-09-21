#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "smart_doc_qa_v1";
constexpr size_t kMaxBatchSize = 64;

BizDefinition MakeDocQaBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.demo_biz = "doc_qa";
  def.display_name = "智能文档问答";
  def.ingress = {RequiredBizInput(kRawRequestIds), RequiredBizInput(kRawDocs),
                 RequiredBizInput(kRawQueries)};
  def.egress = {BizOutput(kLlmAnswers), BizOutput(kIntentMatches),
                BizOutput(kDocChunkCounts)};
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
  def.biz_name = kBizName;
  def.max_batch_size = kMaxBatchSize;

  return def;
}

IoBindingDefinition MakeDocQaOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "doc_qa.operator.v1";
  def.biz_name = kBizName;

  def.input_converter_id = "doc_query.plain.operator.v1";
  def.output_converter_id = "doc_answer.plain.operator.v1";
  def.input_ports = {BindIoPort(kRawRequestIds), BindIoPort(kRawDocs),
                     BindIoPort(kRawQueries)};
  def.output_ports = {BindIoPort(kRawRequestIds), BindIoPort(kLlmAnswers),
                      BindIoPort(kIntentMatches), BindIoPort(kDocChunkCounts)};
  def.max_batch_size = kMaxBatchSize;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeDocQaBizExposure());
REGISTER_IO_BINDING(MakeDocQaOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
