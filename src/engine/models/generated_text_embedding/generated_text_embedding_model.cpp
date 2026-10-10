#include "engine/models/generated_text_embedding/generated_text_embedding_model.h"

#include <cmath>
#include <stdexcept>

#include "contracts/diagnostic.h"
#include "contracts/parameters.h"
#include "edgeflow/log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/models/common/embedding_numeric_support.h"

namespace llm_edgeflow {

namespace {

struct Params {
  int embedding_dim = 0;
  int max_tokens = 1;
  std::string pooling;
  std::string prompt_prefix;
  std::string prompt_suffix;
  bool add_bos = false;
  bool normalize = true;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>(
      {Field("embedding_dim", &Params::embedding_dim)
           .Required()
           .Range(1, 65536)
           .Description("生成 token 的隐藏向量维数，必须与 Backend 返回的每个 "
                        "token 向量维度一致。"),
       Field("max_tokens", &Params::max_tokens)
           .Default(1)
           .Range(1, 64)
           .Description("用于生成嵌入的 token 数上限；控制参与 last/mean "
                        "池化的生成 token，"
                        "不是输入文本长度。"),
       Field("pooling", &Params::pooling)
           .Default("last")
           .Enum({"last", "mean"})
           .Description("last 取最后一个生成 token 的向量；mean 对全部生成 "
                        "token 向量求均值。"),
       Field("normalize", &Params::normalize)
           .Default(true)
           .Description("对输出向量进行 L2 归一化。"),
       Field("prompt_prefix", &Params::prompt_prefix)
           .Default("")
           .Description(
               "直接拼接在输入文本之前的模型提示词，不自动插入分隔符。"),
       Field("prompt_suffix", &Params::prompt_suffix)
           .Default("")
           .Description(
               "直接拼接在输入文本之后的模型提示词，不自动插入分隔符。"),
       Field("add_bos", &Params::add_bos)
           .Default(false)
           .Description("编码输入时请求添加模型的 BOS 起始 "
                        "token，须与所选模型的分词约定一致。")});
}

}  // namespace

std::shared_ptr<IModel> GeneratedTextEmbeddingModel::Create(
    const ModelCreateContext& context, std::string* diagnostic) {
  try {
    const auto& params = context.Params<Params>();
    auto session = std::dynamic_pointer_cast<IGeneratedTokenEmbeddingSession>(
        context.backend_session);
    if (!session || session->GetBatchPolicy().max_batch_size != 1 ||
        session->GetBatchPolicy().fixed_batch_size != 0) {
      throw std::runtime_error(
          "generated_text_embedding requires a single-input generated-token "
          "embedding session");
    }
    auto model = std::make_shared<GeneratedTextEmbeddingModel>();
    model->session_ = std::move(session);
    model->embedding_dim_ = params.embedding_dim;
    model->max_tokens_ = params.max_tokens;
    model->pooling_ = params.pooling;
    model->prompt_prefix_ = params.prompt_prefix;
    model->prompt_suffix_ = params.prompt_suffix;
    model->add_bos_ = params.add_bos;
    model->normalize_ = params.normalize;
    return model;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(diagnostic, e.what());
    return nullptr;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic,
                          "Unknown generated embedding creation error");
    return nullptr;
  }
}

int GeneratedTextEmbeddingModel::Embed(const TextBatch& inputs,
                                       EmbeddingBatch* outputs,
                                       std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  if (!outputs) {
    SetDiagnosticNoexcept(diagnostic, "Model output pointer is null");
    return -1;
  }
  outputs->clear();
  if (!session_) {
    SetDiagnosticNoexcept(diagnostic, "Model session is null");
    return -1;
  }
  return FixedBatchExecutor::ExecuteItems<std::string, std::vector<float>>(
      inputs, session_->GetBatchPolicy(),
      [this, diagnostic](const TraceableItem<std::string>& input,
                         std::vector<float>* output) {
        const auto& text = input.data;
        if (text.empty()) {
          SetDiagnosticNoexcept(diagnostic, "Embedding text is empty");
          return -1;
        }
        GeneratedTokenEmbeddings tokens;
        std::string reason;
        const int result = session_->GenerateEmbeddings(
            prompt_prefix_ + text + prompt_suffix_, add_bos_, max_tokens_,
            &tokens, &reason);
        if (result != 0) {
          ALG_LOG_ERROR("[GeneratedTextEmbeddingModel] %s\n", reason.c_str());
          SetDiagnosticNoexcept(diagnostic, reason);
          return result;
        }
        if (tokens.values.empty() ||
            tokens.values.size() > static_cast<size_t>(max_tokens_) ||
            tokens.token_ids.size() != tokens.values.size()) {
          ALG_LOG_ERROR(
              "[GeneratedTextEmbeddingModel] Missing or invalid generated "
              "token vectors (including immediate EOS)\n");
          SetDiagnosticNoexcept(diagnostic,
                                "Missing or invalid generated token vectors "
                                "(including immediate EOS)");
          return -1;
        }
        std::vector<double> pooled(static_cast<size_t>(embedding_dim_), 0.0);
        for (size_t row = 0; row < tokens.values.size(); ++row) {
          const auto& values = tokens.values[row];
          if (values.size() != pooled.size()) {
            ALG_LOG_ERROR(
                "[GeneratedTextEmbeddingModel] Expected dimension %d, received "
                "%zu; check params.embedding_dim\n",
                embedding_dim_, values.size());
            SetDiagnosticNoexcept(
                diagnostic, "Generated token embedding dimension mismatch");
            return -1;
          }
          for (size_t col = 0; col < values.size(); ++col) {
            if (!std::isfinite(values[col])) {
              SetDiagnosticNoexcept(diagnostic,
                                    "Generated embedding contains NaN or Inf");
              return -1;
            }
            if (pooling_ == "mean")
              pooled[col] += values[col];
            else if (row + 1 == tokens.values.size())
              pooled[col] = values[col];
          }
        }
        for (double& value : pooled) {
          if (pooling_ == "mean") value /= tokens.values.size();
        }
        if (!embedding_support::FinalizeEmbeddingVector(pooled, normalize_,
                                                        output)) {
          SetDiagnosticNoexcept(diagnostic,
                                "Embedding pooling or normalization failed");
          return -1;
        }
        return 0;
      },
      outputs, diagnostic);
}

static const ModelDefinition& GeneratedTextEmbeddingDefinition() {
  static const ModelDefinition definition = [] {
    auto definition = MakeModelDefinition<GeneratedTextEmbeddingModel>();
    definition.description =
        "Experimental text vectors from greedy generated-token hidden states; "
        "last/mean pooling, not encoder embeddings";
    definition.required_protocol = ExecutionProtocol::kGeneratedTokenEmbedding;
    definition.params = ParamSpec();
    return definition;
  }();
  return definition;
}
REGISTER_MODEL_WITH_DEFINITION(GeneratedTextEmbeddingModel,
                               GeneratedTextEmbeddingDefinition());

}  // namespace llm_edgeflow
