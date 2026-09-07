#include <string>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "nodes/node_base.h"

namespace llm_edgeflow {

namespace {

const std::vector<ConfigFieldDefinition>& TextCorpusSourceConfigFields() {
  static const std::vector<ConfigFieldDefinition> kFields = {
      ConfigFieldDefinition{"corpus", ConfigValueKind::kArray, false}};
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

}  // namespace

/**
 * @brief 静态或配置驱动语料知识库源算子 (TextCorpusSourceNode,
 * 无需前置依赖直接产出 TextBatch)
 */
class TextCorpusSourceNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "TextCorpusSourceNode";

  TextCorpusSourceNode()
      : NodeBase(kNodeType), in_trigger_("trigger"), out_corpus_("corpus") {}

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    nlohmann::json normalized;
    if (!ValidateAndNormalizeFields(TextCorpusSourceConfigFields(), config,
                                    &normalized, nullptr)) {
      return false;
    }
    if (!ValidateCorpusEntries(normalized, nullptr)) {
      return false;
    }

    BindPort(init_ctx, in_trigger_);
    BindPort(init_ctx, out_corpus_);

    corpus_items_.clear();
    if (normalized.contains("corpus")) {
      for (const auto& elem : normalized["corpus"]) {
        corpus_items_.push_back(elem.get<std::string>());
      }
    }
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    TextBatch output_batch;
    output_batch.reserve(corpus_items_.size());

    for (size_t i = 0; i < corpus_items_.size(); ++i) {
      output_batch.emplace_back(0, static_cast<uint32_t>(i), corpus_items_[i]);
    }

    out_corpus_.Set(req_ctx, std::move(output_batch));
    return 0;
  }

 private:
  std::vector<std::string> corpus_items_;

  BoundInput<TextBatch> in_trigger_;
  BoundOutput<TextBatch> out_corpus_;
};

NodeDefinition MakeTextCorpusSourceNodeDefinition() {
  NodeDefinition def;
  def.node_type = TextCorpusSourceNode::kNodeType;
  def.category = "common";
  def.validate_config = [](const nlohmann::json& config, const auto&,
                           std::string* diagnostic) {
    return ValidateCorpusEntries(config, diagnostic);
  };
  def.description = "Static text corpus and knowledge database source node";
  def.inputs = {OptionalInputPort("trigger",
                                  BlackboardKey<TextBatch>{"", "TextBatch"},
                                  "1:1", "preserve", "request")};
  def.outputs = {OutputPort("corpus", BlackboardKey<TextBatch>{"", "TextBatch"},
                            "1:N", "generate_sub_id", "session")};
  def.config_fields = TextCorpusSourceConfigFields();
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(TextCorpusSourceNode,
                              MakeTextCorpusSourceNodeDefinition());

}  // namespace llm_edgeflow
