#include "engine/models/bge_reranker/bge_reranker_model.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/models/bge_common/bert_model_support.h"

namespace alg_framework {

namespace {

bool ValidateRerankOutput(const Tensor& tensor, size_t expected_batch,
                          const float** data_ptr,
                          std::string* diagnostic) noexcept {
  if (expected_batch == 0) {
    if (diagnostic) {
      *diagnostic = "Expected batch must be positive";
    }
    return false;
  }

  const auto& shape = tensor.desc.shape;
  if (shape.size() != 1 && shape.size() != 2) {
    if (diagnostic) {
      *diagnostic = "Output tensor rank must be 1 or 2, got: " +
                    std::to_string(shape.size());
    }
    return false;
  }

  for (size_t d = 0; d < shape.size(); ++d) {
    if (shape[d] <= 0) {
      if (diagnostic) {
        *diagnostic =
            "Output tensor dimension " + std::to_string(d) +
            " must be strictly positive, got: " + std::to_string(shape[d]);
      }
      return false;
    }
  }

  if (static_cast<size_t>(shape[0]) != expected_batch) {
    if (diagnostic) {
      *diagnostic = "Output tensor batch dimension mismatch. Expected: " +
                    std::to_string(expected_batch) +
                    ", got: " + std::to_string(shape[0]);
    }
    return false;
  }

  if (shape.size() == 2 && shape[1] != 1) {
    if (diagnostic) {
      *diagnostic = "Output tensor second dimension must be 1, got: " +
                    std::to_string(shape[1]);
    }
    return false;
  }

  const float* data = GetTensorData<float>(tensor, diagnostic);
  if (!data) {
    return false;
  }

  if (data_ptr) {
    *data_ptr = data;
  }
  return true;
}

}  // namespace

BgeRerankerModel::BgeRerankerModel(std::shared_ptr<ITensorGraphSession> session,
                                   BertWordPieceTokenizer tokenizer,
                                   size_t max_length, std::string output_name,
                                   std::string score_activation,
                                   size_t max_batch_size)
    : session_(std::move(session)),
      tokenizer_(std::move(tokenizer)),
      max_length_(max_length),
      output_name_(std::move(output_name)),
      score_activation_(std::move(score_activation)),
      max_batch_size_(max_batch_size) {}

std::shared_ptr<IModel> BgeRerankerModel::Create(const ModelCreateContext& ctx,
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

  std::string tokenizer_file =
      ctx.model_config.value("tokenizer_file", "vocab.txt");
  if (tokenizer_file.empty()) {
    if (diagnostic) *diagnostic = "Field 'tokenizer_file' cannot be empty";
    return nullptr;
  }

  bool do_lower_case = ctx.model_config.value("do_lower_case", true);
  size_t max_length = ctx.model_config.value("max_length", 512);
  if (max_length < 3 || max_length > 4096) {
    if (diagnostic) {
      *diagnostic = "Field 'max_length' must be in range [3, 4096], got: " +
                    std::to_string(max_length);
    }
    return nullptr;
  }

  std::string output_name = ctx.model_config.value("output_name", "logits");
  if (output_name.empty()) {
    if (diagnostic) *diagnostic = "Field 'output_name' cannot be empty";
    return nullptr;
  }

  std::string score_activation =
      ctx.model_config.value("score_activation", "sigmoid");
  if (score_activation != "sigmoid" && score_activation != "identity") {
    if (diagnostic) {
      *diagnostic =
          "Field 'score_activation' must be 'sigmoid' or 'identity', got: " +
          score_activation;
    }
    return nullptr;
  }

  size_t max_batch_size = ctx.model_config.value("max_batch_size", 4);
  if (max_batch_size == 0) {
    if (diagnostic) {
      *diagnostic = "Field 'max_batch_size' must be at least 1";
    }
    return nullptr;
  }

  // 严格 Batch 契约：若 Session 为固定 Batch 且 Model 配置上限小于 Session 固定
  // Batch，明确拒绝
  auto session_policy = tensor_session->GetBatchPolicy();
  if (!ValidateModelBatchLimit(session_policy, max_batch_size, diagnostic)) {
    return nullptr;
  }

  const auto validate_batch_dimension =
      [&](int64_t dimension, const std::string& tensor_kind,
          const std::string& tensor_name) -> bool {
    if (dimension == 0) {
      if (diagnostic) {
        *diagnostic = "BgeRerankerModel " + tensor_kind + " '" + tensor_name +
                      "' batch dimension cannot be 0";
      }
      return false;
    }
    if (dimension < 0) {
      return true;
    }

    const size_t static_batch = static_cast<size_t>(dimension);
    if (session_policy.fixed_batch_size > 0 &&
        static_batch != session_policy.fixed_batch_size) {
      if (diagnostic) {
        *diagnostic = "BgeRerankerModel " + tensor_kind + " '" + tensor_name +
                      "' static batch " + std::to_string(static_batch) +
                      " does not match fixed_batch_size " +
                      std::to_string(session_policy.fixed_batch_size);
      }
      return false;
    }
    if (static_batch > session_policy.max_batch_size) {
      if (diagnostic) {
        *diagnostic = "BgeRerankerModel " + tensor_kind + " '" + tensor_name +
                      "' static batch " + std::to_string(static_batch) +
                      " exceeds session max_batch_size " +
                      std::to_string(session_policy.max_batch_size);
      }
      return false;
    }
    return true;
  };

  // 1. 校验 Session 输入元数据 (严格非空与契约匹配)
  if (tensor_session->Inputs().empty()) {
    if (diagnostic) {
      *diagnostic = "BgeRerankerModel session input metadata cannot be empty";
    }
    return nullptr;
  }

  bool has_input_ids = false;
  bool has_attention_mask = false;
  for (const auto& in_spec : tensor_session->Inputs()) {
    if (in_spec.name == "input_ids") {
      has_input_ids = true;
    } else if (in_spec.name == "attention_mask") {
      has_attention_mask = true;
    } else if (in_spec.name == "token_type_ids") {
      // 标准可选输入
    } else {
      if (diagnostic) {
        *diagnostic =
            "BgeRerankerModel session declares unrecognized required input: '" +
            in_spec.name + "'";
      }
      return nullptr;
    }

    if (in_spec.element_type != ElementType::kInt64) {
      if (diagnostic) {
        *diagnostic = "BgeRerankerModel input '" + in_spec.name +
                      "' dtype must be int64, got: " +
                      ElementTypeToString(in_spec.element_type);
      }
      return nullptr;
    }

    if (in_spec.shape.size() != 2) {
      if (diagnostic) {
        *diagnostic = "BgeRerankerModel input '" + in_spec.name +
                      "' rank must be 2 [batch, sequence], got rank: " +
                      std::to_string(in_spec.shape.size());
      }
      return nullptr;
    }

    if (in_spec.shape[1] == 0) {
      if (diagnostic) {
        *diagnostic = "BgeRerankerModel input '" + in_spec.name +
                      "' sequence dimension cannot be 0";
      }
      return nullptr;
    }

    // 静态 sequence 维度校验；负数表示动态维度，0 不是合法动态维度。
    if (in_spec.shape[1] > 0 &&
        static_cast<size_t>(in_spec.shape[1]) != max_length) {
      if (diagnostic) {
        *diagnostic = "BgeRerankerModel input '" + in_spec.name +
                      "' static sequence length " +
                      std::to_string(in_spec.shape[1]) +
                      " does not match configured max_length " +
                      std::to_string(max_length);
      }
      return nullptr;
    }

    if (!validate_batch_dimension(in_spec.shape[0], "input", in_spec.name)) {
      return nullptr;
    }
  }

  if (!has_input_ids || !has_attention_mask) {
    if (diagnostic) {
      *diagnostic =
          "BgeRerankerModel session missing required inputs (input_ids or "
          "attention_mask)";
    }
    return nullptr;
  }

  // 2. 校验 Session 输出元数据 (严格非空与契约匹配)
  if (tensor_session->Outputs().empty()) {
    if (diagnostic) {
      *diagnostic = "BgeRerankerModel session output metadata cannot be empty";
    }
    return nullptr;
  }

  const TensorSpec* target_output = nullptr;
  for (const auto& out_spec : tensor_session->Outputs()) {
    if (out_spec.name == output_name) {
      target_output = &out_spec;
      break;
    }
  }
  if (!target_output) {
    if (diagnostic) {
      *diagnostic =
          "BgeRerankerModel session outputs missing expected output tensor: '" +
          output_name + "'";
    }
    return nullptr;
  }

  if (target_output->element_type != ElementType::kFloat32) {
    if (diagnostic) {
      *diagnostic = "BgeRerankerModel output '" + output_name +
                    "' dtype must be float32, got: " +
                    ElementTypeToString(target_output->element_type);
    }
    return nullptr;
  }

  if (target_output->shape.size() != 1 && target_output->shape.size() != 2) {
    if (diagnostic) {
      *diagnostic = "BgeRerankerModel output '" + output_name +
                    "' rank must be 1 or 2, got: " +
                    std::to_string(target_output->shape.size());
    }
    return nullptr;
  }

  if (target_output->shape.size() == 2) {
    if (target_output->shape[1] == 0) {
      if (diagnostic) {
        *diagnostic = "BgeRerankerModel output '" + output_name +
                      "' rank 2 second dimension cannot be 0";
      }
      return nullptr;
    }
    if (target_output->shape[1] > 0 && target_output->shape[1] != 1) {
      if (diagnostic) {
        *diagnostic = "BgeRerankerModel output '" + output_name +
                      "' rank 2 second dimension must be 1, got: " +
                      std::to_string(target_output->shape[1]);
      }
      return nullptr;
    }
  }

  if (!validate_batch_dimension(target_output->shape[0], "output",
                                output_name)) {
    return nullptr;
  }

  std::filesystem::path resolved_vocab_path;
  if (!ResolveTokenizerResourcePath(ctx.model_resource_root, tokenizer_file,
                                    &resolved_vocab_path, diagnostic)) {
    return nullptr;
  }

  BertWordPieceTokenizer tokenizer;
  if (!tokenizer.Load(resolved_vocab_path.string(), do_lower_case,
                      diagnostic)) {
    return nullptr;
  }

  return std::make_shared<BgeRerankerModel>(
      std::move(tensor_session), std::move(tokenizer), max_length,
      std::move(output_name), std::move(score_activation), max_batch_size);
}

const std::string& BgeRerankerModel::ModelType() const noexcept {
  static const std::string type = kModelType;
  return type;
}

const std::string& BgeRerankerModel::Capability() const noexcept {
  static const std::string cap = kCapability;
  return cap;
}

InferenceConcurrency BgeRerankerModel::Concurrency() const noexcept {
  return session_ ? session_->Concurrency() : InferenceConcurrency::kConcurrent;
}

size_t BgeRerankerModel::GetMaxBatchSize() const noexcept {
  if (session_) {
    auto policy = session_->GetBatchPolicy();
    return std::min(max_batch_size_, policy.max_batch_size);
  }
  return max_batch_size_;
}

int BgeRerankerModel::Score(const QueryCandidatesBatch& inputs,
                            ScoreBatch* outputs) noexcept {
  if (!outputs) return -1;
  outputs->clear();

  if (inputs.empty()) {
    return 0;
  }

  if (!session_) {
    return -1;
  }

  BatchPolicy policy = session_->GetBatchPolicy();
  if (policy.fixed_batch_size == 0) {
    policy.max_batch_size = std::min(max_batch_size_, policy.max_batch_size);
  }

  return FixedBatchExecutor::Execute<QueryCandidatePair, float>(
      inputs, policy,
      [this, &inputs](const BatchSlice& slice,
                      std::vector<float>* batch_scores) {
        return this->RawScoreSlice(inputs, slice, batch_scores);
      },
      outputs);
}

int BgeRerankerModel::RawScoreSlice(const QueryCandidatesBatch& all_inputs,
                                    const BatchSlice& slice,
                                    std::vector<float>* batch_scores) noexcept {
  if (!batch_scores) return -1;
  batch_scores->clear();

  size_t exec_count = slice.execution_count;
  if (exec_count == 0) return 0;

  try {
    std::string diag;
    BertInputTensors input_tensors;
    if (!input_tensors.Create(exec_count, max_length_, &diag)) {
      ALG_LOG_ERROR("[BgeRerankerModel] Failed to create input tensors: %s\n",
                    diag.c_str());
      return -1;
    }
    int64_t* ids_ptr = input_tensors.ids;
    int64_t* mask_ptr = input_tensors.mask;
    int64_t* type_ptr = input_tensors.types;

    std::vector<int64_t> sample_ids;
    std::vector<int64_t> sample_mask;
    std::vector<int64_t> sample_type;

    for (size_t i = 0; i < exec_count; ++i) {
      if (i < slice.valid_count) {
        const auto& pair = all_inputs[slice.offset + i].data;
        if (!tokenizer_.EncodePair(pair.query, pair.candidate, max_length_,
                                   &sample_ids, &sample_mask, &sample_type,
                                   &diag)) {
          ALG_LOG_ERROR("[BgeRerankerModel] Tokenizer EncodePair error: %s\n",
                        diag.c_str());
          batch_scores->clear();
          return -1;
        }
      } else {
        // Dummy padding item
        if (!tokenizer_.EncodePair("", "", max_length_, &sample_ids,
                                   &sample_mask, &sample_type, &diag)) {
          ALG_LOG_ERROR("[BgeRerankerModel] Dummy EncodePair error: %s\n",
                        diag.c_str());
          batch_scores->clear();
          return -1;
        }
      }

      std::memcpy(ids_ptr + i * max_length_, sample_ids.data(),
                  max_length_ * sizeof(int64_t));
      std::memcpy(mask_ptr + i * max_length_, sample_mask.data(),
                  max_length_ * sizeof(int64_t));
      std::memcpy(type_ptr + i * max_length_, sample_type.data(),
                  max_length_ * sizeof(int64_t));
    }

    TensorMap input_map = input_tensors.ReleaseToMap(
        HasTensorInput(session_->Inputs(), "token_type_ids"));

    TensorMap output_map;
    int ret = session_->Run(input_map, &output_map, &diag);
    if (ret != 0) {
      ALG_LOG_ERROR("[BgeRerankerModel] session_->Run failed: %s\n",
                    diag.c_str());
      batch_scores->clear();
      return ret;
    }

    auto it_out = output_map.find(output_name_);
    if (it_out == output_map.end()) {
      ALG_LOG_ERROR(
          "[BgeRerankerModel] Expected output tensor '%s' not found in session "
          "outputs\n",
          output_name_.c_str());
      batch_scores->clear();
      return -1;
    }

    const float* data = nullptr;
    if (!ValidateRerankOutput(it_out->second, exec_count, &data, &diag)) {
      ALG_LOG_ERROR("[BgeRerankerModel] ValidateRerankOutput failed: %s\n",
                    diag.c_str());
      batch_scores->clear();
      return -1;
    }

    batch_scores->resize(exec_count);
    for (size_t b = 0; b < exec_count; ++b) {
      float logit = data[b];
      if (!std::isfinite(logit)) {
        ALG_LOG_ERROR(
            "[BgeRerankerModel] Output score is NaN or Inf at index %zu\n", b);
        batch_scores->clear();
        return -1;
      }
      if (score_activation_ == "sigmoid") {
        if (logit >= 0.0f) {
          (*batch_scores)[b] = 1.0f / (1.0f + std::exp(-logit));
        } else {
          const float e = std::exp(logit);
          (*batch_scores)[b] = e / (1.0f + e);
        }
      } else {
        (*batch_scores)[b] = logit;
      }
    }

    return 0;
  } catch (const std::exception& e) {
    ALG_LOG_ERROR("[BgeRerankerModel] RawScoreSlice exception: %s\n", e.what());
    batch_scores->clear();
    return -1;
  } catch (...) {
    ALG_LOG_ERROR("[BgeRerankerModel] RawScoreSlice unknown exception\n");
    batch_scores->clear();
    return -1;
  }
}

static const ModelDefinition kBgeRerankerModelDefinition = [] {
  ModelDefinition def;
  def.model_type = BgeRerankerModel::kModelType;
  def.capability = BgeRerankerModel::kCapability;
  def.description =
      "BGE cross-encoder reranker model using TensorGraph protocol";
  def.required_protocol = ExecutionProtocol::kTensorGraph;
  def.concurrency = InferenceConcurrency::kConcurrent;
  def.config_fields = {
      {"tokenizer_file", ConfigValueKind::kString, false, "vocab.txt"},
      {"do_lower_case", ConfigValueKind::kBoolean, false, true},
      {"max_length", ConfigValueKind::kInteger, false, 512, 3.0, 4096.0},
      {"output_name", ConfigValueKind::kString, false, "logits"},
      {"score_activation",
       ConfigValueKind::kString,
       false,
       "sigmoid",
       std::nullopt,
       std::nullopt,
       {"sigmoid", "identity"}},
      {"max_batch_size", ConfigValueKind::kInteger, false, 4, 1.0, 1024.0},
  };
  return def;
}();

REGISTER_MODEL_WITH_DEFINITION(BgeRerankerModel, kBgeRerankerModelDefinition);

}  // namespace alg_framework
