#include "engine/models/generated_text_embedding/generated_text_embedding_model.h"

#include <cmath>
#include <stdexcept>

#include "edgeflow/log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/models/common/embedding_numeric_support.h"

namespace llm_edgeflow {

std::shared_ptr<IModel> GeneratedTextEmbeddingModel::Create(
    const ModelCreateContext& context, std::string* diagnostic) {
  try {
    auto session = std::dynamic_pointer_cast<IGeneratedTokenEmbeddingSession>(
        context.backend_session);
    if (!session ||
        session->Protocol() != ExecutionProtocol::kGeneratedTokenEmbedding ||
        session->GetBatchPolicy().max_batch_size != 1 ||
        session->GetBatchPolicy().fixed_batch_size != 0) {
      throw std::runtime_error(
          "generated_text_embedding requires a single-input generated-token "
          "embedding session");
    }
    auto model = std::make_shared<GeneratedTextEmbeddingModel>();
    model->session_ = std::move(session);
    const auto& dimension = context.model_config.at("embedding_dim");
    const auto limit =
        context.model_config.value("max_tokens", nlohmann::json(1));
    if (!dimension.is_number_integer() || dimension < 1 || dimension > 65536 ||
        !limit.is_number_integer() || limit < 1 || limit > 64) {
      throw std::runtime_error(
          "Invalid generated embedding dimension or token limit");
    }
    model->embedding_dim_ = dimension.get<int>();
    model->max_tokens_ = limit.get<int>();
    model->pooling_ = context.model_config.value("pooling", "last");
    model->prefix_ = context.model_config.value("prefix", "");
    model->suffix_ = context.model_config.value("suffix", "");
    model->add_bos_ = context.model_config.value("add_bos", false);
    if (model->pooling_ != "last" && model->pooling_ != "mean") {
      throw std::runtime_error(
          "Invalid generated_text_embedding configuration");
    }
    return model;
  } catch (const std::exception& e) {
    inference_detail::SetDiagnostic(diagnostic, e.what());
    return nullptr;
  } catch (...) {
    inference_detail::SetDiagnostic(
        diagnostic, "Unknown generated embedding creation error");
    return nullptr;
  }
}

const std::string& GeneratedTextEmbeddingModel::ModelType() const noexcept {
  static const std::string type = "generated_text_embedding";
  return type;
}
const std::string& GeneratedTextEmbeddingModel::Capability() const noexcept {
  static const std::string capability = "embedding";
  return capability;
}
InferenceConcurrency GeneratedTextEmbeddingModel::Concurrency() const noexcept {
  return InferenceConcurrency::kConcurrent;
}
size_t GeneratedTextEmbeddingModel::GetMaxBatchSize() const noexcept {
  return 1;
}

int GeneratedTextEmbeddingModel::Embed(const TextBatch& inputs,
                                       const EmbeddingOptions& options,
                                       EmbeddingBatch* outputs) noexcept {
  if (!outputs) return -1;
  outputs->clear();
  if (!session_) return -1;
  return FixedBatchExecutor::Execute<std::string, std::vector<float>>(
      inputs, session_->GetBatchPolicy(),
      [this, &inputs, &options](const BatchSlice& slice,
                                std::vector<std::vector<float>>* batch) {
        const auto& text = inputs[slice.offset].data;
        if (text.empty()) return -1;
        GeneratedTokenEmbeddings tokens;
        std::string diagnostic;
        const int result =
            session_->GenerateEmbeddings(prefix_ + text + suffix_, add_bos_,
                                         max_tokens_, &tokens, &diagnostic);
        if (result != 0) {
          ALG_LOG_ERROR("[GeneratedTextEmbeddingModel] %s\n",
                        diagnostic.c_str());
          return result;
        }
        if (tokens.values.empty() ||
            tokens.values.size() > static_cast<size_t>(max_tokens_) ||
            tokens.token_ids.size() != tokens.values.size()) {
          ALG_LOG_ERROR(
              "[GeneratedTextEmbeddingModel] Missing or invalid generated "
              "token vectors (including immediate EOS)\n");
          return -1;
        }
        std::vector<double> pooled(static_cast<size_t>(embedding_dim_), 0.0);
        for (size_t row = 0; row < tokens.values.size(); ++row) {
          const auto& values = tokens.values[row];
          if (values.size() != pooled.size()) {
            ALG_LOG_ERROR(
                "[GeneratedTextEmbeddingModel] Expected dimension %d, received "
                "%zu; check model_config.embedding_dim\n",
                embedding_dim_, values.size());
            return -1;
          }
          for (size_t col = 0; col < values.size(); ++col) {
            if (!std::isfinite(values[col])) return -1;
            if (pooling_ == "mean")
              pooled[col] += values[col];
            else if (row + 1 == tokens.values.size())
              pooled[col] = values[col];
          }
        }
        for (double& value : pooled) {
          if (pooling_ == "mean") value /= tokens.values.size();
        }
        std::vector<float> vector;
        if (!embedding_support::FinalizeEmbeddingVector(
                pooled, options.normalize, &vector)) {
          return -1;
        }
        batch->push_back(std::move(vector));
        return 0;
      },
      outputs);
}

static const ModelDefinition kGeneratedTextEmbeddingDefinition = [] {
  ModelDefinition definition;
  definition.model_type = "generated_text_embedding";
  definition.capability = "embedding";
  definition.description =
      "Experimental text vectors from greedy generated-token hidden states; "
      "last/mean pooling, not encoder embeddings";
  definition.required_protocol = ExecutionProtocol::kGeneratedTokenEmbedding;
  definition.concurrency = InferenceConcurrency::kConcurrent;
  definition.config_fields = {
      {"embedding_dim",
       ConfigValueKind::kInteger,
       true,
       nullptr,
       1.0,
       65536.0,
       {},
       "生成 token 的隐藏向量维数，必须与 Backend 返回的每个 token "
       "向量维度一致。"},
      {"max_tokens",
       ConfigValueKind::kInteger,
       false,
       1,
       1.0,
       64.0,
       {},
       "用于生成嵌入的 token 数上限；控制参与 last/mean 池化的生成 "
       "token，不是输入文本长度。"},
      {"pooling",
       ConfigValueKind::kString,
       false,
       "last",
       std::nullopt,
       std::nullopt,
       {"last", "mean"},
       "last 取最后一个生成 token 的向量；mean 对全部生成 token 向量求均值。"},
      {"prefix",
       ConfigValueKind::kString,
       false,
       "",
       std::nullopt,
       std::nullopt,
       {},
       "直接拼接在输入文本之前的模型提示词，不自动插入分隔符。"},
      {"suffix",
       ConfigValueKind::kString,
       false,
       "",
       std::nullopt,
       std::nullopt,
       {},
       "直接拼接在输入文本之后的模型提示词，不自动插入分隔符。"},
      {"add_bos",
       ConfigValueKind::kBoolean,
       false,
       false,
       std::nullopt,
       std::nullopt,
       {},
       "编码输入时请求添加模型的 BOS 起始 "
       "token，须与所选模型的分词约定一致。"}};
  return definition;
}();
REGISTER_MODEL_WITH_DEFINITION(GeneratedTextEmbeddingModel,
                               kGeneratedTextEmbeddingDefinition);

}  // namespace llm_edgeflow
