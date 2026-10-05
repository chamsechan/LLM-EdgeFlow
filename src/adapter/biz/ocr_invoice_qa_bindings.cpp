#include "adapter/biz_blackboard_keys.h"
#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

constexpr const char* kBizName = "ocr_invoice_qa";

BizDefinition MakeOcrInvoiceQaBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.display_name = "OCR 票据问答";
  def.ingress = {RequiredBizInput(kImagePaths), RequiredBizInput(kUserQueries)};
  def.egress = {BizOutput(kExtractedInvoiceJson), BizOutput(kOcrDocs)};
  return def;
}

const bool g_reg_ocr_invoice_qa_biz = []() {
  auto def = MakeOcrInvoiceQaBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

IoBindingDefinition MakeOcrInvoiceQaOperatorBinding() {
  IoBindingDefinition def;
  def.biz_name = kBizName;
  def.input_converter_id = "image_query.plain";
  def.output_converter_id = "invoice_result.plain";
  return def;
}

REGISTER_IO_BINDING(MakeOcrInvoiceQaOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
