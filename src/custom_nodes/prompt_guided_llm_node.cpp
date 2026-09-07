#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_support.h"

namespace llm_edgeflow {
namespace custom_nodes {

namespace error {
inline constexpr int kMissingInput = -8001;
inline constexpr int kModelInferenceFailed = -8002;
inline constexpr int kOutputProvenanceMismatch = -8003;
}  // namespace error

/**
 * @brief 方案开发者自定义节点样例：整合提示词构建、LLM推理与后处理清洗
 *
 * 满足 S1 阶段诉求：
 * 1. 绑定单一已注册模型能力 (ILlmModel, capability="llm")
 * 2. 在 ProcessNode
 * 内自包含完成前处理(Prompt模板替换)、模型调用与后处理(Markdown过滤/Fallback)
 * 3. 通过类型化端口输入输出，保留 (req_id, sub_id)
 * 溯源，线程安全且可被多方案复用
 */
class PromptGuidedLlmNode final : public ModelBoundNode<ILlmModel> {
 public:
  inline static constexpr char kNodeType[] = "PromptGuidedLlmNode";

  explicit PromptGuidedLlmNode(std::string node_name = kNodeType)
      : ModelBoundNode<ILlmModel>(std::move(node_name)),
        in_port_("input"),
        context_port_("context"),
        out_port_("output") {}

 protected:
  bool InitModelNode(const NodeInitContext& init_ctx,
                     const nlohmann::json& config,
                     SessionContext& session_ctx) override {
    (void)session_ctx;
    BindPort(init_ctx, in_port_);
    BindPort(init_ctx, context_port_);
    BindPort(init_ctx, out_port_);

    prompt_template_ = config.value("prompt_template", "{input}");
    system_prompt_ = config.value("system_prompt", "");
    fallback_text_ = config.value("fallback_text", "");
    strip_markdown_ = config.value("strip_markdown", false);

    gen_opt_.temperature = config.value("temperature", 0.7f);
    gen_opt_.max_tokens = config.value("max_tokens", 512);
    gen_opt_.top_k = config.value("top_k", 0);
    gen_opt_.top_p = config.value("top_p", 0.9f);
    gen_opt_.repetition_penalty = config.value("repetition_penalty", 1.0f);

    if (gen_opt_.max_tokens <= 0 || !std::isfinite(gen_opt_.temperature) ||
        gen_opt_.temperature < 0.0f || gen_opt_.temperature > 2.0f ||
        gen_opt_.top_k < 0 || !std::isfinite(gen_opt_.top_p) ||
        gen_opt_.top_p <= 0.0f || gen_opt_.top_p > 1.0f ||
        !std::isfinite(gen_opt_.repetition_penalty) ||
        gen_opt_.repetition_penalty <= 0.0f ||
        gen_opt_.repetition_penalty > 100.0f) {
      return false;
    }

    if (config.contains("stop_words") && config["stop_words"].is_array()) {
      for (const auto& w : config["stop_words"]) {
        if (w.is_string() && !w.get<std::string>().empty()) {
          gen_opt_.stop_words.push_back(w.get<std::string>());
        }
      }
    }
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* inputs =
        in_port_.Require(req_ctx, error::kMissingInput, "input text");
    if (!inputs) {
      return error::kMissingInput;
    }

    // 可选上下文港口
    const auto* contexts = context_port_.Get(req_ctx);

    // 组装模型输入批次，严格保留来源 (req_id, sub_id)
    TextBatch prompts;
    prompts.reserve(inputs->size());

    for (size_t i = 0; i < inputs->size(); ++i) {
      const auto& item = (*inputs)[i];
      std::string ctx_str;
      if (contexts) {
        for (const auto& c : *contexts) {
          if (c.req_id == item.req_id) {
            if (!ctx_str.empty()) ctx_str += "\n";
            ctx_str += c.data;
          }
        }
      }

      std::string rendered = RenderPrompt(item.data, ctx_str);
      prompts.emplace_back(item.req_id, item.sub_id, std::move(rendered));
    }

    TextBatch raw_outputs;
    int infer_ret = model()->Generate(prompts, gen_opt_, &raw_outputs);
    if (infer_ret != 0) {
      if (!fallback_text_.empty()) {
        TextBatch fallback_outputs;
        fallback_outputs.reserve(prompts.size());
        for (const auto& p : prompts) {
          fallback_outputs.emplace_back(p.req_id, p.sub_id, fallback_text_);
        }
        out_port_.Set(req_ctx, std::move(fallback_outputs));
        return 0;
      }
      req_ctx.SetError(error::kModelInferenceFailed,
                       Name() + ": model inference failed with code " +
                           std::to_string(infer_ret));
      return error::kModelInferenceFailed;
    }

    if (raw_outputs.size() != prompts.size()) {
      if (!fallback_text_.empty()) {
        TextBatch fallback_outputs;
        fallback_outputs.reserve(prompts.size());
        for (const auto& p : prompts) {
          fallback_outputs.emplace_back(p.req_id, p.sub_id, fallback_text_);
        }
        out_port_.Set(req_ctx, std::move(fallback_outputs));
        return 0;
      }
      req_ctx.SetError(error::kOutputProvenanceMismatch,
                       Name() + ": model output count mismatch (expected " +
                           std::to_string(prompts.size()) + ", got " +
                           std::to_string(raw_outputs.size()) + ")");
      return error::kOutputProvenanceMismatch;
    }

    TextBatch final_outputs;
    final_outputs.reserve(raw_outputs.size());
    for (size_t i = 0; i < raw_outputs.size(); ++i) {
      const auto& p = prompts[i];
      const auto& r = raw_outputs[i];
      if (r.req_id != p.req_id || r.sub_id != p.sub_id) {
        req_ctx.SetError(error::kOutputProvenanceMismatch,
                         Name() +
                             ": model output provenance mismatch for item " +
                             std::to_string(i));
        return error::kOutputProvenanceMismatch;
      }

      std::string out_str = r.data;
      if (strip_markdown_) {
        out_str = StripMarkdown(out_str);
      }
      final_outputs.emplace_back(r.req_id, r.sub_id, std::move(out_str));
    }

    out_port_.Set(req_ctx, std::move(final_outputs));
    return 0;
  }

 private:
  std::string RenderPrompt(const std::string& input,
                           const std::string& context) const {
    std::string result;
    if (!system_prompt_.empty()) {
      result += system_prompt_ + "\n";
    }
    if (prompt_template_ == "{input}" || prompt_template_.empty()) {
      result += input;
      if (!context.empty()) {
        result += "\n" + context;
      }
      return result;
    }
    std::string body = prompt_template_;
    ReplaceAll(body, "{input}", input);
    ReplaceAll(body, "{context}", context);
    result += body;
    return result;
  }

  static void ReplaceAll(std::string& str, std::string_view from,
                         std::string_view to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
      str.replace(pos, from.length(), to);
      pos += to.length();
    }
  }

  static bool StartsWith(std::string_view str, std::string_view prefix) {
    return str.size() >= prefix.size() &&
           str.compare(0, prefix.size(), prefix) == 0;
  }

  static bool EndsWith(std::string_view str, std::string_view suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  static std::string StripMarkdown(std::string_view text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    size_t end = text.find_last_not_of(" \t\r\n");
    std::string_view trimmed = text.substr(start, end - start + 1);

    if (StartsWith(trimmed, "```")) {
      size_t first_nl = trimmed.find('\n');
      if (first_nl != std::string_view::npos) {
        trimmed = trimmed.substr(first_nl + 1);
      } else {
        trimmed = "";
      }
    }
    if (EndsWith(trimmed, "```")) {
      trimmed.remove_suffix(3);
    }
    start = trimmed.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    end = trimmed.find_last_not_of(" \t\r\n");
    return std::string(trimmed.substr(start, end - start + 1));
  }

  BoundInput<TextBatch> in_port_;
  BoundInput<TextBatch> context_port_;
  BoundOutput<TextBatch> out_port_;

  std::string prompt_template_;
  std::string system_prompt_;
  std::string fallback_text_;
  bool strip_markdown_ = false;
  GenerateOptions gen_opt_;
};

NodeDefinition MakePromptGuidedLlmNodeDefinition() {
  NodeDefinition def;
  def.node_type = PromptGuidedLlmNode::kNodeType;
  def.category = "custom";
  def.description =
      "Custom domain node combining prompt construction, LLM generation, "
      "and response post-processing";
  def.inputs = {
      RequiredInputPort("input", BlackboardKey<TextBatch>{"", "TextBatch"},
                        "1:1", "preserve", "request"),
      OptionalInputPort("context", BlackboardKey<TextBatch>{"", "TextBatch"},
                        "N:1", "aggregate", "request"),
  };
  def.outputs = {
      OutputPort("output", BlackboardKey<TextBatch>{"", "TextBatch"}, "1:1",
                 "preserve", "request"),
  };
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false,
                            "llm_model_v1"},
      ConfigFieldDefinition{"prompt_template", ConfigValueKind::kString, false,
                            "{input}"},
      ConfigFieldDefinition{"system_prompt", ConfigValueKind::kString, false,
                            ""},
      ConfigFieldDefinition{"temperature", ConfigValueKind::kNumber, false, 0.7,
                            0.0, 2.0},
      ConfigFieldDefinition{"max_tokens", ConfigValueKind::kInteger, false, 512,
                            1.0, 32768.0},
      ConfigFieldDefinition{
          "top_k", ConfigValueKind::kInteger, false, 0, 0.0,
          static_cast<double>(std::numeric_limits<int32_t>::max())},
      ConfigFieldDefinition{"top_p", ConfigValueKind::kNumber, false, 0.9,
                            1.0e-9, 1.0},
      ConfigFieldDefinition{"repetition_penalty", ConfigValueKind::kNumber,
                            false, 1.0, 1.0e-9, 100.0},
      ConfigFieldDefinition{"strip_markdown", ConfigValueKind::kBoolean, false,
                            false},
      ConfigFieldDefinition{"fallback_text", ConfigValueKind::kString, false,
                            ""},
      ConfigFieldDefinition{"stop_words", ConfigValueKind::kArray, false,
                            nlohmann::json::array()}};
  def.model_capability = "llm";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(PromptGuidedLlmNode,
                              MakePromptGuidedLlmNodeDefinition());

}  // namespace custom_nodes
}  // namespace llm_edgeflow
