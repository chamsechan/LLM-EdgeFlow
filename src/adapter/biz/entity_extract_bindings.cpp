#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "entity_extract_v1";
constexpr size_t kMaxBatchSize = 64;

BizDefinition MakeEntityExtractBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.demo_biz = "entity_extract";
  def.display_name = "实体抽取";
  def.ingress = {RequiredBizInput(kRawRequestIds),
                 RequiredBizInput(kInputSentences)};
  def.egress = {BizOutput(kExtractedEntities)};
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
  def.biz_name = kBizName;
  def.max_batch_size = kMaxBatchSize;

  return def;
}

IoBindingDefinition MakeEntityExtractOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "entity_extract.operator.v1";
  def.biz_name = kBizName;

  def.input_converter_id = "text.plain.operator.v1";
  def.output_converter_id = "document.structured.operator.v1";
  def.input_ports = {BindIoPort(kRawRequestIds), BindIoPort(kInputSentences)};
  def.output_ports = {BindIoPort(kRawRequestIds),
                      BindIoPort(kExtractedEntities)};
  def.max_batch_size = kMaxBatchSize;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeEntityExtractBizExposure());
REGISTER_IO_BINDING(MakeEntityExtractOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
