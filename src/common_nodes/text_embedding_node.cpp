#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "nodes/authoring.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {
namespace {
struct EmbeddingInputs {
  const TextBatch* text = nullptr;
};
struct EmbeddingParams {
  bool normalize{};
  std::string lifetime;
};
struct EmbeddingModels {
  EmbeddingCall encoder;
};

// The session single-flight cache shares the complete failure with all waiters.
struct EmbeddingFailure : std::runtime_error {
  explicit EmbeddingFailure(NodeFailure value)
      : std::runtime_error(value.message), failure(std::move(value)) {}
  NodeFailure failure;
};

void AppendUint64Le(std::string& buf, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    buf.push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

std::string ConstructSessionCacheKey(const std::string& model_id,
                                     const std::string& revision,
                                     bool normalize, const TextBatch& items) {
  std::string key;
  key.append("SEM1", 4);
  AppendUint64Le(key, model_id.size());
  key.append(model_id.data(), model_id.size());
  AppendUint64Le(key, revision.size());
  key.append(revision.data(), revision.size());
  key.push_back(normalize ? '\x01' : '\x00');
  AppendUint64Le(key, items.size());
  for (const auto& item : items) {
    AppendUint64Le(key, item.req_id);
    AppendUint64Le(key, item.sub_id);
    AppendUint64Le(key, item.data.size());
    key.append(item.data.data(), item.data.size());
  }
  return key;
}

NodeResult<EmbeddingBatch> EmbedText(const EmbeddingInputs& inputs,
                                     const EmbeddingParams& params,
                                     const EmbeddingModels& models,
                                     const SessionResources& resources) {
  const auto& text = *inputs.text;
  if (text.empty()) return NodeResult<EmbeddingBatch>::Success({});
  EmbeddingOptions options;
  options.normalize = params.normalize;
  if (params.lifetime == "request") return models.encoder.Embed(text, options);

  SessionResourceKey<EmbeddingBatch> key(ConstructSessionCacheKey(
      models.encoder.ModelId(),
      resources.GetModelRevision(models.encoder.ModelId()), params.normalize,
      text));
  std::shared_ptr<EmbeddingBatch> cached;
  try {
    cached = resources.GetOrCreateResource<EmbeddingBatch>(key, [&]() {
      auto output = models.encoder.Embed(text, options);
      if (!output.ok()) throw EmbeddingFailure(output.failure());
      return std::make_shared<EmbeddingBatch>(std::move(output).value());
    });
  } catch (const EmbeddingFailure& error) {
    return NodeResult<EmbeddingBatch>::Failure(error.failure);
  }
  if (!cached) {
    return NodeResult<EmbeddingBatch>::Failure(
        NodeErrorKind::kModelCallError,
        "TextEmbeddingNode: single-flight inference failed",
        node_error::text_embedding::kSessionInferenceFailed);
  }
  return detail::ConvertAlignedOutputs(text, EmbeddingBatch(*cached),
                                       "Embedding", models.encoder.SlotName());
}

auto TextEmbeddingSpec() {
  const PortFlow flow{"N:M", "preserve", "request", "lifetime"};
  return MakeBatchSpec(
             InputsOf<EmbeddingInputs>(
                 {Required("text", &EmbeddingInputs::text, flow)}),
             PreservedOutput<EmbeddingBatch>("embedding", "text", flow),
             Parameters<EmbeddingParams>(
                 {Field("normalize", &EmbeddingParams::normalize)
                      .Default(true)
                      .Description("要求模型对输出向量做 L2 归一化。"),
                  Field("lifetime", &EmbeddingParams::lifetime)
                      .Default("request")
                      .Enum({"request", "session"})
                      .Description("request 每次请求计算；session "
                                   "按模型版本、归一化选项和输入缓存向量，输入"
                                   "须满足 session 生命周期契约。")}),
             ModelsOf<EmbeddingModels>(
                 {Model("encoder", "bind_model", &EmbeddingModels::encoder,
                        "embed_model_v1",
                        "引用 models[].model_id；所选模型必须提供 embedding "
                        "文本向量能力。")}),
             &EmbedText)
      .Category("common")
      .Description("Text embedding extraction node")
      .ParallelSafe(true);
}
}  // namespace
REGISTER_FUNCTION_NODE(TextEmbeddingNode, TextEmbeddingSpec());
}  // namespace llm_edgeflow
