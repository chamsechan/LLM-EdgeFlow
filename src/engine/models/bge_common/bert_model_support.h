#pragma once

#include <optional>
#include <string>
#include <vector>

#include "engine/backend_interface.h"

namespace llm_edgeflow {

class BertWordPieceTokenizer;

std::optional<int64_t> StaticOutputDim(const ITensorGraphSession& session,
                                       const std::string& output_name);

std::optional<int64_t> StaticSequenceLength(const ITensorGraphSession& session);

std::shared_ptr<ITensorGraphSession> RequireTensorGraphSession(
    const std::shared_ptr<IBackendSession>& backend_session,
    std::string* diagnostic);

bool LoadBertTokenizer(const std::string& tokenizer_file, bool do_lower_case,
                       BertWordPieceTokenizer* tokenizer,
                       std::string* diagnostic);

bool ValidateTensorBatchDimension(int64_t dimension,
                                  const BatchPolicy& session_policy,
                                  const std::string& label,
                                  const std::string& tensor_kind,
                                  const std::string& tensor_name,
                                  std::string* diagnostic);

/**
 * @brief 检查 BERT 输入名称、int64 类型及二维形状，并核对编码长度和 Session
 * 批策略。 input_ids 和 attention_mask 必须存在，token_type_ids
 * 可选；不读取张量数据。
 */
bool ValidateBertInputMetadata(const ITensorGraphSession& session,
                               size_t max_tokens, const std::string& label,
                               std::string* diagnostic);

/**
 * @brief 按名称查找 float32 输出，并校验维度数量及声明的批大小符合 Session
 * 批策略。成功返回借用 Session 元数据的指针，失败返回空指针并给出诊断。
 */
const TensorSpec* RequireFloatOutputMetadata(const ITensorGraphSession& session,
                                             const std::string& output_name,
                                             const std::string& label,
                                             size_t min_rank, size_t max_rank,
                                             std::string* diagnostic);

/**
 * @brief 在解释模型输出前检查实际形状：维度须为正，首维须等于本次执行批大小。
 * 仅校验形状，元素类型、内存布局及数值有效性由相应的数据访问和解释步骤检查。
 */
bool ValidateRuntimeBatchTensor(const Tensor& tensor, size_t expected_batch,
                                size_t min_rank, size_t max_rank,
                                std::string* diagnostic) noexcept;

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
