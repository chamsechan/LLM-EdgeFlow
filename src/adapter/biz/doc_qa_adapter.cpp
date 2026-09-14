#include <cstring>
#include <vector>

#include "adapter/adapter_batch.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/result_packing_adapter.h"
#include "adapter/result_validation.h"
#include "edgeflow/c_api.h"

namespace llm_edgeflow {

inline static constexpr char kDocQaBizName[] = "smart_doc_qa_v1";

class DocQaAdapter
    : public ResultPackingAdapter<DocQaAdapter, CompanyDocOutputStruct,
                                  DocResult> {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_DOC_QA; }

  const char* AdapterName() const override { return "DocQA"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_DOC_QA,
        "DocQA",
        COMPANY_ALG_ABI_VERSION,
        "CompanyDocInputStruct",
        "CompanyDocOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kDocQaBizName,
          "doc_qa",
          "智能文档问答",
          {RequiredBizInput(kRawRequestIds), RequiredBizInput(kRawDocs),
           RequiredBizInput(kRawQueries)},
          {BizOutput(kLlmAnswers), BizOutput(kIntentMatches),
           BizOutput(kDocChunkCounts)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, AdapterName());
    if (valid_ret != 0 || !ctx) {
      return AdapterValidationHelper::ReturnInvalidInput(
          out_status, "Batch envelope validation failed or null AlgContext",
          "inputs", AdapterName());
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
                                                   AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].query_text", in_doc->query_text, kMaxQueryLen, i,
              AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (in_doc->doc_text) {
        if (!AdapterValidationHelper::RequireBoundedString(
                "inputs[i].doc_text", in_doc->doc_text, kMaxDocLen, i,
                AdapterName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }
      }

      raw_req_ids.push_back(in_doc->request_id);
      raw_docs.emplace_back(static_cast<uint32_t>(i), 0,
                            in_doc->doc_text ? in_doc->doc_text : "");
      raw_queries.emplace_back(static_cast<uint32_t>(i), 0,
                               in_doc->query_text ? in_doc->query_text : "");
    }

    if (!AdapterValidationHelper::PublishContextValue(
            *ctx, kRawRequestIds, std::move(raw_req_ids), AdapterName(),
            out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kRawDocs, std::move(raw_docs), AdapterName(), out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kRawQueries, std::move(raw_queries), AdapterName(),
            out_status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  template <typename Output>
  int PackTyped(AlgContext* ctx, void** outputs, int* num_outputs,
                AdapterStatus* out_status = nullptr) const {
    const ResultBindingSpec<TextBatch> primary_spec(
        kLlmAnswers, "llm_answers", "answers", true,
        "llm_answers not found in AlgContext",
        COMPANY_ALG_ERR_BUFFER_TOO_SMALL);

    const ResultBindingSpec<std::vector<uint64_t>> raw_req_ids_spec(
        kRawRequestIds, "raw_request_ids", "raw_request_ids", true,
        "raw_request_ids missing or count mismatch in AlgContext",
        COMPANY_ALG_ERR_INVALID_INPUT);

    const ResultBindingSpec<RuleMatchBatch> intent_spec(
        kIntentMatches, "intent_matches", "intent_matches", true,
        "intent_matches missing or count mismatch in AlgContext",
        COMPANY_ALG_ERR_INVALID_INPUT);

    const ResultBindingSpec<Int32Batch> chunk_spec(
        kDocChunkCounts, "doc_chunk_counts", "chunk_counts", true,
        "doc_chunk_counts missing or count mismatch in AlgContext",
        COMPANY_ALG_ERR_INVALID_INPUT);

    RequestResults<TextBatch, RuleMatchBatch, Int32Batch> results;
    int ret = ReadMultiWayResults(ctx, outputs, num_outputs, AdapterName(),
                                  out_status, &results, primary_spec,
                                  raw_req_ids_spec, intent_spec, chunk_spec);
    if (ret != COMPANY_ALG_SUCCESS) return ret;

    for (size_t i = 0; i < results.Size(); ++i) {
      auto* out_ptr = static_cast<Output*>(outputs[i]);
      out_ptr->request_id = results.RequestId(i);

      const auto& match = results.Secondary<0>(i).data;
      const std::string& intent = match.category;
      float conf = match.score;
      out_ptr->confidence = conf;

      out_ptr->chunk_count = results.Secondary<1>(i).data;
      out_ptr->status_code = match.status_code;

      if (!CopyResultString(out_ptr->intent_name, intent.c_str(),
                            "outputs[i].intent_name", static_cast<int>(i),
                            AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }

      if (!CopyResultString(out_ptr->answer_text,
                            results.Primary(i).data.c_str(),
                            "outputs[i].answer_text", static_cast<int>(i),
                            AdapterName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = static_cast<int>(results.Size());
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(DocQaAdapter);

}  // namespace llm_edgeflow
