#include <cstdint>
#include <limits>

#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace {

bool ParseGenerateOptions(const nlohmann::json& config,
                          GenerateOptions* options, std::string* diagnostic) {
  auto reject = [&](const std::string& message) {
    if (diagnostic) *diagnostic = message;
    return false;
  };
  try {
    GenerateOptions parsed;
    parsed.max_tokens = config.at("max_tokens").get<int>();
    parsed.top_k = config.at("top_k").get<int>();
    parsed.temperature = config.at("temperature").get<float>();
    parsed.top_p = config.at("top_p").get<float>();
    parsed.repetition_penalty = config.at("repetition_penalty").get<float>();
    if (config.contains("stop_words")) {
      if (!config["stop_words"].is_array())
        return reject("stop_words must be an array");
      for (const auto& word : config["stop_words"]) {
        if (!word.is_string() || word.get_ref<const std::string&>().empty()) {
          return reject("stop_words must contain non-empty strings");
        }
        parsed.stop_words.push_back(word.get<std::string>());
      }
    }
    if (options) *options = std::move(parsed);
    return true;
  } catch (const std::exception& error) {
    return reject(error.what());
  }
}

struct Inputs {
  const TextBatch* prompt = nullptr;
};
struct Models {
  LlmCall generator;
};

NodeResult<TextBatch> Generate(const Inputs& inputs,
                               const GenerateOptions& params,
                               const Models& models) {
  return models.generator.Generate(*inputs.prompt, params);
}

auto LlmGenerateSpec() {
  NodeConfigParser<GenerateOptions> parser(
      {ConfigFieldDefinition{
           "temperature",
           ConfigValueKind::kNumber,
           false,
           0.7,
           0.0,
           2.0,
           {},
           "生成采样温度；0 用于贪心生成，具体采样由绑定模型执行。"},
       ConfigFieldDefinition{"max_tokens",
                             ConfigValueKind::kInteger,
                             false,
                             128,
                             1.0,
                             32768.0,
                             {},
                             "每条输入最多生成的 token "
                             "数，不包含输入提示词；还受模型上下文容量限制。"},
       ConfigFieldDefinition{
           "top_k",
           ConfigValueKind::kInteger,
           false,
           0,
           0.0,
           static_cast<double>(std::numeric_limits<int32_t>::max()),
           {},
           "采样时保留的候选 token 数；0 "
           "表示不按数量截断，区别于检索返回条数。"},
       ConfigFieldDefinition{"top_p",
                             ConfigValueKind::kNumber,
                             false,
                             0.9,
                             1.0e-9,
                             1.0,
                             {},
                             "核采样的累计概率阈值；1 表示不按累计概率截断。"},
       ConfigFieldDefinition{
           "repetition_penalty",
           ConfigValueKind::kNumber,
           false,
           1.0,
           1.0e-9,
           100.0,
           {},
           "已出现 token 的重复惩罚系数；1 不调整，大于 1 抑制重复。"},
       ConfigFieldDefinition{"stop_words",
                             ConfigValueKind::kArray,
                             false,
                             nlohmann::json::array(),
                             std::nullopt,
                             std::nullopt,
                             {},
                             "生成停止文本数组，例如 [\"结束\", "
                             "\"<END>\"]；命中后输出不包含停止文本。"}},
      ParseGenerateOptions);
  return MakeBatchSpec(
             InputsOf<Inputs>{Required("prompt", &Inputs::prompt)},
             PreservedOutput<TextBatch>("text", "prompt"),
             Parameters<GenerateOptions>{}.WithParser(std::move(parser)),
             ModelsOf<Models>{Model("generator", "bind_model",
                                    &Models::generator,
                                    "引用 models[].model_id；所选模型必须提供 "
                                    "llm 文本生成能力。")},
             &Generate)
      .Category("common")
      .ParallelSafe(true)
      .Description("LLM generate text inference node");
}

REGISTER_FUNCTION_NODE(LlmGenerateNode, LlmGenerateSpec());

}  // namespace
}  // namespace llm_edgeflow
