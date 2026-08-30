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
  if (session_policy.fixed_batch_size == 0 ||
      model_max_batch_size >= session_policy.fixed_batch_size) {
    return true;
  }
  if (diagnostic) {
    *diagnostic = "Model max_batch_size (" +
                  std::to_string(model_max_batch_size) +
                  ") cannot be smaller than Session fixed_batch_size (" +
                  std::to_string(session_policy.fixed_batch_size) + ")";
  }
  return false;
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
                              std::string* diagnostic) {
  TensorDesc descriptor;
  descriptor.element_type = ElementType::kInt64;
  descriptor.shape = {static_cast<int64_t>(batch_size),
                      static_cast<int64_t>(sequence_length)};
  if (!CreateHostTensor(descriptor, &input_ids, diagnostic) ||
      !CreateHostTensor(descriptor, &attention_mask, diagnostic) ||
      !CreateHostTensor(descriptor, &token_type_ids, diagnostic)) {
    return false;
  }

  ids = static_cast<int64_t*>(input_ids.buffer->MutableData());
  mask = static_cast<int64_t*>(attention_mask.buffer->MutableData());
  types = static_cast<int64_t*>(token_type_ids.buffer->MutableData());
  return ids && mask && types;
}

TensorMap BertInputTensors::ReleaseToMap(bool include_token_type_ids) {
  TensorMap inputs;
  inputs["input_ids"] = std::move(input_ids);
  inputs["attention_mask"] = std::move(attention_mask);
  if (include_token_type_ids) {
    inputs["token_type_ids"] = std::move(token_type_ids);
  }
  return inputs;
}

}  // namespace alg_framework
