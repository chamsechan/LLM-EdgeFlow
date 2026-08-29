#include "engine/onnx/onnx_embedding_engine.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "engine/backend_registry.h"
#include "engine/engine_registry.h"
#include "engine/model_registry.h"

namespace alg_framework {

OnnxEmbeddingEngine::OnnxEmbeddingEngine() = default;
OnnxEmbeddingEngine::~OnnxEmbeddingEngine() = default;

bool OnnxEmbeddingEngine::Load(const std::string& model_path,
                               const nlohmann::json& engine_config) {
  model_path_ = model_path;
  max_batch_size_ = engine_config.value("max_batch_size", 4);
  embedding_dim_ = engine_config.value("embedding_dim", 384);
  device_id_ = engine_config.value("device_id", -1);

  auto backend = BackendRegistry::Instance().Create("onnxruntime");
  if (!backend) {
    ALG_LOG_WARNING(
        "[OnnxEmbeddingEngine] onnxruntime backend not registered in "
        "catalog\n");
    return false;
  }

  BackendLoadSpec bspec;
  bspec.model_path = model_path;
  bspec.backend_config = {{"max_batch_size", max_batch_size_}};
  std::string diag;
  auto session = backend->Load(bspec, &diag);
  if (!session) {
    ALG_LOG_WARNING(
        "[OnnxEmbeddingEngine] Failed to load ONNX backend session: %s\n",
        diag.c_str());
    return false;
  }

  ModelCreateContext mctx;
  mctx.backend_session = session;
  mctx.model_resource_root =
      std::filesystem::path(model_path).parent_path().string();
  mctx.model_config = {
      {"tokenizer_file", engine_config.value("tokenizer_file", "vocab.txt")},
      {"max_length", engine_config.value("max_length", 512)},
      {"pooling_strategy", engine_config.value("pooling_strategy", "cls")},
      {"normalize", engine_config.value("normalize", true)},
      {"output_name", engine_config.value("output_name", "last_hidden_state")},
      {"embedding_dim", embedding_dim_},
      {"max_batch_size", max_batch_size_}};

  auto model = ModelRegistry::Instance().Create("bge_embedding", mctx, &diag);
  if (!model) {
    ALG_LOG_WARNING(
        "[OnnxEmbeddingEngine] Failed to create BGE embedding model: %s\n",
        diag.c_str());
    return false;
  }

  model_ = std::dynamic_pointer_cast<IEmbeddingModel>(model);
  if (!model_) {
    return false;
  }

  is_loaded_ = true;
  return true;
}

const std::string& OnnxEmbeddingEngine::EngineType() const {
  static const std::string type = kEngineType;
  return type;
}

int OnnxEmbeddingEngine::InferTraceableBatch(
    const std::vector<TraceableItem<std::string>>& input_texts,
    std::vector<TraceableItem<std::vector<float>>>* output_embeddings) {
  if (!is_loaded_ || !model_) return -9001;
  EmbeddingOptions options;
  options.normalize = true;
  return model_->Embed(input_texts, options, output_embeddings);
}

EngineDefinition MakeOnnxEmbeddingDefinition() {
  EngineDefinition def;
  def.engine_type = OnnxEmbeddingEngine::kEngineType;
  def.capability = "embedding";
  def.description = "Legacy ONNX Runtime embedding engine adapter";
  def.config_fields = {
      ConfigFieldDefinition{"max_batch_size", ConfigValueKind::kInteger, false,
                            4, 1.0, 4096.0},
      ConfigFieldDefinition{"embedding_dim", ConfigValueKind::kInteger, false,
                            384, 1.0, 65536.0},
      ConfigFieldDefinition{"device_id", ConfigValueKind::kInteger, false, -1,
                            -1.0, 1024.0}};
  def.thread_model = EngineThreadModel::kConcurrent;
  return def;
}

REGISTER_ENGINE_WITH_DEFINITION(OnnxEmbeddingEngine,
                                MakeOnnxEmbeddingDefinition());

}  // namespace alg_framework
