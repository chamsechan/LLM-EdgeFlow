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

namespace detail {

template <typename OutputBatchT, typename InputBatchT>
inline NodeResult<OutputBatchT> ConvertAlignedOutputs(
    const InputBatchT& inputs, OutputBatchT&& outputs,
    const std::string& model_type_name, const std::string& slot_name) {
  auto alignment = ValidatePreservedTraceableAlignment(inputs, outputs);
  if (alignment.error == TraceableAlignmentError::kCountMismatch) {
    return NodeResult<OutputBatchT>::Failure(
        NodeErrorKind::kOutputCountMismatch,
        model_type_name + " output count mismatch",
        node_error::author_node::kOutputCountMismatch, "align", slot_name);
  }
  if (alignment.error == TraceableAlignmentError::kProvenanceMismatch) {
    return NodeResult<OutputBatchT>::Failure(
        NodeErrorKind::kOutputProvenanceMismatch,
        model_type_name + " output provenance mismatch",
        node_error::author_node::kOutputProvenanceMismatch, "align", slot_name);
  }
  return NodeResult<OutputBatchT>::Success(std::forward<OutputBatchT>(outputs));
}

template <typename OutputBatchT, typename InputBatchT, typename ModelT,
          typename InvokeT>
NodeResult<OutputBatchT> InvokeAlignedModel(
    const InputBatchT& inputs, const std::shared_ptr<ModelT>& model,
    const std::string& model_name, const std::string& operation,
    const std::string& slot_name, InvokeT invoke) {
  if (inputs.empty()) return NodeResult<OutputBatchT>::Success({});
  if (!model) {
    return NodeResult<OutputBatchT>::Failure(
        NodeErrorKind::kModelCallError, model_name + " model is null",
        node_error::author_node::kModelCallFailed, operation, slot_name);
  }
  OutputBatchT outputs;
  std::string diagnostic;
  const int code = invoke(*model, &outputs, &diagnostic);
  if (code != 0) {
    return NodeResult<OutputBatchT>::Failure(
        NodeErrorKind::kModelCallError,
        model_name + " " + operation + " failed with code " +
            std::to_string(code) +
            (diagnostic.empty() ? "" : ": " + diagnostic),
        code, operation, slot_name);
  }
  return ConvertAlignedOutputs(inputs, std::move(outputs), model_name,
                               slot_name);
}

}  // namespace detail

class LlmCall {
 public:
  using ModelType = ILlmModel;
  LlmCall() = default;
  explicit LlmCall(std::shared_ptr<ModelType> model,
                   std::string slot_name = "generator",
                   std::string model_id = {})
      : model_(std::move(model)),
        slot_name_(std::move(slot_name)),
        model_id_(std::move(model_id)) {}
  LlmCall(const LlmCall&) = delete;
  LlmCall& operator=(const LlmCall&) = delete;
  LlmCall(LlmCall&&) noexcept = default;
  LlmCall& operator=(LlmCall&&) noexcept = default;

  NodeResult<TextBatch> Generate(
      const TextBatch& inputs,
      const GenerateOptions& options = GenerateOptions{}) const {
    return detail::InvokeAlignedModel<TextBatch>(
        inputs, model_, "LLM", "generate", slot_name_,
        [&](ModelType& model, TextBatch* outputs, std::string* diagnostic) {
          return model.Generate(inputs, options, outputs, diagnostic);
        });
  }

  const std::string& SlotName() const noexcept { return slot_name_; }
  const std::string& ModelId() const noexcept { return model_id_; }
  bool IsBound() const noexcept { return model_ != nullptr; }

 private:
  std::shared_ptr<ModelType> model_;
  std::string slot_name_;
  std::string model_id_;
};

class EmbeddingCall {
 public:
  using ModelType = IEmbeddingModel;
  EmbeddingCall() = default;
  explicit EmbeddingCall(std::shared_ptr<ModelType> model,
                         std::string slot_name = "encoder",
                         std::string model_id = {})
      : model_(std::move(model)),
        slot_name_(std::move(slot_name)),
        model_id_(std::move(model_id)) {}
  EmbeddingCall(const EmbeddingCall&) = delete;
  EmbeddingCall& operator=(const EmbeddingCall&) = delete;
  EmbeddingCall(EmbeddingCall&&) noexcept = default;
  EmbeddingCall& operator=(EmbeddingCall&&) noexcept = default;

  NodeResult<EmbeddingBatch> Embed(
      const TextBatch& inputs,
      const EmbeddingOptions& options = EmbeddingOptions{}) const {
    return detail::InvokeAlignedModel<EmbeddingBatch>(
        inputs, model_, "Embedding", "embed", slot_name_,
        [&](ModelType& model, EmbeddingBatch* outputs,
            std::string* diagnostic) {
          return model.Embed(inputs, options, outputs, diagnostic);
        });
  }

  const std::string& SlotName() const noexcept { return slot_name_; }
  const std::string& ModelId() const noexcept { return model_id_; }
  bool IsBound() const noexcept { return model_ != nullptr; }

 private:
  std::shared_ptr<ModelType> model_;
  std::string slot_name_;
  std::string model_id_;
};

class AsrCall {
 public:
  using ModelType = IAsrModel;
  AsrCall() = default;
  explicit AsrCall(std::shared_ptr<ModelType> model,
                   std::string slot_name = "transcriber",
                   std::string model_id = {})
      : model_(std::move(model)),
        slot_name_(std::move(slot_name)),
        model_id_(std::move(model_id)) {}
  AsrCall(const AsrCall&) = delete;
  AsrCall& operator=(const AsrCall&) = delete;
  AsrCall(AsrCall&&) noexcept = default;
  AsrCall& operator=(AsrCall&&) noexcept = default;

  NodeResult<TextBatch> Transcribe(const AudioPcmBatch& inputs) const {
    return detail::InvokeAlignedModel<TextBatch>(
        inputs, model_, "ASR", "transcribe", slot_name_,
        [&](ModelType& model, TextBatch* outputs, std::string* diagnostic) {
          return model.Transcribe(inputs, outputs, diagnostic);
        });
  }

  const std::string& SlotName() const noexcept { return slot_name_; }
  const std::string& ModelId() const noexcept { return model_id_; }
  bool IsBound() const noexcept { return model_ != nullptr; }

 private:
  std::shared_ptr<ModelType> model_;
  std::string slot_name_;
  std::string model_id_;
};

class OcrCall {
 public:
  using ModelType = IOcrModel;
  OcrCall() = default;
  explicit OcrCall(std::shared_ptr<ModelType> model,
                   std::string slot_name = "detector",
                   std::string model_id = {})
      : model_(std::move(model)),
        slot_name_(std::move(slot_name)),
        model_id_(std::move(model_id)) {}
  OcrCall(const OcrCall&) = delete;
  OcrCall& operator=(const OcrCall&) = delete;
  OcrCall(OcrCall&&) noexcept = default;
  OcrCall& operator=(OcrCall&&) noexcept = default;

  NodeResult<OcrDocumentBatch> Recognize(const ImageRefBatch& inputs) const {
    return detail::InvokeAlignedModel<OcrDocumentBatch>(
        inputs, model_, "OCR", "recognize", slot_name_,
        [&](ModelType& model, OcrDocumentBatch* outputs,
            std::string* diagnostic) {
          return model.Recognize(inputs, outputs, diagnostic);
        });
  }

  const std::string& SlotName() const noexcept { return slot_name_; }
  const std::string& ModelId() const noexcept { return model_id_; }
  bool IsBound() const noexcept { return model_ != nullptr; }

 private:
  std::shared_ptr<ModelType> model_;
  std::string slot_name_;
  std::string model_id_;
};

class RerankCall {
 public:
  using ModelType = IRerankModel;
  RerankCall() = default;
  explicit RerankCall(std::shared_ptr<ModelType> model,
                      std::string slot_name = "reranker",
                      std::string model_id = {})
      : model_(std::move(model)),
        slot_name_(std::move(slot_name)),
        model_id_(std::move(model_id)) {}
  RerankCall(const RerankCall&) = delete;
  RerankCall& operator=(const RerankCall&) = delete;
  RerankCall(RerankCall&&) noexcept = default;
  RerankCall& operator=(RerankCall&&) noexcept = default;

  NodeResult<ScoreBatch> Score(const QueryCandidatesBatch& inputs) const {
    return detail::InvokeAlignedModel<ScoreBatch>(
        inputs, model_, "Rerank", "score", slot_name_,
        [&](ModelType& model, ScoreBatch* outputs, std::string* diagnostic) {
          return model.Score(inputs, outputs, diagnostic);
        });
  }

  const std::string& SlotName() const noexcept { return slot_name_; }
  const std::string& ModelId() const noexcept { return model_id_; }
  bool IsBound() const noexcept { return model_ != nullptr; }

 private:
  std::shared_ptr<ModelType> model_;
  std::string slot_name_;
  std::string model_id_;
};

struct NoModels {};

}  // namespace llm_edgeflow
