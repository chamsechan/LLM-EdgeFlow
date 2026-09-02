#include "engine/models/bge_reranker/bge_reranker_model.h"

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "company_alg_log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/models/bge_common/bert_model_support.h"

namespace llm_edgeflow {

namespace {

bool ValidateRerankOutput(const Tensor& tensor, size_t expected_batch,
                          const float** data_ptr,
                          std::string* diagnostic) noexcept {
  if (!ValidateRuntimeBatchTensor(tensor, expected_batch, 1, 2, diagnostic)) {
    return false;
  }
  const auto& shape = tensor.desc.shape;
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
  auto tensor_session =
      RequireTensorGraphSession(ctx.backend_session, diagnostic);
  if (!tensor_session) return nullptr;

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

  auto session_policy = tensor_session->GetBatchPolicy();
  if (!ValidateModelBatchLimit(session_policy, max_batch_size, diagnostic)) {
    return nullptr;
  }

  if (!ValidateBertInputMetadata(*tensor_session, max_length,
                                 "BgeRerankerModel", diagnostic)) {
    return nullptr;
  }

  const TensorSpec* target_output = RequireFloatOutputMetadata(
      *tensor_session, output_name, "BgeRerankerModel", 1, 2, diagnostic);
  if (!target_output) return nullptr;

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

  BertWordPieceTokenizer tokenizer;
  if (!LoadBertTokenizer(ctx.model_resource_root, tokenizer_file, do_lower_case,
                         &tokenizer, diagnostic)) {
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
  return InferenceConcurrency::kConcurrent;
}

size_t BgeRerankerModel::GetMaxBatchSize() const noexcept {
  return ConstrainModelBatchPolicy(session_.get(), max_batch_size_)
      .max_batch_size;
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

  BatchPolicy policy =
      ConstrainModelBatchPolicy(session_.get(), max_batch_size_);

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
    const bool include_token_type_ids =
        HasTensorInput(session_->Inputs(), "token_type_ids");
    if (!input_tensors.Create(exec_count, max_length_, include_token_type_ids,
                              &diag)) {
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
      if (type_ptr) {
        std::memcpy(type_ptr + i * max_length_, sample_type.data(),
                    max_length_ * sizeof(int64_t));
      }
    }

    TensorMap input_map = input_tensors.ReleaseToMap();

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

}  // namespace llm_edgeflow
