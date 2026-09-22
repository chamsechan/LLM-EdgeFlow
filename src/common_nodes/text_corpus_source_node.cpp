#include <string>
#include <vector>

#include "core/common_contracts.h"
#include "nodes/authoring.h"

namespace llm_edgeflow {

namespace {

const std::vector<ConfigFieldDefinition>& TextCorpusSourceConfigFields() {
  static const std::vector<ConfigFieldDefinition> kFields = {
      ConfigFieldDefinition{
          "corpus",
          ConfigValueKind::kArray,
          false,
          nlohmann::json(),
          std::nullopt,
          std::nullopt,
          {},
          "静态语料字符串数组，例如 [\"开户步骤\", \"退款政策\"]；数组顺序对应 "
          "sub_id，使用共享语料 req_id=0。"}};
  return kFields;
}

bool ValidateCorpusEntries(const nlohmann::json& config,
                           std::string* diagnostic) {
  if (config.contains("corpus")) {
    for (const auto& item : config.at("corpus")) {
      if (!item.is_string()) {
        if (diagnostic) *diagnostic = "corpus entries must be strings";
        return false;
      }
    }
  }
  return true;
}

struct CorpusInputs {
  const TextBatch* trigger = nullptr;
};
struct CorpusParams {
  std::vector<std::string> corpus;
};

NodeResult<TextBatch> ProduceCorpus(const CorpusInputs&,
                                    const CorpusParams& params) {
  TextBatch output;
  output.reserve(params.corpus.size());
  for (size_t i = 0; i < params.corpus.size(); ++i) {
    output.emplace_back(0, static_cast<uint32_t>(i), params.corpus[i]);
  }
  return NodeResult<TextBatch>::Success(std::move(output));
}

auto TextCorpusSourceSpec() {
  auto params =
      Parameters<CorpusParams>{}.WithParser(NodeConfigParser<CorpusParams>(
          TextCorpusSourceConfigFields(),
          [](const nlohmann::json& config, CorpusParams* value,
             std::string* diagnostic) {
            if (!ValidateCorpusEntries(config, diagnostic)) return false;
            if (config.contains("corpus"))
              value->corpus =
                  config.at("corpus").get<std::vector<std::string>>();
            return true;
          }));
  return MakeBatchSpec(
             InputsOf<CorpusInputs>(
                 {OptionalValue("trigger", &CorpusInputs::trigger)}),
             ProducedBatch<TextBatch>(
                 "corpus", PortFlow{"1:N", "generate_sub_id", "session"}),
             std::move(params), &ProduceCorpus)
      .Category("common")
      .Description("Static text corpus and knowledge database source node")
      .ParallelSafe(true);
}
}  // namespace
REGISTER_FUNCTION_NODE(TextCorpusSourceNode, TextCorpusSourceSpec());
}  // namespace llm_edgeflow
