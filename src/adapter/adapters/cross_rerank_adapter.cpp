#include <algorithm>
#include <cstring>
#include <vector>

#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "company_alg_interface.h"

namespace llm_edgeflow {

inline static constexpr char kCrossRerankBizName[] =
    "dense_cross_rerank_scoring";

class CrossRerankAdapter : public IBizAdapter {
 public:
  CompanyAlgBizType BizType() const override {
    return ALG_BIZ_TYPE_CROSS_RERANK;
  }

  const char* BizName() const override { return "CrossRerank"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_CROSS_RERANK,
        "CrossRerank",
        "2.0.0",
        "CompanyRerankBatchInputStruct",
        "CompanyRerankBatchOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{kCrossRerankBizName,
          "cross_rerank",
          "Cross-Encoder 精排",
          {RequiredInput(kRawRequestIds), RequiredInput(kRerankQueries),
           RequiredInput(kRerankCandidates)},
          {Output(kRankedResults)}}}};
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
    TextBatch queries;
    RankedTextBatch candidates;
    QueryCandidatesBatch pairs;

    raw_req_ids.reserve(num_inputs);
    queries.reserve(num_inputs);

    constexpr size_t kMaxTextLen = 64 * 1024;  // 64KB 单文本上限

    for (int i = 0; i < num_inputs; ++i) {
      auto* in_rerank =
          static_cast<const CompanyRerankBatchInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in_rerank, i,
                                                   BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].query_text", in_rerank->query_text, kMaxTextLen, i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      if (!AdapterValidationHelper::RequireRange("inputs[i].candidate_count",
                                                 in_rerank->candidate_count, 1,
                                                 8, i, BizName(), out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      raw_req_ids.push_back(in_rerank->request_id);
      queries.emplace_back(static_cast<uint32_t>(i), 0, in_rerank->query_text);

      for (int c = 0; c < in_rerank->candidate_count; ++c) {
        std::string field_name =
            "inputs[i].candidate_passages[" + std::to_string(c) + "]";
        if (!AdapterValidationHelper::RequireBoundedString(
                field_name.c_str(), in_rerank->candidate_passages[c],
                kMaxTextLen, i, BizName(), out_status)) {
          return COMPANY_ALG_ERR_INVALID_INPUT;
        }

        std::string passage = in_rerank->candidate_passages[c];
        candidates.emplace_back(
            static_cast<uint32_t>(i), static_cast<uint32_t>(c),
            RankedCandidate(passage, 0.0f, c + 1, static_cast<uint32_t>(c)));
        pairs.emplace_back(
            static_cast<uint32_t>(i), static_cast<uint32_t>(c),
            QueryCandidatePair(in_rerank->query_text, std::move(passage)));
      }
    }

    if (!AdapterValidationHelper::PublishContextValue(*ctx, kRawRequestIds,
                                                      std::move(raw_req_ids),
                                                      BizName(), out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kRerankQueries, std::move(queries), BizName(), out_status) ||
        !AdapterValidationHelper::PublishContextValue(*ctx, kRerankCandidates,
                                                      std::move(candidates),
                                                      BizName(), out_status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kRerankPairs, std::move(pairs), BizName(), out_status)) {
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

    const auto* res = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, kRankedResults, BizName(), out_status);
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    const auto* raw_req_ids = ctx->Read(kRawRequestIds);

    // 按 req_id 分组
    std::unordered_map<uint32_t, std::vector<RankedCandidate>> req_map;
    for (const auto& item : *res) {
      req_map[item.req_id].push_back(item.data);
    }

    int count = raw_req_ids ? static_cast<int>(raw_req_ids->size())
                            : static_cast<int>(req_map.size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName(), out_status);
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out_ptr = static_cast<CompanyRerankBatchOutputStruct*>(outputs[i]);
      uint64_t req_id =
          (raw_req_ids && i < static_cast<int>(raw_req_ids->size()))
              ? (*raw_req_ids)[i]
              : static_cast<uint64_t>(i);
      out_ptr->request_id = req_id;

      const auto& cand_list = req_map[static_cast<uint32_t>(i)];
      int item_cnt = std::min(static_cast<int>(cand_list.size()), 8);
      out_ptr->count = item_cnt;
      out_ptr->status_code = 0;

      for (int k = 0; k < item_cnt; ++k) {
        out_ptr->scores[k] = cand_list[k].score;
        out_ptr->sorted_indices[k] =
            static_cast<int>(cand_list[k].original_sub_id);
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BIZ_ADAPTER(CrossRerankAdapter);

}  // namespace llm_edgeflow
