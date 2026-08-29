#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "company_alg_interface.h"
#include "core/common_contracts.h"

namespace alg_framework {

inline static constexpr char kDocQaBusinessName[] = "smart_doc_qa_v1";
inline static constexpr char kDocQaOnnxBusinessName[] =
    "smart_doc_qa_onnx_llamacpp_v1";
inline static constexpr char kDocQaRerankBusinessName[] =
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
        {{kDocQaBusinessName,
          "doc_qa",
          "智能文档问答",
          {RequiredInput(kRawRequestIds), RequiredInput(kRawDocs),
           RequiredInput(kRawQueries)},
          {Output(kLlmAnswers), Output(kIntentMatches),
           Output(kDocChunkCounts)}},
         {kDocQaOnnxBusinessName,
          "doc_qa",
          "智能文档问答（ONNX/llama.cpp）",
          {RequiredInput(kRawRequestIds), RequiredInput(kRawDocs),
           RequiredInput(kRawQueries)},
          {Output(kLlmAnswers), Output(kIntentMatches),
           Output(kDocChunkCounts)}},
         {kDocQaRerankBusinessName,
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
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "Batch envelope validation failed or null AlgContext", "inputs", -1,
            BizName());
      }
      return COMPANY_ALG_ERR_INVALID_INPUT;
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

    ctx->Set(kRawRequestIds, std::move(raw_req_ids));
    ctx->Set(kRawDocs, std::move(raw_docs));
    ctx->Set(kRawQueries, std::move(raw_queries));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Null AlgContext passed to Pack", "ctx", -1, BizName());
      }
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    const auto* answers = ctx->Read(kLlmAnswers);
    const auto* intent_matches = ctx->Read(kIntentMatches);
    const auto* chunk_counts = ctx->Read(kDocChunkCounts);
    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    if (!answers) {
      if (out_status) {
        *out_status =
            AdapterStatus::BufferTooSmall("llm_answers not found in AlgContext",
                                          "llm_answers", -1, BizName());
      }
      return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    }

    int count = static_cast<int>(answers->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) {
      if (out_status) {
        *out_status = AdapterStatus::BufferTooSmall(
            "Output slots insufficient or null", "outputs", -1, BizName());
      }
      return valid_ret;
    }

    if (!intent_matches ||
        intent_matches->size() != static_cast<size_t>(count)) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "intent_matches missing or count mismatch in AlgContext",
            "intent_matches", -1, BizName());
      }
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }

    if (!chunk_counts || chunk_counts->size() != static_cast<size_t>(count)) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "doc_chunk_counts missing or count mismatch in AlgContext",
            "doc_chunk_counts", -1, BizName());
      }
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    if (!raw_req_ids || raw_req_ids->size() != static_cast<size_t>(count)) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "raw_request_ids missing or count mismatch in AlgContext",
            "raw_request_ids", -1, BizName());
      }
      return COMPANY_ALG_ERR_INVALID_INPUT;
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

}  // namespace alg_framework
