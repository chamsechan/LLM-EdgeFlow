#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "engine/backend_interface.h"

namespace alg_framework {

class BertWordPieceTokenizer;

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

}  // namespace alg_framework
