#include <type_traits>

#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/result_packing_adapter.h"

namespace llm_edgeflow {

// Reuse the established text/JSON carrier's copy-in, provenance and packing
// checks. Only the external JSON field mapping belongs to this Adapter.
class TranslateAdapter
    : public ResultPackingAdapter<TranslateAdapter, CompanyEntityOutputStruct,
                                  EntityResult> {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_TRANSLATE; }
  const char* AdapterName() const override { return "Translate"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static const AdapterDescriptor desc{
        ALG_BIZ_TYPE_TRANSLATE,
        "Translate",
        COMPANY_ALG_ABI_VERSION,
        "CompanyEntityInputStruct",
        "CompanyEntityOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {{"translate_v1",
          "translate",
          "JSON 字符串翻译",
          {RequiredBizInput(kRawRequestIds), RequiredBizInput(kInputSentences)},
          {BizOutput(kLlmAnswers)}}}};
    return desc;
  }

  int Unpack(const void** inputs, int count, AlgContext* ctx,
             AdapterStatus* status = nullptr) const override {
    const auto carrier =
        BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_ENTITY_EXTRACT);
    if (!ctx || !carrier)
      return Invalid(status, "Missing text carrier or context");
    AlgContext raw;
    const int ret = carrier->Unpack(inputs, count, &raw, status);
    if (ret != 0) return ret;

    TextBatch queries;
    for (const auto& item : *raw.Read(kInputSentences)) {
      const auto request = nlohmann::json::parse(item.data, nullptr, false);
      if (!request.is_object() || !request.contains("query") ||
          !request["query"].is_string()) {
        return Invalid(status,
                       "Expected a JSON object with string field query");
      }
      // Pass only the original text to generation. std::string preserves
      // decoded newlines, quotes and embedded NUL. Ignore all other fields.
      queries.emplace_back(item.req_id, item.sub_id,
                           request["query"].get<std::string>());
    }
    if (!AdapterValidationHelper::PublishContextValue(*ctx, kRawRequestIds,
                                                      *raw.Read(kRawRequestIds),
                                                      AdapterName(), status) ||
        !AdapterValidationHelper::PublishContextValue(
            *ctx, kInputSentences, std::move(queries), AdapterName(), status)) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  }

  template <typename Output>
  int PackTyped(AlgContext* ctx, void** outputs, int* count,
                AdapterStatus* status = nullptr) const {
    const auto carrier =
        BizAdapterRegistry::Instance().GetAdapter(ALG_BIZ_TYPE_ENTITY_EXTRACT);
    if (!ctx || !carrier)
      return Invalid(status, "Missing text carrier or context");
    const auto* answers = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, kLlmAnswers, AdapterName(), status);
    const auto* ids = AdapterValidationHelper::ReadRequiredContextValue(
        *ctx, kRawRequestIds, AdapterName(), status);
    if (!answers || !ids) return COMPANY_ALG_ERR_INVALID_INPUT;

    StructuredDocumentBatch translated;
    for (const auto& item : *answers) {
      // The model returns plain translated text. C++ alone owns the external
      // response schema and escaping; do not parse, trim or repair its text.
      const nlohmann::json response = {{"translated", item.data}};
      translated.emplace_back(
          item.req_id, item.sub_id,
          JsonDocumentItem(response.dump(), true, JsonParseStatus::kOk, "",
                           response));
    }
    AlgContext normalized;
    normalized.Publish(kRawRequestIds, *ids);
    normalized.Publish(kExtractedEntities, std::move(translated));
    if constexpr (std::is_same_v<Output, CompanyEntityOutputStruct>) {
      return carrier->Pack(&normalized, outputs, count, status);
    } else {
      return carrier->PackResultBatch(&normalized, outputs, count, status);
    }
  }

 private:
  int Invalid(AdapterStatus* status, const char* message) const {
    return AdapterValidationHelper::ReturnInvalidInput(status, message, "json",
                                                       AdapterName());
  }
};

REGISTER_BIZ_ADAPTER(TranslateAdapter);

}  // namespace llm_edgeflow
