#include "adapter/converter_authoring.h"
#include "adapter/io_binding.h"
#include "core/pipeline_catalog.h"

namespace llm_edgeflow {
namespace {

BizDefinition MakeOcrDocQaBizDefinition() {
  BizDefinition def;
  def.biz_name = "multimodal_ocr_invoice_qa";
  def.demo_biz = "ocr_doc_qa";
  def.display_name = "OCR 票据问答";
  def.ingress = {
      BizPortDefinition("raw_request_ids", "vector<uint64>", true, "1:1"),
      BizPortDefinition("image_paths", "ImageRefBatch", true, "1:1"),
      BizPortDefinition("user_queries", "TextBatch", true, "1:1")};
  def.egress = {BizPortDefinition("extracted_invoice_json",
                                  "StructuredDocumentBatch", true, "1:1"),
                BizPortDefinition("ocr_docs", "OcrDocumentBatch", true, "1:1")};
  return def;
}

const bool g_reg_ocr_doc_qa_biz = []() {
  auto def = MakeOcrDocQaBizDefinition();
  if (!PipelineCatalog::FindBiz(def.biz_name)) {
    PipelineCatalog::RegisterBizDefinition(def);
  }
  return true;
}();

BizExposureDefinition MakeOcrDocQaBizExposure() {
  BizExposureDefinition def;
  def.biz_name = "multimodal_ocr_invoice_qa";
  def.max_batch_size = 64;

  return def;
}

IoBindingDefinition MakeOcrDocQaOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "ocr_doc_qa.operator.v1";
  def.biz_name = "multimodal_ocr_invoice_qa";

  def.input_converter_id = "image_query.plain.operator.v1";
  def.output_converter_id = "invoice_result.plain.operator.v1";
  def.input_ports = {{"raw_request_ids", "raw_request_ids"},
                     {"image_paths", "image_paths"},
                     {"user_queries", "user_queries"}};
  def.output_ports = {{"raw_request_ids", "raw_request_ids"},
                      {"extracted_invoice_json", "extracted_invoice_json"},
                      {"ocr_docs", "ocr_docs"}};
  def.max_batch_size = 64;
  return def;
}

REGISTER_BIZ_EXPOSURE(MakeOcrDocQaBizExposure());
REGISTER_IO_BINDING(MakeOcrDocQaOperatorBinding());

}  // namespace
}  // namespace llm_edgeflow
