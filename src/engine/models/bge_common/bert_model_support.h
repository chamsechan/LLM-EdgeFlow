#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "engine/backend_interface.h"

namespace llm_edgeflow {

class BertWordPieceTokenizer;

// BERT 类模型参数的跨字段规则（供各模型 Parameters::Validate 使用）：
// 词表文件名和输出张量名不能为空。
bool ValidateBertTextParameters(const std::string& tokenizer_file,
                                const std::string& output_name,
                                std::string* diagnostic);

// 从会话的张量元数据读取固定形状：维度为固定正数时返回该值，否则返回空。
// 输出张量 output_name 的最后一维（向量维数）。
std::optional<int64_t> StaticOutputDim(const ITensorGraphSession& session,
                                       const std::string& output_name);

// 输入张量的序列维（[batch, sequence] 中的 sequence）。
std::optional<int64_t> StaticSequenceLength(const ITensorGraphSession& session);

std::shared_ptr<ITensorGraphSession> RequireTensorGraphSession(
    const std::shared_ptr<IBackendSession>& backend_session,
    std::string* diagnostic);

bool LoadBertTokenizer(const std::string& model_resource_root,
                       const std::string& tokenizer_file, bool do_lower_case,
                       BertWordPieceTokenizer* tokenizer,
                       std::string* diagnostic);

bool ValidateModelBatchLimit(const BatchPolicy& session_policy,
                             size_t model_max_batch_size,
                             std::string* diagnostic);

bool ValidateTensorBatchDimension(int64_t dimension,
                                  const BatchPolicy& session_policy,
                                  const std::string& model_name,
                                  const std::string& tensor_kind,
                                  const std::string& tensor_name,
                                  std::string* diagnostic);

bool ValidateBertInputMetadata(const ITensorGraphSession& session,
                               size_t max_length, const std::string& model_name,
                               std::string* diagnostic);

const TensorSpec* RequireFloatOutputMetadata(const ITensorGraphSession& session,
                                             const std::string& output_name,
                                             const std::string& model_name,
                                             size_t min_rank, size_t max_rank,
                                             std::string* diagnostic);

bool ValidateRuntimeBatchTensor(const Tensor& tensor, size_t expected_batch,
                                size_t min_rank, size_t max_rank,
                                std::string* diagnostic) noexcept;

BatchPolicy ConstrainModelBatchPolicy(const ITensorGraphSession* session,
                                      size_t model_max_batch_size) noexcept;

bool HasTensorInput(const std::vector<TensorSpec>& inputs,
                    const std::string& name) noexcept;

struct BertInputTensors {
  Tensor input_ids;
  Tensor attention_mask;
  Tensor token_type_ids;
  int64_t* ids = nullptr;
  int64_t* mask = nullptr;
  int64_t* types = nullptr;

  bool Create(size_t batch_size, size_t sequence_length,
              bool include_token_type_ids, std::string* diagnostic);
  TensorMap ReleaseToMap();

 private:
  bool include_token_type_ids_ = false;
};

}  // namespace llm_edgeflow
