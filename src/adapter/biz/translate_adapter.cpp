#include <nlohmann/json.hpp>

#include "adapter/adapter_authoring.h"
#include "adapter/biz_adapter_registry.h"
#include "adapter/biz_blackboard_keys.h"

namespace llm_edgeflow {

namespace {

AdapterResult<std::string> DecodeTranslateRequest(
    const OwnedTextRequest& request) {
  const auto req_json = nlohmann::json::parse(request.text, nullptr, false);
  if (!req_json.is_object() || !req_json.contains("query") ||
      !req_json["query"].is_string()) {
    return AdapterResult<std::string>::InvalidInput(
        "Expected a JSON object with string field query", "json", -1,
        "Translate");
  }
  // Pass only the original text to generation. std::string preserves
  // decoded newlines, quotes and embedded NUL. Ignore all other fields.
  return AdapterResult<std::string>::Ok(req_json["query"].get<std::string>());
}

AdapterResult<std::string> EncodeTranslateResponse(const std::string& answer) {
  // The model returns plain translated text. C++ alone owns the external
  // response schema and escaping; do not parse, trim or repair its text.
  // Exceptions from json dump (e.g. invalid UTF-8) propagate to
  // SharedAlgorithmRuntime to restore COMPANY_ALG_ERR_EXCEPTION (-99) contract
  // per RFC-0053 §3.2.
  const nlohmann::json response = {{"translated", answer}};
  return AdapterResult<std::string>::Ok(response.dump());
}

struct TranslateSpecProvider {
  static const OneToOneTextAdapterSpec& GetSpec() {
    static const OneToOneTextAdapterSpec spec = [] {
      OneToOneTextAdapterSpec s;
      s.biz_type = ALG_BIZ_TYPE_TRANSLATE;
      s.adapter_name = "Translate";
      s.sdk_abi_version = COMPANY_ALG_ABI_VERSION;
      s.c_input_type_name = "CompanyEntityInputStruct";
      s.c_output_type_name = "CompanyEntityOutputStruct";
      s.max_batch_size = 64;
      s.ownership_policy = OwnershipPolicy::kCopyIn;
      s.thread_model = ThreadModel::kStatelessThreadSafe;
      s.cardinality = OutputCardinality::kOneToOne;
      s.biz_name = "translate_v1";
      s.demo_biz = "translate";
      s.display_name = "JSON 字符串翻译";
      s.input_key = kInputSentences;
      s.output_key = kLlmAnswers;
      s.decode_fn = &DecodeTranslateRequest;
      s.encode_fn = &EncodeTranslateResponse;
      s.carrier_adapter_name = "EntityExtract";
      s.null_ctx_field = "json";
      return s;
    }();
    return spec;
  }
};

}  // namespace

using TranslateAdapter = OneToOneTextAdapter<TranslateSpecProvider>;

REGISTER_BIZ_ADAPTER(TranslateAdapter);

}  // namespace llm_edgeflow
