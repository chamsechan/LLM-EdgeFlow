#pragma once

#include <memory>
#include <string>
#include <utility>

#include "contracts/inference_payloads.h"
#include "contracts/traceable_item.h"
#include "engine/model_interface.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_result.h"
#include "nodes/traceable_batch_validation.h"

namespace llm_edgeflow {

class LlmCall {
 public:
  LlmCall() = default;
  explicit LlmCall(std::shared_ptr<ILlmModel> model,
                   std::string slot_name = "generator")
      : model_(std::move(model)), slot_name_(std::move(slot_name)) {}

  LlmCall(const LlmCall&) = delete;
  LlmCall& operator=(const LlmCall&) = delete;
  LlmCall(LlmCall&&) noexcept = default;
  LlmCall& operator=(LlmCall&&) noexcept = default;

  NodeResult<TextBatch> Generate(
      const TextBatch& prompts,
      const GenerateOptions& options = GenerateOptions{}) const {
    if (prompts.empty()) {
      return NodeResult<TextBatch>::Success(TextBatch{});
    }
    if (!model_) {
      return NodeResult<TextBatch>::Failure(
          NodeErrorKind::kModelCallError, "LLM model is null",
          node_error::author_node::kModelCallFailed, "generate", slot_name_);
    }
    TextBatch outputs;
    int ret = model_->Generate(prompts, options, &outputs);
    if (ret != 0) {
      return NodeResult<TextBatch>::Failure(
          NodeErrorKind::kModelCallError,
          "LLM generate failed with code " + std::to_string(ret), ret,
          "generate", slot_name_);
    }
    auto alignment = ValidatePreservedTraceableAlignment(prompts, outputs);
    if (alignment.error == TraceableAlignmentError::kCountMismatch) {
      return NodeResult<TextBatch>::Failure(
          NodeErrorKind::kOutputCountMismatch, "LLM output count mismatch",
          node_error::author_node::kOutputCountMismatch, "align", slot_name_);
    }
    if (alignment.error == TraceableAlignmentError::kProvenanceMismatch) {
      return NodeResult<TextBatch>::Failure(
          NodeErrorKind::kOutputProvenanceMismatch,
          "LLM output provenance mismatch",
          node_error::author_node::kOutputProvenanceMismatch, "align",
          slot_name_);
    }
    return NodeResult<TextBatch>::Success(std::move(outputs));
  }

  const std::string& SlotName() const noexcept { return slot_name_; }
  bool IsBound() const noexcept { return model_ != nullptr; }

 private:
  std::shared_ptr<ILlmModel> model_;
  std::string slot_name_;
};

class EmbeddingCall {
 public:
  EmbeddingCall() = default;
  explicit EmbeddingCall(std::shared_ptr<IEmbeddingModel> model,
                         std::string slot_name = "encoder")
      : model_(std::move(model)), slot_name_(std::move(slot_name)) {}

  EmbeddingCall(const EmbeddingCall&) = delete;
  EmbeddingCall& operator=(const EmbeddingCall&) = delete;
  EmbeddingCall(EmbeddingCall&&) noexcept = default;
  EmbeddingCall& operator=(EmbeddingCall&&) noexcept = default;

  NodeResult<EmbeddingBatch> Embed(
      const TextBatch& inputs,
      const EmbeddingOptions& options = EmbeddingOptions{}) const {
    if (inputs.empty()) {
      return NodeResult<EmbeddingBatch>::Success(EmbeddingBatch{});
    }
    if (!model_) {
      return NodeResult<EmbeddingBatch>::Failure(
          NodeErrorKind::kModelCallError, "Embedding model is null",
          node_error::author_node::kModelCallFailed, "embed", slot_name_);
    }
    EmbeddingBatch outputs;
    int ret = model_->Embed(inputs, options, &outputs);
    if (ret != 0) {
      return NodeResult<EmbeddingBatch>::Failure(
          NodeErrorKind::kModelCallError,
          "Embedding embed failed with code " + std::to_string(ret), ret,
          "embed", slot_name_);
    }
    auto alignment = ValidatePreservedTraceableAlignment(inputs, outputs);
    if (alignment.error == TraceableAlignmentError::kCountMismatch) {
      return NodeResult<EmbeddingBatch>::Failure(
          NodeErrorKind::kOutputCountMismatch,
          "Embedding output count mismatch",
          node_error::author_node::kOutputCountMismatch, "align", slot_name_);
    }
    if (alignment.error == TraceableAlignmentError::kProvenanceMismatch) {
      return NodeResult<EmbeddingBatch>::Failure(
          NodeErrorKind::kOutputProvenanceMismatch,
          "Embedding output provenance mismatch",
          node_error::author_node::kOutputProvenanceMismatch, "align",
          slot_name_);
    }
    return NodeResult<EmbeddingBatch>::Success(std::move(outputs));
  }

  const std::string& SlotName() const noexcept { return slot_name_; }
  bool IsBound() const noexcept { return model_ != nullptr; }

 private:
  std::shared_ptr<IEmbeddingModel> model_;
  std::string slot_name_;
};

struct NoModels {};

}  // namespace llm_edgeflow
