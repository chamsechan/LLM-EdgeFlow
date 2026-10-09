#include "engine/models/bge_reranker/bge_reranker_model.h"

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
#include "engine/models/common/from_model.h"

namespace llm_edgeflow {

namespace {

struct Params {
  std::string tokenizer_file;
  bool do_lower_case = true;
  std::optional<int64_t> max_length;
  std::string output_name;
  std::string score_activation;
  int max_batch_size = 4;
};

Parameters<Params> ParamSpec() {
  auto spec = Parameters<Params>(
      {Field("tokenizer_file", &Params::tokenizer_file)
           .Default("vocab.txt")
           .Description("匹配该重排权重的 BERT WordPiece "
                        "词表；相对路径基于模型资源目录，例如 "
                        "vocab.txt，也可使用绝对路径。"),
       Field("do_lower_case", &Params::do_lower_case)
           .Default(true)
           .Description(
               "查询和候选分词前，将英文大写和全角英文字母归一为半角小写。"),
       Field("max_length", &Params::max_length)
           .Range(3, 4096)
           .Description(
               "查询与候选成对编码后的总 token 长度，包含 "
               "[CLS]/[SEP]；截断和补齐后须匹配"
               "模型张量形状；不写时从模型输入张量读取，读不到时为 512。"),
       Field("output_name", &Params::output_name)
           .Default("logits")
           .Description("读取查询与候选评分的输出张量名，例如 "
                        "logits，必须与模型导出名称一致。"),
       Field("score_activation", &Params::score_activation)
           .Default("sigmoid")
           .Enum({"sigmoid", "identity"})
           .Description("sigmoid 将模型原始分数映射至 [0,1]；identity "
                        "直接使用原始分数。"),
       Field("max_batch_size", &Params::max_batch_size)
           .Default(4)
           .Range(1, 1024)
           .Description("一次模型执行处理的查询与候选对数量上限；必须满足 "
                        "Backend 的批次约束。")});
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
  const auto& params = ctx.Params<Params>();
  auto tensor_session =
      RequireTensorGraphSession(ctx.backend_session, diagnostic);
  if (!tensor_session) return nullptr;

  int64_t max_length = 0;
  if (!ResolveFromModel("max_length", params.max_length,
                        StaticSequenceLength(*tensor_session), 512, &max_length,
                        diagnostic)) {
    return nullptr;
  }
  // 固定形状由资源提供，必须留出句对编码所需的三个特殊 token。
  if (max_length < 3 || max_length > 4096) {
    SetDiagnosticNoexcept(diagnostic, "Model max_length must be in [3, 4096]");
    return nullptr;
  }
  const auto& output_name = params.output_name;
  const size_t max_batch_size = params.max_batch_size;

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
  if (!LoadBertTokenizer(ctx.model_resource_root, params.tokenizer_file,
                         params.do_lower_case, &tokenizer, diagnostic)) {
    return nullptr;
  }

  return std::make_shared<BgeRerankerModel>(
      std::move(tensor_session), std::move(tokenizer), max_length, output_name,
      params.score_activation, max_batch_size);
}

int BgeRerankerModel::Score(const QueryCandidatesBatch& inputs,
                            ScoreBatch* outputs,
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

  BatchPolicy policy =
      ConstrainModelBatchPolicy(session_.get(), max_batch_size_);

  return FixedBatchExecutor::Execute<QueryCandidatePair, float>(
      inputs, policy,
      [this, &inputs, diagnostic](const BatchSlice& slice,
                                  std::vector<float>* batch_scores) {
        return this->RawScoreSlice(inputs, slice, batch_scores, diagnostic);
      },
      outputs, diagnostic);
}

int BgeRerankerModel::RawScoreSlice(const QueryCandidatesBatch& all_inputs,
                                    const BatchSlice& slice,
                                    std::vector<float>* batch_scores,
                                    std::string* diagnostic) noexcept {
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
      SetDiagnosticNoexcept(diagnostic, diag);
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
          SetDiagnosticNoexcept(diagnostic, diag);
          batch_scores->clear();
          return -1;
        }
      } else {
        // 填充用的空条目
        if (!tokenizer_.EncodePair("", "", max_length_, &sample_ids,
                                   &sample_mask, &sample_type, &diag)) {
          ALG_LOG_ERROR("[BgeRerankerModel] Dummy EncodePair error: %s\n",
                        diag.c_str());
          SetDiagnosticNoexcept(diagnostic, diag);
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
      SetDiagnosticNoexcept(diagnostic, diag);
      batch_scores->clear();
      return ret;
    }

    auto it_out = output_map.find(output_name_);
    if (it_out == output_map.end()) {
      ALG_LOG_ERROR(
          "[BgeRerankerModel] Expected output tensor '%s' not found in session "
          "outputs\n",
          output_name_.c_str());
      SetDiagnosticNoexcept(diagnostic, "Expected output tensor is missing");
      batch_scores->clear();
      return -1;
    }

    const float* data = nullptr;
    if (!ValidateRerankOutput(it_out->second, exec_count, &data, &diag)) {
      ALG_LOG_ERROR("[BgeRerankerModel] ValidateRerankOutput failed: %s\n",
                    diag.c_str());
      SetDiagnosticNoexcept(diagnostic, diag);
      batch_scores->clear();
      return -1;
    }

    batch_scores->resize(exec_count);
    for (size_t b = 0; b < exec_count; ++b) {
      float logit = data[b];
      if (!std::isfinite(logit)) {
        ALG_LOG_ERROR(
            "[BgeRerankerModel] Output score is NaN or Inf at index %zu\n", b);
        SetDiagnosticNoexcept(diagnostic,
                              "Reranker output score is NaN or Inf");
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
    SetDiagnosticNoexcept(diagnostic, e.what());
    batch_scores->clear();
    return -1;
  } catch (...) {
    ALG_LOG_ERROR("[BgeRerankerModel] RawScoreSlice unknown exception\n");
    SetDiagnosticNoexcept(diagnostic, "Unknown model execution exception");
    batch_scores->clear();
    return -1;
  }
}

static const ModelDefinition& BgeRerankerModelDefinition() {
  static const ModelDefinition definition = [] {
    auto def = MakeModelDefinition<BgeRerankerModel>();
    def.description =
        "BGE cross-encoder reranker model using TensorGraph protocol";
    def.required_protocol = ExecutionProtocol::kTensorGraph;
    def.params = ParamSpec();
    return def;
  }();
  return definition;
}

REGISTER_MODEL_WITH_DEFINITION(BgeRerankerModel, BgeRerankerModelDefinition());

}  // namespace llm_edgeflow
