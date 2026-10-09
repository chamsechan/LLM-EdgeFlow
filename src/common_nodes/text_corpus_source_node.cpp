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

struct Inputs {
  const TextBatch* trigger = nullptr;
};
struct Params {
  std::vector<std::string> corpus;
};

NodeResult<TextBatch> Run(const Inputs&, const Params& params) {
  TextBatch output;
  output.reserve(params.corpus.size());
  for (size_t i = 0; i < params.corpus.size(); ++i) {
    output.emplace_back(0, static_cast<uint32_t>(i), params.corpus[i]);
  }
  return NodeResult<TextBatch>::Success(std::move(output));
}

auto Spec() {
  auto params = Parameters<Params>{}.WithParser(ConfigParser<Params>(
      TextCorpusSourceConfigFields(),
      [](const nlohmann::json& config, Params* value, std::string* diagnostic) {
        if (!ValidateCorpusEntries(config, diagnostic)) return false;
        if (config.contains("corpus"))
          value->corpus = config.at("corpus").get<std::vector<std::string>>();
        return true;
      }));
  return MakeNodeSpec(
             InputsOf<Inputs>({OptionalValue("trigger", &Inputs::trigger)}),
             ProducedBatch<TextBatch>(
                 "corpus", PortFlow{"1:N", "generate_sub_id", "session"}),
             std::move(params), &Run)
      .Category("common")
      .Description("Static text corpus and knowledge database source node")
      .ParallelSafe(true);
}
}  // namespace
REGISTER_FUNCTION_NODE(TextCorpusSourceNode, Spec());
}  // namespace llm_edgeflow
