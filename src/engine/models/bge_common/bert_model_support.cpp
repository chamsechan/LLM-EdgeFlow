#include "engine/models/bge_common/bert_model_support.h"

#include <algorithm>
#include <utility>

#include "engine/models/bge_embedding/bert_wordpiece_tokenizer.h"

namespace alg_framework {

std::shared_ptr<ITensorGraphSession> RequireTensorGraphSession(
    const std::shared_ptr<IBackendSession>& backend_session,
    std::string* diagnostic) {
  auto session =
      std::dynamic_pointer_cast<ITensorGraphSession>(backend_session);
  if (session) return session;
  if (diagnostic) {
    *diagnostic = backend_session ? "Backend session does not implement "
                                    "ITensorGraphSession protocol"
                                  : "Backend session is null";
  }
  return nullptr;
}

namespace {

bool Reject(std::string* diagnostic, std::string message) {
  if (diagnostic) *diagnostic = std::move(message);
  return false;
}

void SetDiagnostic(std::string* diagnostic, std::string message) {
  if (diagnostic) *diagnostic = std::move(message);
}

std::string RankRequirement(size_t min_rank, size_t max_rank) {
  if (min_rank == max_rank) return std::to_string(min_rank);
  return std::to_string(min_rank) + " or " + std::to_string(max_rank);
}

bool ResolveTokenizerResourcePath(const std::string& model_resource_root,
                                  const std::string& tokenizer_file,
                                  std::filesystem::path* resolved_path,
                                  std::string* diagnostic) {
  if (!resolved_path) {
    if (diagnostic) *diagnostic = "Resolved tokenizer path is null";
    return false;
  }

  const std::filesystem::path tokenizer_path(tokenizer_file);
  if (tokenizer_path.is_absolute()) {
    *resolved_path = tokenizer_path.lexically_normal();
    return true;
  }

  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::path(model_resource_root), error);
  if (error) {
    if (diagnostic) {
      *diagnostic =
          "Failed to canonicalize model resource root: " + model_resource_root;
    }
    return false;
  }

  const auto candidate =
      std::filesystem::weakly_canonical(root / tokenizer_path, error);
  if (error) {
    if (diagnostic) {
      *diagnostic = "Failed to canonicalize tokenizer file: " + tokenizer_file;
    }
    return false;
  }

  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  while (root_it != root.end() && candidate_it != candidate.end() &&
         *root_it == *candidate_it) {
    ++root_it;
    ++candidate_it;
  }
  if (root_it != root.end()) {
    if (diagnostic) {
      *diagnostic = "Tokenizer file path cannot escape root: " + tokenizer_file;
    }
    return false;
  }

  *resolved_path = candidate;
  return true;
}

}  // namespace

bool LoadBertTokenizer(const std::string& model_resource_root,
                       const std::string& tokenizer_file, bool do_lower_case,
                       BertWordPieceTokenizer* tokenizer,
                       std::string* diagnostic) {
  std::filesystem::path resolved_path;
  return ResolveTokenizerResourcePath(model_resource_root, tokenizer_file,
                                      &resolved_path, diagnostic) &&
         tokenizer->Load(resolved_path.string(), do_lower_case, diagnostic);
}

bool ValidateModelBatchLimit(const BatchPolicy& session_policy,
                             size_t model_max_batch_size,
                             std::string* diagnostic) {
  if (model_max_batch_size == 0) {
    return Reject(diagnostic, "Model max_batch_size must be at least 1");
  }
  if (session_policy.max_batch_size == 0 ||
      (session_policy.fixed_batch_size != 0 &&
       session_policy.fixed_batch_size != session_policy.max_batch_size)) {
    return Reject(
        diagnostic,
        "Invalid Session BatchPolicy (max=" +
            std::to_string(session_policy.max_batch_size) +
            ", fixed=" + std::to_string(session_policy.fixed_batch_size) + ")");
  }
  if (session_policy.fixed_batch_size == 0 ||
      model_max_batch_size >= session_policy.fixed_batch_size) {
    return true;
  }
  return Reject(diagnostic,
                "Model max_batch_size (" +
                    std::to_string(model_max_batch_size) +
                    ") cannot be smaller than Session fixed_batch_size (" +
                    std::to_string(session_policy.fixed_batch_size) + ")");
}

bool ValidateTensorBatchDimension(int64_t dimension,
                                  const BatchPolicy& session_policy,
                                  const std::string& model_name,
                                  const std::string& tensor_kind,
                                  const std::string& tensor_name,
                                  std::string* diagnostic) {
  if (dimension == 0) {
    return Reject(diagnostic, model_name + " " + tensor_kind + " '" +
                                  tensor_name +
                                  "' batch dimension cannot be 0");
  }
  if (dimension < 0) return true;

  const size_t static_batch = static_cast<size_t>(dimension);
  if (session_policy.fixed_batch_size > 0 &&
      static_batch != session_policy.fixed_batch_size) {
    return Reject(diagnostic,
                  model_name + " " + tensor_kind + " '" + tensor_name +
                      "' static batch " + std::to_string(static_batch) +
                      " does not match fixed_batch_size " +
                      std::to_string(session_policy.fixed_batch_size));
  }
  if (static_batch > session_policy.max_batch_size) {
    return Reject(diagnostic,
                  model_name + " " + tensor_kind + " '" + tensor_name +
                      "' static batch " + std::to_string(static_batch) +
                      " exceeds session max_batch_size " +
                      std::to_string(session_policy.max_batch_size));
  }
  return true;
}

bool ValidateBertInputMetadata(const ITensorGraphSession& session,
                               size_t max_length, const std::string& model_name,
                               std::string* diagnostic) {
  const auto& inputs = session.Inputs();
  if (inputs.empty()) {
    return Reject(diagnostic,
                  model_name + " session input metadata cannot be empty");
  }

  bool has_input_ids = false;
  bool has_attention_mask = false;
  bool has_token_type_ids = false;
  const BatchPolicy policy = session.GetBatchPolicy();
  for (const auto& spec : inputs) {
    bool* seen = nullptr;
    if (spec.name == "input_ids") {
      seen = &has_input_ids;
    } else if (spec.name == "attention_mask") {
      seen = &has_attention_mask;
    } else if (spec.name == "token_type_ids") {
      seen = &has_token_type_ids;
    } else {
      return Reject(diagnostic,
                    model_name +
                        " session declares unrecognized required input: '" +
                        spec.name + "'");
    }
    if (*seen) {
      return Reject(diagnostic, model_name +
                                    " session declares duplicate input: '" +
                                    spec.name + "'");
    }
    *seen = true;

    if (spec.element_type != ElementType::kInt64) {
      return Reject(diagnostic, model_name + " input '" + spec.name +
                                    "' dtype must be int64, got: " +
                                    ElementTypeToString(spec.element_type));
    }
    if (spec.shape.size() != 2) {
      return Reject(diagnostic,
                    model_name + " input '" + spec.name +
                        "' rank must be 2 [batch, sequence], got rank: " +
                        std::to_string(spec.shape.size()));
    }
    if (spec.shape[1] == 0) {
      return Reject(diagnostic, model_name + " input '" + spec.name +
                                    "' sequence dimension cannot be 0");
    }
    if (spec.shape[1] > 0 && static_cast<size_t>(spec.shape[1]) != max_length) {
      return Reject(diagnostic, model_name + " input '" + spec.name +
                                    "' static sequence length " +
                                    std::to_string(spec.shape[1]) +
                                    " does not match configured max_length " +
                                    std::to_string(max_length));
    }
    if (!ValidateTensorBatchDimension(spec.shape[0], policy, model_name,
                                      "input", spec.name, diagnostic)) {
      return false;
    }
  }

  if (!has_input_ids || !has_attention_mask) {
    return Reject(diagnostic,
                  model_name +
                      " session missing required inputs (input_ids or "
                      "attention_mask)");
  }
  return true;
}

const TensorSpec* RequireFloatOutputMetadata(const ITensorGraphSession& session,
                                             const std::string& output_name,
                                             const std::string& model_name,
                                             size_t min_rank, size_t max_rank,
                                             std::string* diagnostic) {
  const auto& outputs = session.Outputs();
  if (outputs.empty()) {
    SetDiagnostic(diagnostic,
                  model_name + " session output metadata cannot be empty");
    return nullptr;
  }

  const auto output = std::find_if(outputs.begin(), outputs.end(),
                                   [&output_name](const TensorSpec& spec) {
                                     return spec.name == output_name;
                                   });
  if (output == outputs.end()) {
    SetDiagnostic(diagnostic,
                  model_name +
                      " session outputs missing expected output tensor: '" +
                      output_name + "'");
    return nullptr;
  }
  if (output->element_type != ElementType::kFloat32) {
    SetDiagnostic(diagnostic, model_name + " output '" + output_name +
                                  "' dtype must be float32, got: " +
                                  ElementTypeToString(output->element_type));
    return nullptr;
  }
  if (min_rank > max_rank || output->shape.size() < min_rank ||
      output->shape.size() > max_rank) {
    SetDiagnostic(diagnostic,
                  model_name + " output '" + output_name + "' rank must be " +
                      RankRequirement(min_rank, max_rank) +
                      ", got: " + std::to_string(output->shape.size()));
    return nullptr;
  }
  if (!ValidateTensorBatchDimension(output->shape[0], session.GetBatchPolicy(),
                                    model_name, "output", output_name,
                                    diagnostic)) {
    return nullptr;
  }
  return &*output;
}

bool ValidateRuntimeBatchTensor(const Tensor& tensor, size_t expected_batch,
                                size_t min_rank, size_t max_rank,
                                std::string* diagnostic) noexcept {
  try {
    if (expected_batch == 0) {
      return Reject(diagnostic, "Expected batch must be positive");
    }
    const auto& shape = tensor.desc.shape;
    if (min_rank > max_rank || shape.size() < min_rank ||
        shape.size() > max_rank) {
      return Reject(diagnostic, "Output tensor rank must be " +
                                    RankRequirement(min_rank, max_rank) +
                                    ", got: " + std::to_string(shape.size()));
    }
    for (size_t dimension = 0; dimension < shape.size(); ++dimension) {
      if (shape[dimension] <= 0) {
        return Reject(diagnostic, "Output tensor dimension " +
                                      std::to_string(dimension) +
                                      " must be strictly positive, got: " +
                                      std::to_string(shape[dimension]));
      }
    }
    if (static_cast<size_t>(shape[0]) != expected_batch) {
      return Reject(diagnostic,
                    "Output tensor batch dimension mismatch. Expected: " +
                        std::to_string(expected_batch) +
                        ", got: " + std::to_string(shape[0]));
    }
    return true;
  } catch (...) {
    try {
      SetDiagnostic(diagnostic, "Exception validating output tensor metadata");
    } catch (...) {
    }
    return false;
  }
}

BatchPolicy ConstrainModelBatchPolicy(const ITensorGraphSession* session,
                                      size_t model_max_batch_size) noexcept {
  BatchPolicy policy{model_max_batch_size, 0};
  if (session) policy = session->GetBatchPolicy();
  policy.max_batch_size = std::min(model_max_batch_size, policy.max_batch_size);
  return policy;
}

bool HasTensorInput(const std::vector<TensorSpec>& inputs,
                    const std::string& name) noexcept {
  for (const auto& input : inputs) {
    if (input.name == name) return true;
  }
  return false;
}

bool BertInputTensors::Create(size_t batch_size, size_t sequence_length,
                              bool include_token_type_ids,
                              std::string* diagnostic) {
  input_ids = {};
  attention_mask = {};
  token_type_ids = {};
  ids = nullptr;
  mask = nullptr;
  types = nullptr;
  include_token_type_ids_ = include_token_type_ids;
  TensorDesc descriptor;
  descriptor.element_type = ElementType::kInt64;
  descriptor.shape = {static_cast<int64_t>(batch_size),
                      static_cast<int64_t>(sequence_length)};
  if (!CreateHostTensor(descriptor, &input_ids, diagnostic) ||
      !CreateHostTensor(descriptor, &attention_mask, diagnostic) ||
      (include_token_type_ids_ &&
       !CreateHostTensor(descriptor, &token_type_ids, diagnostic))) {
    return false;
  }

  ids = static_cast<int64_t*>(input_ids.buffer->MutableData());
  mask = static_cast<int64_t*>(attention_mask.buffer->MutableData());
  types = include_token_type_ids_
              ? static_cast<int64_t*>(token_type_ids.buffer->MutableData())
              : nullptr;
  return ids && mask && (!include_token_type_ids_ || types);
}

TensorMap BertInputTensors::ReleaseToMap() {
  TensorMap inputs;
  inputs["input_ids"] = std::move(input_ids);
  inputs["attention_mask"] = std::move(attention_mask);
  if (include_token_type_ids_) {
    inputs["token_type_ids"] = std::move(token_type_ids);
  }
  return inputs;
}

}  // namespace alg_framework
