#include "engine/models/bge_embedding/bge_embedding_model.h"

#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "contracts/diagnostic.h"
#include "contracts/parameters.h"
#include "edgeflow/log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/models/bge_common/bert_model_support.h"
#include "engine/models/common/embedding_numeric_support.h"
#include "engine/models/common/from_model.h"

namespace llm_edgeflow {

namespace {

struct Params {
  std::string tokenizer_file;
  bool do_lower_case = true;
  std::optional<int64_t> max_tokens;
  std::string pooling;
  std::string output_name;
  std::optional<int64_t> embedding_dim;
  bool normalize = true;
};

Parameters<Params> ParamSpec() {
  auto spec = Parameters<Params>(
      {Field("tokenizer_file", &Params::tokenizer_file)
           .Required()
           .File()
           .Description("匹配该权重的 BERT WordPiece "
                        "词表；相对路径基于 Pipeline JSON 所在目录，例如 "
                        "vocab.txt。"),
       Field("do_lower_case", &Params::do_lower_case)
           .Default(true)
           .Description("分词前将英文大写和全角英文字母归一为半角小写，须与模型"
                        "训练时的分词设置一致。"),
       Field("max_tokens", &Params::max_tokens)
           .Range(2, 4096)
           .Description("每条文本编码后的固定 token 长度，包含 "
                        "[CLS]/[SEP]；超长截断，不足补齐；"
                        "不写时从模型输入张量读取，读不到时为 512。"),
       Field("pooling", &Params::pooling)
           .Default("cls")
           .Enum({"cls", "mean"})
           .Description("三维 token 输出的池化：cls 取首 token，mean 按 "
                        "attention_mask 求均值；"
                        "二维向量输出直接使用。"),
       Field("output_name", &Params::output_name)
           .Default("last_hidden_state")
           .Description("读取的模型输出张量名，例如 "
                        "last_hidden_state，必须与模型导出名称一致。"),
       Field("embedding_dim", &Params::embedding_dim)
           .Range(1, 65536)
           .Description("每条输出向量的维数；不写时从模型输出张量读取，读不到时"
                        "必须填写。"),
       Field("normalize", &Params::normalize)
           .Default(true)
           .Description("对输出向量进行 L2 归一化。")});
  spec.Validate([](const Params& params, std::string* diagnostic) {
    if (params.tokenizer_file.empty()) {
      if (diagnostic) *diagnostic = "Field 'tokenizer_file' cannot be empty";
      return false;
    }
    if (params.output_name.empty()) {
      if (diagnostic) *diagnostic = "Field 'output_name' cannot be empty";
      return false;
    }
    return true;
  });
  return spec;
}

bool ValidateEmbeddingOutput(const Tensor& tensor, size_t expected_batch,
                             size_t expected_sequence, size_t expected_dim,
                             const float** data_ptr,
                             std::string* diagnostic) noexcept {
  if (expected_batch == 0 || expected_sequence == 0 || expected_dim == 0) {
    if (diagnostic) {
      *diagnostic = "Expected batch, sequence and dim must be positive";
    }
    return false;
  }

  if (!ValidateRuntimeBatchTensor(tensor, expected_batch, 2, 3, diagnostic)) {
    return false;
  }
  const auto& shape = tensor.desc.shape;
  size_t dim = (shape.size() == 2) ? static_cast<size_t>(shape[1])
                                   : static_cast<size_t>(shape[2]);
  if (shape.size() == 3 && static_cast<size_t>(shape[1]) != expected_sequence) {
    if (diagnostic) {
      *diagnostic = "Output tensor sequence dimension mismatch. Expected: " +
                    std::to_string(expected_sequence) +
                    ", got: " + std::to_string(shape[1]);
    }
    return false;
  }
  if (dim != expected_dim) {
    if (diagnostic) {
      *diagnostic = "Output tensor embedding_dim mismatch. Expected: " +
                    std::to_string(expected_dim) +
                    ", got: " + std::to_string(dim);
    }
    return false;
  }

  // 使用公共安全访问器 GetTensorData<float> 统一完成 dtype、对齐、溢出与精确
  // byte size 校验
  const float* data = GetTensorData<float>(tensor, diagnostic);
  if (!data) {
    return false;
  }

  const size_t element_count = tensor.buffer->ByteSize() / sizeof(float);
  for (size_t i = 0; i < element_count; ++i) {
    if (!std::isfinite(data[i])) {
      if (diagnostic) {
        *diagnostic =
            "Output tensor contains NaN or Inf at index " + std::to_string(i);
      }
      return false;
    }
  }

  if (data_ptr) {
    *data_ptr = data;
  }
  return true;
}

bool ValidateEmbeddingOutputMetadata(const ITensorGraphSession& session,
                                     const std::string& output_name,
                                     size_t max_tokens, size_t embedding_dim,
                                     std::string* diagnostic) {
  const TensorSpec* output = RequireFloatOutputMetadata(
      session, output_name, "BgeEmbeddingModel", 2, 3, diagnostic);
  if (!output) return false;

  if (output->shape.size() == 3) {
    if (output->shape[1] == 0) {
      if (diagnostic) {
        *diagnostic = "BgeEmbeddingModel output '" + output_name +
                      "' sequence dimension cannot be 0";
      }
      return false;
    }
    if (output->shape[1] > 0 &&
        static_cast<size_t>(output->shape[1]) != max_tokens) {
      if (diagnostic) {
        *diagnostic = "BgeEmbeddingModel output '" + output_name +
                      "' static sequence length " +
                      std::to_string(output->shape[1]) +
                      " does not match configured max_tokens " +
                      std::to_string(max_tokens);
      }
      return false;
    }
  }

  const size_t dimension_index = output->shape.size() - 1;
  if (output->shape[dimension_index] == 0) {
    if (diagnostic) {
      *diagnostic = "BgeEmbeddingModel output '" + output_name +
                    "' embedding dimension cannot be 0";
    }
    return false;
  }
  if (output->shape[dimension_index] > 0 &&
      static_cast<size_t>(output->shape[dimension_index]) != embedding_dim) {
    if (diagnostic) {
      *diagnostic = "BgeEmbeddingModel output '" + output_name +
                    "' static embedding dimension " +
                    std::to_string(output->shape[dimension_index]) +
                    " does not match configured embedding_dim " +
                    std::to_string(embedding_dim);
    }
    return false;
  }
  return true;
}

}  // namespace

BgeEmbeddingModel::BgeEmbeddingModel(
    std::shared_ptr<ITensorGraphSession> session,
    BertWordPieceTokenizer tokenizer, size_t max_tokens, std::string pooling,
    std::string output_name, size_t embedding_dim, bool normalize)
    : session_(std::move(session)),
      tokenizer_(std::move(tokenizer)),
      max_tokens_(max_tokens),
      pooling_(std::move(pooling)),
      output_name_(std::move(output_name)),
      embedding_dim_(embedding_dim),
      normalize_(normalize) {}

std::shared_ptr<IModel> BgeEmbeddingModel::Create(const ModelCreateContext& ctx,
                                                  std::string* diagnostic) {
  const auto& params = ctx.Params<Params>();
  auto tensor_session =
      RequireTensorGraphSession(ctx.backend_session, diagnostic);
  if (!tensor_session) return nullptr;

  int64_t embedding_dim = 0;
  int64_t max_tokens = 0;
  if (!ResolveFromModel("embedding_dim", params.embedding_dim,
                        StaticOutputDim(*tensor_session, params.output_name),
                        std::nullopt, &embedding_dim, diagnostic) ||
      !ResolveFromModel("max_tokens", params.max_tokens,
                        StaticSequenceLength(*tensor_session), 512, &max_tokens,
                        diagnostic)) {
    return nullptr;
  }

  // 固定形状由资源提供，仍须满足本实现支持的编码长度和向量容量。
  if (max_tokens < 2 || max_tokens > 4096) {
    SetDiagnosticNoexcept(diagnostic, "Model max_tokens must be in [2, 4096]");
    return nullptr;
  }
  if (embedding_dim < 1 || embedding_dim > 65536) {
    SetDiagnosticNoexcept(diagnostic,
                          "Model embedding_dim must be in [1, 65536]");
    return nullptr;
  }

  if (!ValidateBertInputMetadata(*tensor_session, max_tokens,
                                 "BgeEmbeddingModel", diagnostic) ||
      !ValidateEmbeddingOutputMetadata(*tensor_session, params.output_name,
                                       max_tokens, embedding_dim, diagnostic)) {
    return nullptr;
  }

  BertWordPieceTokenizer tokenizer;
  if (!LoadBertTokenizer(params.tokenizer_file, params.do_lower_case,
                         &tokenizer, diagnostic)) {
    return nullptr;
  }

  return std::make_shared<BgeEmbeddingModel>(
      std::move(tensor_session), std::move(tokenizer), max_tokens,
      params.pooling, params.output_name, embedding_dim, params.normalize);
}

int BgeEmbeddingModel::Embed(const TextBatch& inputs, EmbeddingBatch* outputs,
                             std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  if (!outputs) {
    SetDiagnosticNoexcept(diagnostic, "Model output pointer is null");
    return -1;
  }
  outputs->clear();

  if (inputs.empty()) {
    return 0;
  }

  if (!session_) {
    SetDiagnosticNoexcept(diagnostic, "Model session is null");
    return -1;
  }

  const bool should_normalize = normalize_;
  const BatchPolicy policy = session_->GetBatchPolicy();

  return FixedBatchExecutor::Execute<std::string, std::vector<float>>(
      inputs, policy,
      [this, should_normalize, &inputs, diagnostic](
          const BatchSlice& slice,
          std::vector<std::vector<float>>* batch_embeddings) {
        return this->RawEmbedSlice(inputs, slice, batch_embeddings,
                                   should_normalize, diagnostic);
      },
      outputs, diagnostic);
}

int BgeEmbeddingModel::RawEmbedSlice(
    const TextBatch& all_inputs, const BatchSlice& slice,
    std::vector<std::vector<float>>* batch_embeddings, bool normalize_flag,
    std::string* diagnostic) noexcept {
  if (!batch_embeddings) return -1;
  batch_embeddings->clear();

  size_t exec_count = slice.execution_count;
  if (exec_count == 0) return 0;

  try {
    std::string diag;
    BertInputTensors input_tensors;
    const bool include_token_type_ids =
        HasTensorInput(session_->Inputs(), "token_type_ids");
    if (!input_tensors.Create(exec_count, max_tokens_, include_token_type_ids,
                              &diag)) {
      ALG_LOG_ERROR("[BgeEmbeddingModel] Failed to create input tensors: %s\n",
                    diag.c_str());
      SetDiagnosticNoexcept(diagnostic, diag);
      return -1;
    }
    int64_t* ids_ptr = input_tensors.ids;
    int64_t* mask_ptr = input_tensors.mask;
    int64_t* type_ptr = input_tensors.types;

    std::vector<int64_t> sample_ids(max_tokens_, 0);
    std::vector<int64_t> sample_mask(max_tokens_, 0);

    for (size_t i = 0; i < exec_count; ++i) {
      if (i < slice.valid_count) {
        const auto& text = all_inputs[slice.offset + i].data;
        if (!tokenizer_.Encode(text, max_tokens_, &sample_ids, &sample_mask,
                               &diag)) {
          ALG_LOG_ERROR("[BgeEmbeddingModel] Tokenizer encode error: %s\n",
                        diag.c_str());
          SetDiagnosticNoexcept(diagnostic, diag);
          batch_embeddings->clear();
          return -1;
        }
      } else {
        // 填充用的空条目
        tokenizer_.Encode("", max_tokens_, &sample_ids, &sample_mask, nullptr);
      }

      std::memcpy(ids_ptr + i * max_tokens_, sample_ids.data(),
                  max_tokens_ * sizeof(int64_t));
      std::memcpy(mask_ptr + i * max_tokens_, sample_mask.data(),
                  max_tokens_ * sizeof(int64_t));
      if (type_ptr) {
        std::memset(type_ptr + i * max_tokens_, 0,
                    max_tokens_ * sizeof(int64_t));
      }
    }

    TensorMap input_map = input_tensors.ReleaseToMap();

    TensorMap output_map;
    int ret = session_->Run(input_map, &output_map, &diag);
    if (ret != 0) {
      ALG_LOG_ERROR("[BgeEmbeddingModel] session_->Run failed: %s\n",
                    diag.c_str());
      SetDiagnosticNoexcept(diagnostic, diag);
      batch_embeddings->clear();
      return ret;
    }

    // 严格按 output_name_ 提取输出 Tensor
    auto it_out = output_map.find(output_name_);
    if (it_out == output_map.end()) {
      ALG_LOG_ERROR(
          "[BgeEmbeddingModel] Expected output tensor '%s' not found in "
          "session "
          "outputs\n",
          output_name_.c_str());
      SetDiagnosticNoexcept(diagnostic, "Expected output tensor is missing");
      batch_embeddings->clear();
      return -1;
    }

    const float* data = nullptr;
    if (!ValidateEmbeddingOutput(it_out->second, exec_count, max_tokens_,
                                 embedding_dim_, &data, &diag)) {
      ALG_LOG_ERROR("[BgeEmbeddingModel] ValidateEmbeddingOutput failed: %s\n",
                    diag.c_str());
      SetDiagnosticNoexcept(diagnostic, diag);
      batch_embeddings->clear();
      return -1;
    }

    const auto& shape = it_out->second.desc.shape;
    batch_embeddings->resize(exec_count);

    if (shape.size() == 3) {
      // 3D 结构 [exec_count, seq_len, dim]
      size_t seq_len = static_cast<size_t>(shape[1]);
      size_t dim = static_cast<size_t>(shape[2]);

      for (size_t b = 0; b < exec_count; ++b) {
        std::vector<double> pooled(dim, 0.0);

        if (pooling_ == "mean") {
          double sum_mask = 0.0;
          for (size_t s = 0; s < seq_len; ++s) {
            int64_t m = mask_ptr[b * max_tokens_ + s];
            if (m > 0) {
              sum_mask += 1.0;
              const float* token_vec = data + (b * seq_len + s) * dim;
              for (size_t d = 0; d < dim; ++d) {
                pooled[d] += static_cast<double>(token_vec[d]);
              }
            }
          }
          if (sum_mask > 0.0) {
            for (size_t d = 0; d < dim; ++d) {
              pooled[d] /= sum_mask;
            }
          }
        } else {
          // CLS pooling: 取 [b, 0, :]
          const float* cls_vec = data + (b * seq_len + 0) * dim;
          for (size_t d = 0; d < dim; ++d) {
            pooled[d] = static_cast<double>(cls_vec[d]);
          }
        }

        if (!embedding_support::FinalizeEmbeddingVector(
                pooled, normalize_flag, &(*batch_embeddings)[b])) {
          SetDiagnosticNoexcept(diagnostic,
                                "Embedding pooling or normalization failed");
          batch_embeddings->clear();
          return -1;
        }
      }
    } else if (shape.size() == 2) {
      // 2D 结构 [exec_count, dim]
      size_t dim = static_cast<size_t>(shape[1]);
      for (size_t b = 0; b < exec_count; ++b) {
        if (!embedding_support::FinalizeEmbeddingVector(
                data + b * dim, dim, normalize_flag, &(*batch_embeddings)[b])) {
          SetDiagnosticNoexcept(diagnostic,
                                "Embedding pooling or normalization failed");
          batch_embeddings->clear();
          return -1;
        }
      }
    }

    return 0;
  } catch (const std::exception& e) {
    ALG_LOG_ERROR("[BgeEmbeddingModel] RawEmbedSlice exception: %s\n",
                  e.what());
    SetDiagnosticNoexcept(diagnostic, e.what());
    batch_embeddings->clear();
    return -1;
  } catch (...) {
    ALG_LOG_ERROR("[BgeEmbeddingModel] RawEmbedSlice unknown exception\n");
    SetDiagnosticNoexcept(diagnostic, "Unknown model execution exception");
    batch_embeddings->clear();
    return -1;
  }
}

static const ModelDefinition& BgeEmbeddingModelDefinition() {
  static const ModelDefinition definition = [] {
    auto def = MakeModelDefinition<BgeEmbeddingModel>();
    def.description = "BGE text embedding model using TensorGraph protocol";
    def.required_protocol = ExecutionProtocol::kTensorGraph;
    def.params = ParamSpec();
    return def;
  }();
  return definition;
}

REGISTER_MODEL_WITH_DEFINITION(BgeEmbeddingModel,
                               BgeEmbeddingModelDefinition());

}  // namespace llm_edgeflow
