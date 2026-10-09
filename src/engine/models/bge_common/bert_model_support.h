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

bool ValidateBertInputMetadata(const ITensorGraphSession& session,
                               size_t max_tokens, const std::string& label,
                               std::string* diagnostic);

const TensorSpec* RequireFloatOutputMetadata(const ITensorGraphSession& session,
                                             const std::string& output_name,
                                             const std::string& label,
                                             size_t min_rank, size_t max_rank,
                                             std::string* diagnostic);

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
