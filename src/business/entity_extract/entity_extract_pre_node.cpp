#include <iostream>
#include <string>
#include <vector>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 实体提取前处理算子 (构造 0.6B 实体/名词提取 Prompt)
 */
class EntityExtractPreNode : public INode {
 public:
  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)session_ctx;
    prompt_template_ = config.value(
        "prompt_template",
        "你是一个中文实体与名词抽取助手。请从以下句子中提取出所有名词与实体，"
        "并仅以JSON列表形式返回：\n输入文本：{text}\n提取结果：");
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    auto* sentences = req_ctx->Get<std::vector<std::string>>("input_sentences");
    if (!sentences) {
      req_ctx->SetError(-6001, "EntityExtractPreNode: Missing input_sentences");
      return -6001;
    }

    std::vector<TraceableItem<std::string>> prompt_items;
    prompt_items.reserve(sentences->size());

    for (uint32_t req_id = 0; req_id < sentences->size(); ++req_id) {
      std::string prompt = prompt_template_;
      size_t pos = prompt.find("{text}");
      if (pos != std::string::npos) {
        prompt.replace(pos, 6, (*sentences)[req_id]);
      }
      prompt_items.emplace_back(req_id, 0, std::move(prompt));
    }

    std::cout << "[EntityExtractPreNode] Formatted " << prompt_items.size()
              << " prompts for 0.6B model." << std::endl;

    req_ctx->Set("llm_input_prompts", std::move(prompt_items));
    return 0;
  }

  const std::string& Name() const override {
    static std::string name = "EntityExtractPreNode";
    return name;
  }

 private:
  std::string prompt_template_;
};

REGISTER_NODE(EntityExtractPreNode);

}  // namespace alg_framework
