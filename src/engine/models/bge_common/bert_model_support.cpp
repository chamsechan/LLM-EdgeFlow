#include "engine/models/bge_common/bert_model_support.h"

#include <algorithm>
#include <utility>

#include "contracts/diagnostic.h"
#include "engine/models/bge_common/bert_wordpiece_tokenizer.h"

namespace llm_edgeflow {

std::optional<int64_t> StaticOutputDim(const ITensorGraphSession& session,
                                       const std::string& output_name) {
  for (const auto& output : session.Outputs()) {
    if (output.name == output_name && !output.shape.empty() &&
        output.shape.back() > 0) {
      return output.shape.back();
    }
  }
  return std::nullopt;
}

std::optional<int64_t> StaticSequenceLength(
    const ITensorGraphSession& session) {
  for (const auto& input : session.Inputs()) {
    if (input.shape.size() == 2 && input.shape[1] > 0) {
      return input.shape[1];
    }
  }
  return std::nullopt;
}

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

std::string RankRequirement(size_t min_rank, size_t max_rank) {
  if (min_rank == max_rank) return std::to_string(min_rank);
  return std::to_string(min_rank) + " or " + std::to_string(max_rank);
}

}  // namespace

bool LoadBertTokenizer(const std::string& tokenizer_file, bool do_lower_case,
                       BertWordPieceTokenizer* tokenizer,
                       std::string* diagnostic) {
  return tokenizer->Load(tokenizer_file, do_lower_case, diagnostic);
}

bool ValidateTensorBatchDimension(int64_t dimension,
                                  const BatchPolicy& session_policy,
                                  const std::string& label,
                                  const std::string& tensor_kind,
                                  const std::string& tensor_name,
                                  std::string* diagnostic) {
  if (dimension == 0) {
    return Reject(diagnostic, label + " " + tensor_kind + " '" + tensor_name +
                                  "' batch dimension cannot be 0");
  }
  if (dimension < 0) return true;

  const size_t static_batch = static_cast<size_t>(dimension);
  if (session_policy.fixed_batch_size > 0 &&
      static_batch != session_policy.fixed_batch_size) {
    return Reject(diagnostic,
                  label + " " + tensor_kind + " '" + tensor_name +
                      "' static batch " + std::to_string(static_batch) +
                      " does not match fixed_batch_size " +
                      std::to_string(session_policy.fixed_batch_size));
  }
  if (static_batch > session_policy.max_batch_size) {
    return Reject(diagnostic,
                  label + " " + tensor_kind + " '" + tensor_name +
                      "' static batch " + std::to_string(static_batch) +
                      " exceeds session max_batch_size " +
                      std::to_string(session_policy.max_batch_size));
  }
  return true;
}

bool ValidateBertInputMetadata(const ITensorGraphSession& session,
                               size_t max_tokens, const std::string& label,
                               std::string* diagnostic) {
  const auto& inputs = session.Inputs();
  if (inputs.empty()) {
    return Reject(diagnostic,
                  label + " session input metadata cannot be empty");
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
                    label + " session declares unrecognized required input: '" +
                        spec.name + "'");
    }
    if (*seen) {
      return Reject(diagnostic, label + " session declares duplicate input: '" +
                                    spec.name + "'");
    }
    *seen = true;

    if (spec.element_type != ElementType::kInt64) {
      return Reject(diagnostic, label + " input '" + spec.name +
                                    "' dtype must be int64, got: " +
                                    ElementTypeName(spec.element_type));
    }
    if (spec.shape.size() != 2) {
      return Reject(diagnostic,
                    label + " input '" + spec.name +
                        "' rank must be 2 [batch, sequence], got rank: " +
                        std::to_string(spec.shape.size()));
    }
    if (spec.shape[1] == 0) {
      return Reject(diagnostic, label + " input '" + spec.name +
                                    "' sequence dimension cannot be 0");
    }
    if (spec.shape[1] > 0 && static_cast<size_t>(spec.shape[1]) != max_tokens) {
      return Reject(diagnostic, label + " input '" + spec.name +
                                    "' static sequence length " +
                                    std::to_string(spec.shape[1]) +
                                    " does not match configured max_tokens " +
                                    std::to_string(max_tokens));
    }
    if (!ValidateTensorBatchDimension(spec.shape[0], policy, label, "input",
                                      spec.name, diagnostic)) {
      return false;
    }
  }

  if (!has_input_ids || !has_attention_mask) {
    return Reject(diagnostic,
                  label +
                      " session missing required inputs (input_ids or "
                      "attention_mask)");
  }
  return true;
}

const TensorSpec* RequireFloatOutputMetadata(const ITensorGraphSession& session,
                                             const std::string& output_name,
                                             const std::string& label,
                                             size_t min_rank, size_t max_rank,
                                             std::string* diagnostic) {
  const auto& outputs = session.Outputs();
  if (outputs.empty()) {
    SetDiagnosticNoexcept(diagnostic,
                          label + " session output metadata cannot be empty");
    return nullptr;
  }

  const auto output = std::find_if(outputs.begin(), outputs.end(),
                                   [&output_name](const TensorSpec& spec) {
                                     return spec.name == output_name;
                                   });
  if (output == outputs.end()) {
    SetDiagnosticNoexcept(
        diagnostic, label +
                        " session outputs missing expected output tensor: '" +
                        output_name + "'");
    return nullptr;
  }
  if (output->element_type != ElementType::kFloat32) {
    SetDiagnosticNoexcept(diagnostic,
                          label + " output '" + output_name +
                              "' dtype must be float32, got: " +
                              ElementTypeName(output->element_type));
    return nullptr;
  }
  if (min_rank > max_rank || output->shape.size() < min_rank ||
      output->shape.size() > max_rank) {
    SetDiagnosticNoexcept(
        diagnostic, label + " output '" + output_name + "' rank must be " +
                        RankRequirement(min_rank, max_rank) +
                        ", got: " + std::to_string(output->shape.size()));
    return nullptr;
  }
  if (!ValidateTensorBatchDimension(output->shape[0], session.GetBatchPolicy(),
                                    label, "output", output_name, diagnostic)) {
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
    SetDiagnosticNoexcept(diagnostic,
                          "Exception validating output tensor metadata");
    return false;
  }
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

}  // namespace llm_edgeflow
