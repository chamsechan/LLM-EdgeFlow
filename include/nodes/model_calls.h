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

namespace detail {

// Owns one bound model capability and its slot identity. Calls are move-only,
// so each Node instance keeps exclusive ownership of its bindings.
template <typename ModelT>
class ModelCallBase {
 public:
  using ModelType = ModelT;

  const std::string& SlotName() const noexcept { return slot_name_; }
  const std::string& ModelId() const noexcept { return model_id_; }
  bool IsBound() const noexcept { return model_ != nullptr; }

 protected:
  ModelCallBase() = default;
  ModelCallBase(std::shared_ptr<ModelT> model, std::string slot_name,
                std::string model_id)
      : model_(std::move(model)),
        slot_name_(std::move(slot_name)),
        model_id_(std::move(model_id)) {}
  ModelCallBase(const ModelCallBase&) = delete;
  ModelCallBase& operator=(const ModelCallBase&) = delete;
  ModelCallBase(ModelCallBase&&) noexcept = default;
  ModelCallBase& operator=(ModelCallBase&&) noexcept = default;
  ~ModelCallBase() = default;

  template <typename OutputBatchT, typename InputBatchT, typename InvokeT>
  NodeResult<OutputBatchT> Invoke(const InputBatchT& inputs,
                                  const char* model_name, const char* operation,
                                  InvokeT invoke) const {
    return InvokeAlignedModel<OutputBatchT>(
        inputs, model_, model_name, operation, slot_name_, std::move(invoke));
  }

 private:
  std::shared_ptr<ModelT> model_;
  std::string slot_name_;
  std::string model_id_;
};

}  // namespace detail

class LlmCall : public detail::ModelCallBase<ILlmModel> {
 public:
  LlmCall() = default;
  explicit LlmCall(std::shared_ptr<ModelType> model,
                   std::string slot_name = "generator",
                   std::string model_id = {})
      : ModelCallBase(std::move(model), std::move(slot_name),
                      std::move(model_id)) {}

  NodeResult<TextBatch> Generate(
      const TextBatch& inputs,
      const GenerateOptions& options = GenerateOptions{}) const {
    return Invoke<TextBatch>(
        inputs, "LLM", "generate",
        [&](ModelType& model, TextBatch* outputs, std::string* diagnostic) {
          return model.Generate(inputs, options, outputs, diagnostic);
        });
  }
};

class EmbeddingCall : public detail::ModelCallBase<IEmbeddingModel> {
 public:
  EmbeddingCall() = default;
  explicit EmbeddingCall(std::shared_ptr<ModelType> model,
                         std::string slot_name = "encoder",
                         std::string model_id = {})
      : ModelCallBase(std::move(model), std::move(slot_name),
                      std::move(model_id)) {}

  NodeResult<EmbeddingBatch> Embed(
      const TextBatch& inputs,
      const EmbeddingOptions& options = EmbeddingOptions{}) const {
    return Invoke<EmbeddingBatch>(inputs, "Embedding", "embed",
                                  [&](ModelType& model, EmbeddingBatch* outputs,
                                      std::string* diagnostic) {
                                    return model.Embed(inputs, options, outputs,
                                                       diagnostic);
                                  });
  }
};

class AsrCall : public detail::ModelCallBase<IAsrModel> {
 public:
  AsrCall() = default;
  explicit AsrCall(std::shared_ptr<ModelType> model,
                   std::string slot_name = "transcriber",
                   std::string model_id = {})
      : ModelCallBase(std::move(model), std::move(slot_name),
                      std::move(model_id)) {}

  NodeResult<TextBatch> Transcribe(const AudioPcmBatch& inputs) const {
    return Invoke<TextBatch>(
        inputs, "ASR", "transcribe",
        [&](ModelType& model, TextBatch* outputs, std::string* diagnostic) {
          return model.Transcribe(inputs, outputs, diagnostic);
        });
  }
};

class OcrCall : public detail::ModelCallBase<IOcrModel> {
 public:
  OcrCall() = default;
  explicit OcrCall(std::shared_ptr<ModelType> model,
                   std::string slot_name = "detector",
                   std::string model_id = {})
      : ModelCallBase(std::move(model), std::move(slot_name),
                      std::move(model_id)) {}

  NodeResult<OcrDocumentBatch> Recognize(const ImageRefBatch& inputs) const {
    return Invoke<OcrDocumentBatch>(
        inputs, "OCR", "recognize",
        [&](ModelType& model, OcrDocumentBatch* outputs,
            std::string* diagnostic) {
          return model.Recognize(inputs, outputs, diagnostic);
        });
  }
};

class RerankCall : public detail::ModelCallBase<IRerankModel> {
 public:
  RerankCall() = default;
  explicit RerankCall(std::shared_ptr<ModelType> model,
                      std::string slot_name = "reranker",
                      std::string model_id = {})
      : ModelCallBase(std::move(model), std::move(slot_name),
                      std::move(model_id)) {}

  NodeResult<ScoreBatch> Score(const QueryCandidatesBatch& inputs) const {
    return Invoke<ScoreBatch>(
        inputs, "Rerank", "score",
        [&](ModelType& model, ScoreBatch* outputs, std::string* diagnostic) {
          return model.Score(inputs, outputs, diagnostic);
        });
  }
};

struct NoModels {};

}  // namespace llm_edgeflow
