#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "company_alg_interface.h"

namespace llm_edgeflow {

inline static constexpr char kDocQaBizName[] = "smart_doc_qa_v1";
inline static constexpr char kDocQaOnnxBizName[] =
    "smart_doc_qa_onnx_llamacpp_v1";
inline static constexpr char kDocQaRerankBizName[] =
    "smart_doc_qa_rerank_llm_v1";

class DocQaAdapter : public IBizAdapter {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_DOC_QA; }

  const char* BizName() const override { return "DocQA"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_DOC_QA,
        "DocQA",
        "2.0.0",
        "CompanyDocInputStruct",
        "CompanyDocOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kDocQaBizName,
          "doc_qa",
          "智能文档问答",
          {RequiredInput(kRawRequestIds), RequiredInput(kRawDocs),
           RequiredInput(kRawQueries)},
          {Output(kLlmAnswers), Output(kIntentMatches),
           Output(kDocChunkCounts)}},
         {kDocQaOnnxBizName,
          "doc_qa",
          "智能文档问答（ONNX/llama.cpp）",
          {RequiredInput(kRawRequestIds), RequiredInput(kRawDocs),
           RequiredInput(kRawQueries)},
          {Output(kLlmAnswers), Output(kIntentMatches),
           Output(kDocChunkCounts)}},
         {kDocQaRerankBizName,
          "doc_qa",
          "智能文档问答（精排）",
          {RequiredInput(kRawRequestIds), RequiredInput(kRawDocs),
           RequiredInput(kRawQueries)},
          {Output(kLlmAnswers), Output(kIntentMatches),
           Output(kDocChunkCounts)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed or null AlgContext",
          "inputs", BizName());
    }

    std::vector<uint64_t> raw_req_ids;
    TextBatch raw_docs;
    TextBatch raw_queries;

    raw_req_ids.reserve(num_inputs);
    raw_docs.reserve(num_inputs);
    raw_queries.reserve(num_inputs);

    constexpr size_t kMaxQueryLen = 64 * 1024;       // 64KB
    constexpr size_t kMaxDocLen = 10 * 1024 * 1024;  // 10MB 单文档上限

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_doc = static_cast<const CompanyDocInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in_doc, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].query_text", in_doc->query_text, kMaxQueryLen, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (in_doc->doc_text) {
        if (!AdapterValidationHelper::RequireBoundedString(
                "inputs[i].doc_text", in_doc->doc_text, kMaxDocLen, i,
                BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
      }

      raw_req_ids.push_back(in_doc->request_id);
      raw_docs.emplace_back(static_cast<uint32_t>(i), 0,
                            in_doc->doc_text ? in_doc->doc_text : "");
      raw_queries.emplace_back(static_cast<uint32_t>(i), 0,
                               in_doc->query_text ? in_doc->query_text : "");
    }

    if (!AdapterValidationHelper::PublishContextValue(*ctx, kRawRequestIds,
                                                      std::move(raw_req_ids),
                                                      BizName(), out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kRawDocs, std::move(raw_docs), BizName(), out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kRawQueries, std::move(raw_queries), BizName(), out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) {
      return AdapterValidationHelper::ReturnBufferTooSmall(
          out_status, "Null AlgContext passed to Pack", "ctx", BizName());
    }

    const auto* answers = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, kLlmAnswers, BizName(), out_status);
    if (!answers) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* intent_matches = ctx->Read(kIntentMatches);
    const auto* chunk_counts = ctx->Read(kDocChunkCounts);
    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    int count = static_cast<int>(answers->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName(), out_status);
    if (valid_ret != 0) return valid_ret;

    if (!intent_matches ||
        intent_matches->size() != static_cast<size_t>(count)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "intent_matches missing or count mismatch in AlgContext",
          "intent_matches", BizName());
    }

    if (!chunk_counts || chunk_counts->size() != static_cast<size_t>(count)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status,
          "doc_chunk_counts missing or count mismatch in AlgContext",
          "doc_chunk_counts", BizName());
    }
    if (!raw_req_ids || raw_req_ids->size() != static_cast<size_t>(count)) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "raw_request_ids missing or count mismatch in AlgContext",
          "raw_request_ids", BizName());
    }

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyDocOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*raw_req_ids)[i];

      const auto& match = (*intent_matches)[i].data;
      const std::string& intent = match.category;
      float conf = match.score;
      out_ptr->confidence = conf;

      out_ptr->chunk_count = (*chunk_counts)[i].data;
      out_ptr->status_code = 0;

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->intent_name, sizeof(out_ptr->intent_name),
              intent.c_str(), "outputs[i].intent_name", i, BizName(),
              out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->answer_text, sizeof(out_ptr->answer_text),
              (*answers)[i].data.c_str(), "outputs[i].answer_text", i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(DocQaAdapter);

}  // namespace llm_edgeflow
