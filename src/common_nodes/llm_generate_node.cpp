#include <cmath>
#include <cstdint>
#include <limits>

#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "edgeflow/log.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/model_calls.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {
namespace {

bool ParseGenerateOptions(const nlohmann::json& config,
                          GenerateOptions* options, std::string* diagnostic) {
  auto reject = [&](const std::string& message) {
    if (diagnostic) *diagnostic = message;
    return false;
  };
  try {
    const auto max_tokens = config.value("max_tokens", nlohmann::json(128));
    const auto top_k = config.value("top_k", nlohmann::json(0));
    if (!max_tokens.is_number_integer() || max_tokens < 1 ||
        max_tokens > 32768 || !top_k.is_number_integer() || top_k < 0 ||
        top_k > std::numeric_limits<int32_t>::max()) {
      return reject(
          "max_tokens or top_k is outside the supported integer range");
    }
    const double temperature = config.value("temperature", 0.7);
    const double top_p = config.value("top_p", 0.9);
    const double repetition_penalty = config.value("repetition_penalty", 1.0);
    if (!std::isfinite(temperature) || temperature < 0 || temperature > 2 ||
        !std::isfinite(top_p) || top_p < 1.0e-9 || top_p > 1 ||
        !std::isfinite(repetition_penalty) || repetition_penalty < 1.0e-9 ||
        repetition_penalty > 100) {
      return reject("Generation options are outside the supported range");
    }
    GenerateOptions parsed;
    parsed.max_tokens = max_tokens.get<int>();
    parsed.top_k = top_k.get<int>();
    parsed.temperature = static_cast<float>(temperature);
    parsed.top_p = static_cast<float>(top_p);
    parsed.repetition_penalty = static_cast<float>(repetition_penalty);
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

}  // namespace

/**
 * @brief LLM 推理生成公共算子 (LlmGenerateNode, 调用绑定的 ILlmModel)
 */
class LlmGenerateNode final : public ModelBoundNode<ILlmModel> {
 public:
  inline static constexpr char kNodeType[] = "LlmGenerateNode";

  LlmGenerateNode() : ModelBoundNode(kNodeType) {}

 protected:
  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& config, SessionContext&) override {
    BindPort(init_ctx, input_);
    BindPort(init_ctx, output_);
    std::string diagnostic;
    if (!ParseGenerateOptions(config, &gen_opt_, &diagnostic)) {
      return init_ctx.Fail(diagnostic);
    }
    generator_ = LlmCall(model());
    return true;
  }

  int ProcessNode(AlgContext& ctx) override {
    const auto* prompts =
        input_.Require(ctx, node_error::llm_generate::kMissingInput);
    if (!prompts) return node_error::llm_generate::kMissingInput;
    ALG_LOG_DEBUG(
        "[LlmGenerateNode] Inferring LLM outputs for %zu prompt items...\n",
        prompts->size());
    auto result = generator_.Generate(*prompts, gen_opt_);
    if (!result.ok()) {
      // Keep this existing node's public error codes while sharing validation.
      const auto& failure = result.failure();
      int code = failure.cause_code;
      std::string message = Name() + " inference failed";
      if (failure.kind == NodeErrorKind::kOutputCountMismatch) {
        code = node_error::llm_generate::kOutputCountMismatch;
        message = Name() + " output count mismatch";
      } else if (failure.kind == NodeErrorKind::kOutputProvenanceMismatch) {
        code = node_error::llm_generate::kOutputProvenanceMismatch;
        message = Name() + " output provenance mismatch";
      }
      return Fail(ctx, code, message);
    }
    output_.Set(ctx, std::move(result).value());
    return 0;
  }

 private:
  BoundInput<TextBatch> input_{"prompt"};
  BoundOutput<TextBatch> output_{"text"};
  GenerateOptions gen_opt_;
  LlmCall generator_;
};

NodeDefinition MakeLlmGenerateNodeDefinition() {
  NodeDefinition def;
  def.node_type = LlmGenerateNode::kNodeType;
  def.category = "common";
  def.validate_config = [](const nlohmann::json& config, const auto&,
                           std::string* diagnostic) {
    return ParseGenerateOptions(config, nullptr, diagnostic);
  };
  def.description = "LLM generate text inference node";
  def.inputs = {RequiredInputPort("prompt",
                                  BlackboardKey<TextBatch>{"", "TextBatch"},
                                  "1:1", "preserve", "request")};
  def.outputs = {OutputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"},
                            "1:1", "preserve", "request")};
  def.config_fields = {
      ConfigFieldDefinition{
          "bind_model",
          ConfigValueKind::kString,
          false,
          "llm_model_v1",
          std::nullopt,
          std::nullopt,
          {},
          "引用 models[].model_id；所选模型必须提供 llm 文本生成能力。"},
      ConfigFieldDefinition{
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
                            "\"<END>\"]；命中后输出不包含停止文本。"}};
  def.model_dependencies = {{"generator", "llm", "bind_model"}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(LlmGenerateNode, MakeLlmGenerateNodeDefinition());

}  // namespace llm_edgeflow
