#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "nodes/authoring.h"
#include "nodes/node_error_codes.h"

namespace llm_edgeflow {
namespace {
struct Inputs {
  const TextBatch* text = nullptr;
};
struct Params {
  bool cache = false;
};
struct Models {
  EmbeddingCall encoder;
};

void AppendUint64Le(std::string& buf, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    buf.push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

std::string ConstructSessionCacheKey(const std::string& model_name,
                                     const std::string& revision,
                                     const TextBatch& items) {
  std::string key;
  key.append("SEM1", 4);
  AppendUint64Le(key, model_name.size());
  key.append(model_name.data(), model_name.size());
  AppendUint64Le(key, revision.size());
  key.append(revision.data(), revision.size());
  AppendUint64Le(key, items.size());
  for (const auto& item : items) {
    AppendUint64Le(key, item.req_id);
    AppendUint64Le(key, item.sub_id);
    AppendUint64Le(key, item.data.size());
    key.append(item.data.data(), item.data.size());
  }
  return key;
}

NodeResult<EmbeddingBatch> Run(const Inputs& inputs, const Params& params,
                               const Models& models,
                               const SessionResources& resources) {
  const auto& text = *inputs.text;
  if (text.empty()) return NodeResult<EmbeddingBatch>::Success({});
  if (!params.cache) return models.encoder.Embed(text);

  SessionResourceKey<EmbeddingBatch> key(ConstructSessionCacheKey(
      models.encoder.ModelName(),
      resources.GetModelRevision(models.encoder.ModelName()), text));
  auto cached = resources.GetOrCreateResult<EmbeddingBatch>(
      key, [&]() { return models.encoder.Embed(text); });
  if (!cached.ok())
    return NodeResult<EmbeddingBatch>::Failure(cached.failure());
  if (!cached.value()) {
    return NodeResult<EmbeddingBatch>::Failure(
        NodeErrorKind::kModelCallError,
        "text_embedding: single-flight inference failed",
        node_error::text_embedding::kSessionInferenceFailed);
  }
  // 模型 facade 已校验缓存结果；PreservedOutput 在为本请求发布前
  // 会再次检查返回的副本。
  return NodeResult<EmbeddingBatch>::Success(*cached.value());
}

auto Spec() {
  const PortFlow flow{"1:1", "preserve", FollowLifetime("text")};
  return MakeNodeSpec(
             InputsOf<Inputs>({Required("text", &Inputs::text)}),
             PreservedOutput<EmbeddingBatch>("embedding", "text", flow),
             Parameters<Params>{}.Prepare(
                 [](Params* params, const BindingFacts& facts, std::string*) {
                   params->cache = facts.InputLifetime("text") == "session";
                   return true;
                 }),
             ModelsOf<Models>(
                 {Model("encoder", "bind_model", &Models::encoder)}),
             &Run)
      .Category("common")
      .Description("Text embedding extraction node")
      .ParallelSafe(true);
}
}  // namespace
REGISTER_FUNCTION_NODE(text_embedding, Spec());
}  // namespace llm_edgeflow
