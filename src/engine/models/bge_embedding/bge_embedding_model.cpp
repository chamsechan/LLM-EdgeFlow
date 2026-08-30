#include "engine/models/bge_embedding/bge_embedding_model.h"

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/models/bge_common/bert_model_support.h"

namespace alg_framework {

namespace {

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
                                     size_t max_length, size_t embedding_dim,
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
        static_cast<size_t>(output->shape[1]) != max_length) {
      if (diagnostic) {
        *diagnostic = "BgeEmbeddingModel output '" + output_name +
                      "' static sequence length " +
                      std::to_string(output->shape[1]) +
                      " does not match configured max_length " +
                      std::to_string(max_length);
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
    BertWordPieceTokenizer tokenizer, size_t max_length,
    std::string pooling_strategy, bool normalize, std::string output_name,
    size_t embedding_dim, size_t max_batch_size)
    : session_(std::move(session)),
      tokenizer_(std::move(tokenizer)),
      max_length_(max_length),
      pooling_strategy_(std::move(pooling_strategy)),
      default_normalize_(normalize),
      output_name_(std::move(output_name)),
      embedding_dim_(embedding_dim),
      max_batch_size_(max_batch_size) {}

std::shared_ptr<IModel> BgeEmbeddingModel::Create(const ModelCreateContext& ctx,
                                                  std::string* diagnostic) {
  auto tensor_session =
      RequireTensorGraphSession(ctx.backend_session, diagnostic);
  if (!tensor_session) return nullptr;

  if (!ctx.model_config.contains("embedding_dim") ||
      !ctx.model_config["embedding_dim"].is_number_integer()) {
    if (diagnostic) {
      *diagnostic = "Required field 'embedding_dim' is missing or not integer";
    }
    return nullptr;
  }
  size_t embedding_dim = ctx.model_config["embedding_dim"].get<size_t>();
  size_t max_batch_size = ctx.model_config.value("max_batch_size", 4);
  std::string tokenizer_file =
      ctx.model_config.value("tokenizer_file", "vocab.txt");
  bool do_lower_case = ctx.model_config.value("do_lower_case", true);
  size_t max_length = ctx.model_config.value("max_length", 512);
  std::string pooling = ctx.model_config.value("pooling_strategy", "cls");
  bool normalize = ctx.model_config.value("normalize", true);
  std::string output_name =
      ctx.model_config.value("output_name", "last_hidden_state");

  if (!ValidateModelBatchLimit(tensor_session->GetBatchPolicy(), max_batch_size,
                               diagnostic) ||
      !ValidateBertInputMetadata(*tensor_session, max_length,
                                 "BgeEmbeddingModel", diagnostic) ||
      !ValidateEmbeddingOutputMetadata(*tensor_session, output_name, max_length,
                                       embedding_dim, diagnostic)) {
    return nullptr;
  }

  BertWordPieceTokenizer tokenizer;
  if (!LoadBertTokenizer(ctx.model_resource_root, tokenizer_file, do_lower_case,
                         &tokenizer, diagnostic)) {
    return nullptr;
  }

  return std::make_shared<BgeEmbeddingModel>(
      std::move(tensor_session), std::move(tokenizer), max_length,
      std::move(pooling), normalize, std::move(output_name), embedding_dim,
      max_batch_size);
}

const std::string& BgeEmbeddingModel::ModelType() const noexcept {
  static const std::string type = kModelType;
  return type;
}

const std::string& BgeEmbeddingModel::Capability() const noexcept {
  static const std::string cap = kCapability;
  return cap;
}

InferenceConcurrency BgeEmbeddingModel::Concurrency() const noexcept {
  return InferenceConcurrency::kConcurrent;
}

size_t BgeEmbeddingModel::GetMaxBatchSize() const noexcept {
  return ConstrainModelBatchPolicy(session_.get(), max_batch_size_)
      .max_batch_size;
}

int BgeEmbeddingModel::Embed(const TextBatch& inputs,
                             const EmbeddingOptions& options,
                             EmbeddingBatch* outputs) noexcept {
  if (!outputs) return -1;
  outputs->clear();

  if (inputs.empty()) {
    return 0;
  }

  if (!session_) {
    return -1;
  }

  bool should_normalize = options.normalize && default_normalize_;
  BatchPolicy policy =
      ConstrainModelBatchPolicy(session_.get(), max_batch_size_);

  return FixedBatchExecutor::Execute<std::string, std::vector<float>>(
      inputs, policy,
      [this, should_normalize, &inputs](
          const BatchSlice& slice,
          std::vector<std::vector<float>>* batch_embeddings) {
        return this->RawEmbedSlice(inputs, slice, batch_embeddings,
                                   should_normalize);
      },
      outputs);
}

int BgeEmbeddingModel::RawEmbedSlice(
    const TextBatch& all_inputs, const BatchSlice& slice,
    std::vector<std::vector<float>>* batch_embeddings,
    bool normalize_flag) noexcept {
  if (!batch_embeddings) return -1;
  batch_embeddings->clear();

  size_t exec_count = slice.execution_count;
  if (exec_count == 0) return 0;

  try {
    std::string diag;
    BertInputTensors input_tensors;
    const bool include_token_type_ids =
        HasTensorInput(session_->Inputs(), "token_type_ids");
    if (!input_tensors.Create(exec_count, max_length_, include_token_type_ids,
                              &diag)) {
      ALG_LOG_ERROR("[BgeEmbeddingModel] Failed to create input tensors: %s\n",
                    diag.c_str());
      return -1;
    }
    int64_t* ids_ptr = input_tensors.ids;
    int64_t* mask_ptr = input_tensors.mask;
    int64_t* type_ptr = input_tensors.types;

    std::vector<int64_t> sample_ids(max_length_, 0);
    std::vector<int64_t> sample_mask(max_length_, 0);

    for (size_t i = 0; i < exec_count; ++i) {
      if (i < slice.valid_count) {
        const auto& text = all_inputs[slice.offset + i].data;
        if (!tokenizer_.Encode(text, max_length_, &sample_ids, &sample_mask,
                               &diag)) {
          ALG_LOG_ERROR("[BgeEmbeddingModel] Tokenizer encode error: %s\n",
                        diag.c_str());
          batch_embeddings->clear();
          return -1;
        }
      } else {
        // Dummy padding item
        tokenizer_.Encode("", max_length_, &sample_ids, &sample_mask, nullptr);
      }

      std::memcpy(ids_ptr + i * max_length_, sample_ids.data(),
                  max_length_ * sizeof(int64_t));
      std::memcpy(mask_ptr + i * max_length_, sample_mask.data(),
                  max_length_ * sizeof(int64_t));
      if (type_ptr) {
        std::memset(type_ptr + i * max_length_, 0,
                    max_length_ * sizeof(int64_t));
      }
    }

    TensorMap input_map = input_tensors.ReleaseToMap();

    TensorMap output_map;
    int ret = session_->Run(input_map, &output_map, &diag);
    if (ret != 0) {
      ALG_LOG_ERROR("[BgeEmbeddingModel] session_->Run failed: %s\n",
                    diag.c_str());
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
      batch_embeddings->clear();
      return -1;
    }

    const float* data = nullptr;
    if (!ValidateEmbeddingOutput(it_out->second, exec_count, max_length_,
                                 embedding_dim_, &data, &diag)) {
      ALG_LOG_ERROR("[BgeEmbeddingModel] ValidateEmbeddingOutput failed: %s\n",
                    diag.c_str());
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
        std::vector<float>& vec = (*batch_embeddings)[b];
        vec.assign(dim, 0.0f);

        if (pooling_strategy_ == "mean") {
          float sum_mask = 0.0f;
          for (size_t s = 0; s < seq_len; ++s) {
            int64_t m = mask_ptr[b * max_length_ + s];
            if (m > 0) {
              sum_mask += 1.0f;
              const float* token_vec = data + (b * seq_len + s) * dim;
              for (size_t d = 0; d < dim; ++d) {
                vec[d] += token_vec[d];
              }
            }
          }
          if (sum_mask > 0.0f) {
            for (size_t d = 0; d < dim; ++d) {
              vec[d] /= sum_mask;
            }
          }
        } else {
          // CLS pooling: 取 [b, 0, :]
          const float* cls_vec = data + (b * seq_len + 0) * dim;
          std::memcpy(vec.data(), cls_vec, dim * sizeof(float));
        }

        if (normalize_flag) {
          float norm_sq = 0.0f;
          for (float v : vec) norm_sq += v * v;
          float norm = std::sqrt(norm_sq);
          if (norm > 1e-12f) {
            for (float& v : vec) v /= norm;
          }
        }
      }
    } else if (shape.size() == 2) {
      // 2D 结构 [exec_count, dim]
      size_t dim = static_cast<size_t>(shape[1]);
      for (size_t b = 0; b < exec_count; ++b) {
        std::vector<float>& vec = (*batch_embeddings)[b];
        vec.assign(dim, 0.0f);
        std::memcpy(vec.data(), data + b * dim, dim * sizeof(float));

        if (normalize_flag) {
          float norm_sq = 0.0f;
          for (float v : vec) norm_sq += v * v;
          float norm = std::sqrt(norm_sq);
          if (norm > 1e-12f) {
            for (float& v : vec) v /= norm;
          }
        }
      }
    }

    return 0;
  } catch (const std::exception& e) {
    ALG_LOG_ERROR("[BgeEmbeddingModel] RawEmbedSlice exception: %s\n",
                  e.what());
    batch_embeddings->clear();
    return -1;
  } catch (...) {
    ALG_LOG_ERROR("[BgeEmbeddingModel] RawEmbedSlice unknown exception\n");
    batch_embeddings->clear();
    return -1;
  }
}

static const ModelDefinition kBgeEmbeddingModelDefinition = [] {
  ModelDefinition def;
  def.model_type = BgeEmbeddingModel::kModelType;
  def.capability = BgeEmbeddingModel::kCapability;
  def.description = "BGE text embedding model using TensorGraph protocol";
  def.required_protocol = ExecutionProtocol::kTensorGraph;
  def.concurrency = InferenceConcurrency::kConcurrent;
  def.config_fields = {
      {"tokenizer_file", ConfigValueKind::kString, false, "vocab.txt"},
      {"do_lower_case", ConfigValueKind::kBoolean, false, true},
      {"max_length", ConfigValueKind::kInteger, false, 512, 2.0, 4096.0},
      {"pooling_strategy",
       ConfigValueKind::kString,
       false,
       "cls",
       std::nullopt,
       std::nullopt,
       {"cls", "mean"}},
      {"normalize", ConfigValueKind::kBoolean, false, true},
      {"output_name", ConfigValueKind::kString, false, "last_hidden_state"},
      {"embedding_dim", ConfigValueKind::kInteger, true, nlohmann::json(), 1.0,
       65536.0},
      {"max_batch_size", ConfigValueKind::kInteger, false, 4, 1.0, 1024.0},
  };
  return def;
}();

REGISTER_MODEL_WITH_DEFINITION(BgeEmbeddingModel, kBgeEmbeddingModelDefinition);

}  // namespace alg_framework
