#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "engine/backend_interface.h"

namespace alg_framework {

bool ResolveTokenizerResourcePath(const std::string& model_resource_root,
                                  const std::string& tokenizer_file,
                                  std::filesystem::path* resolved_path,
                                  std::string* diagnostic);

bool ValidateModelBatchLimit(const BatchPolicy& session_policy,
                             size_t model_max_batch_size,
                             std::string* diagnostic);

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
              std::string* diagnostic);
  TensorMap ReleaseToMap(bool include_token_type_ids);
};

}  // namespace alg_framework
