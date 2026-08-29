#include "engine/models/bge_embedding/bge_embedding_model.h"

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

namespace {

// 简易且确定性的中英文字符分词器 (映射至 BERT 基础词表空间)
void SimpleBertTokenize(const std::string& text, size_t max_len,
                        std::vector<int64_t>* input_ids,
                        std::vector<int64_t>* attention_mask) {
  input_ids->assign(max_len, 0);
  attention_mask->assign(max_len, 0);

  size_t idx = 0;
  (*input_ids)[idx] = 101;  // [CLS]
  (*attention_mask)[idx] = 1;
  idx++;

  size_t text_len = text.size();
  size_t byte_idx = 0;

  while (byte_idx < text_len && idx + 1 < max_len) {
    unsigned char c = static_cast<unsigned char>(text[byte_idx]);
    int64_t token_id = 0;
    if (c < 0x80) {
      token_id = static_cast<int64_t>(c) + 1000;
      byte_idx += 1;
    } else if ((c & 0xE0) == 0xC0 && byte_idx + 1 < text_len) {
      token_id = 10000 + ((c & 0x1F) << 6) +
                 (static_cast<unsigned char>(text[byte_idx + 1]) & 0x3F);
      byte_idx += 2;
    } else if ((c & 0xF0) == 0xE0 && byte_idx + 2 < text_len) {
      token_id =
          20000 + ((c & 0x0F) << 12) +
          ((static_cast<unsigned char>(text[byte_idx + 1]) & 0x3F) << 6) +
          (static_cast<unsigned char>(text[byte_idx + 2]) & 0x3F);
      byte_idx += 3;
    } else if ((c & 0xF8) == 0xF0 && byte_idx + 3 < text_len) {
      token_id =
          50000 + ((c & 0x07) << 18) +
          ((static_cast<unsigned char>(text[byte_idx + 1]) & 0x3F) << 12) +
          ((static_cast<unsigned char>(text[byte_idx + 2]) & 0x3F) << 6) +
          (static_cast<unsigned char>(text[byte_idx + 3]) & 0x3F);
      byte_idx += 4;
    } else {
      token_id = 100;  // [UNK]
      byte_idx += 1;
    }
    (*input_ids)[idx] = token_id;
    (*attention_mask)[idx] = 1;
    idx++;
  }

  if (idx < max_len) {
    (*input_ids)[idx] = 102;  // [SEP]
    (*attention_mask)[idx] = 1;
  }
}

}  // namespace

BgeEmbeddingModel::BgeEmbeddingModel(
    std::shared_ptr<ITensorGraphSession> session, size_t max_length,
    std::string pooling_strategy, bool normalize, size_t max_batch_size,
    size_t embedding_dim)
    : session_(std::move(session)),
      max_length_(max_length),
      pooling_strategy_(std::move(pooling_strategy)),
      default_normalize_(normalize),
      max_batch_size_(max_batch_size),
      embedding_dim_(embedding_dim) {}

std::shared_ptr<IModel> BgeEmbeddingModel::Create(const ModelCreateContext& ctx,
                                                  std::string* diagnostic) {
  if (!ctx.backend_session) {
    if (diagnostic) *diagnostic = "Backend session is null";
    return nullptr;
  }

  auto tensor_session =
      std::dynamic_pointer_cast<ITensorGraphSession>(ctx.backend_session);
  if (!tensor_session) {
    if (diagnostic) {
      *diagnostic =
          "Backend session does not implement ITensorGraphSession protocol";
    }
    return nullptr;
  }

  size_t max_length = ctx.model_config.value("max_length", 512);
  std::string pooling = ctx.model_config.value("pooling_strategy", "cls");
  bool normalize = ctx.model_config.value("normalize", true);
  size_t max_batch_size = ctx.model_config.value("max_batch_size", 4);
  size_t embedding_dim = ctx.model_config.value("embedding_dim", 384);

  return std::make_shared<BgeEmbeddingModel>(
      std::move(tensor_session), max_length, std::move(pooling), normalize,
      max_batch_size, embedding_dim);
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
  return session_ ? session_->Concurrency() : InferenceConcurrency::kConcurrent;
}

size_t BgeEmbeddingModel::GetMaxBatchSize() const noexcept {
  return max_batch_size_;
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

  std::string dummy_pad = "<PAD>";
  bool should_normalize = options.normalize && default_normalize_;

  return FixedBatchExecutor::Execute<std::string, std::vector<float>>(
      inputs, max_batch_size_, dummy_pad,
      [this, should_normalize](
          const std::vector<std::string>& batch_texts,
          std::vector<std::vector<float>>* batch_embeddings) {
        return this->RawEmbedBatch(batch_texts, batch_embeddings,
                                   should_normalize);
      },
      outputs);
}

int BgeEmbeddingModel::RawEmbedBatch(
    const std::vector<std::string>& batch_texts,
    std::vector<std::vector<float>>* batch_embeddings,
    bool normalize_flag) noexcept {
  if (!batch_embeddings) return -1;
  batch_embeddings->clear();

  size_t batch_size = batch_texts.size();
  if (batch_size == 0) return 0;

  try {
    std::string diag;
    TensorDesc in_desc;
    in_desc.element_type = ElementType::kInt64;
    in_desc.shape = {static_cast<int64_t>(batch_size),
                     static_cast<int64_t>(max_length_)};

    Tensor input_ids_tensor;
    Tensor attention_mask_tensor;
    Tensor token_type_ids_tensor;

    if (!CreateHostTensor(in_desc, &input_ids_tensor, &diag) ||
        !CreateHostTensor(in_desc, &attention_mask_tensor, &diag) ||
        !CreateHostTensor(in_desc, &token_type_ids_tensor, &diag)) {
      ALG_LOG_ERROR("[BgeEmbeddingModel] Failed to create input tensors: %s\n",
                    diag.c_str());
      return -1;
    }

    int64_t* ids_ptr =
        static_cast<int64_t*>(input_ids_tensor.buffer->MutableData());
    int64_t* mask_ptr =
        static_cast<int64_t*>(attention_mask_tensor.buffer->MutableData());
    int64_t* type_ptr =
        static_cast<int64_t*>(token_type_ids_tensor.buffer->MutableData());

    std::vector<int64_t> sample_ids(max_length_, 0);
    std::vector<int64_t> sample_mask(max_length_, 0);

    for (size_t b = 0; b < batch_size; ++b) {
      SimpleBertTokenize(batch_texts[b], max_length_, &sample_ids,
                         &sample_mask);
      std::memcpy(ids_ptr + b * max_length_, sample_ids.data(),
                  max_length_ * sizeof(int64_t));
      std::memcpy(mask_ptr + b * max_length_, sample_mask.data(),
                  max_length_ * sizeof(int64_t));
      std::memset(type_ptr + b * max_length_, 0, max_length_ * sizeof(int64_t));
    }

    TensorMap input_map;
    input_map["input_ids"] = std::move(input_ids_tensor);
    input_map["attention_mask"] = std::move(attention_mask_tensor);

    // 检查模型端口是否需要 token_type_ids
    for (const auto& in_spec : session_->Inputs()) {
      if (in_spec.name == "token_type_ids") {
        input_map["token_type_ids"] = std::move(token_type_ids_tensor);
        break;
      }
    }

    TensorMap output_map;
    int ret = session_->Run(input_map, &output_map, &diag);
    if (ret != 0) {
      ALG_LOG_ERROR("[BgeEmbeddingModel] session_->Run failed: %s\n",
                    diag.c_str());
      return ret;
    }

    // 提取输出 Tensor
    const Tensor* out_tensor = nullptr;
    auto it_last_hidden = output_map.find("last_hidden_state");
    if (it_last_hidden != output_map.end()) {
      out_tensor = &it_last_hidden->second;
    } else if (!output_map.empty()) {
      out_tensor = &output_map.begin()->second;
    }

    if (!out_tensor || !out_tensor->buffer ||
        out_tensor->desc.element_type != ElementType::kFloat32) {
      ALG_LOG_ERROR("[BgeEmbeddingModel] Invalid output tensor from session\n");
      return -1;
    }

    const float* data = static_cast<const float*>(out_tensor->buffer->Data());
    const auto& shape = out_tensor->desc.shape;

    batch_embeddings->resize(batch_size);

    if (shape.size() == 3) {
      // 3D 结构 [batch_size, seq_len, dim]
      size_t seq_len = static_cast<size_t>(shape[1]);
      size_t dim = static_cast<size_t>(shape[2]);

      for (size_t b = 0; b < batch_size; ++b) {
        std::vector<float>& vec = (*batch_embeddings)[b];
        vec.assign(dim, 0.0f);

        if (pooling_strategy_ == "mean") {
          float sum_mask = 0.0f;
          for (size_t s = 0; s < seq_len && s < max_length_; ++s) {
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
      // 2D 结构 [batch_size, dim]
      size_t dim = static_cast<size_t>(shape[1]);
      for (size_t b = 0; b < batch_size; ++b) {
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
    } else {
      ALG_LOG_ERROR("[BgeEmbeddingModel] Unexpected output tensor rank: %zu\n",
                    shape.size());
      return -1;
    }

    return 0;
  } catch (const std::exception& e) {
    ALG_LOG_ERROR("[BgeEmbeddingModel] RawEmbedBatch exception: %s\n",
                  e.what());
    batch_embeddings->clear();
    return -1;
  } catch (...) {
    ALG_LOG_ERROR("[BgeEmbeddingModel] RawEmbedBatch unknown exception\n");
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
      {"max_length", ConfigValueKind::kInteger, false, 512, 1.0, 4096.0},
      {"pooling_strategy",
       ConfigValueKind::kString,
       false,
       "cls",
       std::nullopt,
       std::nullopt,
       {"cls", "mean"}},
      {"normalize", ConfigValueKind::kBoolean, false, true},
      {"max_batch_size", ConfigValueKind::kInteger, false, 4, 1.0, 64.0},
      {"embedding_dim", ConfigValueKind::kInteger, false, 384, 1.0, 4096.0},
  };
  return def;
}();

REGISTER_MODEL_WITH_DEFINITION(BgeEmbeddingModel, kBgeEmbeddingModelDefinition);

}  // namespace alg_framework
