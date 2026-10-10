#pragma once

#include <memory>
#include <string>
#include <string_view>
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
    const InputBatchT& inputs, OutputBatchT&& outputs, const std::string& label,
    const std::string& slot_name) {
  auto alignment = ValidatePreservedTraceableAlignment(inputs, outputs);
  if (alignment.error == TraceableAlignmentError::kCountMismatch) {
    return NodeResult<OutputBatchT>::Failure(
        NodeErrorKind::kOutputCountMismatch, label + " output count mismatch",
        node_error::author_node::kOutputCountMismatch, "align", slot_name);
  }
  if (alignment.error == TraceableAlignmentError::kProvenanceMismatch) {
    return NodeResult<OutputBatchT>::Failure(
        NodeErrorKind::kOutputProvenanceMismatch,
        label + " output provenance mismatch",
        node_error::author_node::kOutputProvenanceMismatch, "align", slot_name);
  }
  return NodeResult<OutputBatchT>::Success(std::forward<OutputBatchT>(outputs));
}

template <typename OutputBatchT, typename InputBatchT, typename ModelT,
          typename InvokeT>
NodeResult<OutputBatchT> InvokeAlignedModel(
    const InputBatchT& inputs, const std::shared_ptr<ModelT>& model,
    const std::string& label, const std::string& operation,
    const std::string& slot_name, InvokeT invoke) {
  if (inputs.empty()) return NodeResult<OutputBatchT>::Success({});
  if (!model) {
    return NodeResult<OutputBatchT>::Failure(
        NodeErrorKind::kModelCallError, label + " model is null",
        node_error::author_node::kModelCallFailed, operation, slot_name);
  }
  OutputBatchT outputs;
  std::string diagnostic;
  const int code = invoke(*model, &outputs, &diagnostic);
  if (code != 0) {
    return NodeResult<OutputBatchT>::Failure(
        NodeErrorKind::kModelCallError,
        label + " " + operation + " failed with code " + std::to_string(code) +
            (diagnostic.empty() ? "" : ": " + diagnostic),
        code, operation, slot_name);
  }
  return ConvertAlignedOutputs(inputs, std::move(outputs), label, slot_name);
}

}  // namespace detail

namespace detail {

// 持有一个已绑定的模型能力及其槽位标识。调用对象只能移动，
// 因此每个 Node 实例独占其绑定。
template <typename ModelT>
class ModelCallBase {
 public:
  using Interface = ModelT;

  const std::string& SlotName() const noexcept { return slot_name_; }
  const std::string& ModelName() const noexcept { return model_name_; }
  bool IsBound() const noexcept { return model_ != nullptr; }

 protected:
  ModelCallBase() = default;
  ModelCallBase(std::shared_ptr<ModelT> model, std::string slot_name,
                std::string model_name)
      : model_(std::move(model)),
        slot_name_(std::move(slot_name)),
        model_name_(std::move(model_name)) {}
  ModelCallBase(const ModelCallBase&) = delete;
  ModelCallBase& operator=(const ModelCallBase&) = delete;
  ModelCallBase(ModelCallBase&&) noexcept = default;
  ModelCallBase& operator=(ModelCallBase&&) noexcept = default;
  ~ModelCallBase() = default;
  const ModelT* BoundModel() const noexcept { return model_.get(); }

  template <typename OutputBatchT, typename InputBatchT, typename InvokeT>
  NodeResult<OutputBatchT> Invoke(const InputBatchT& inputs, const char* label,
                                  const char* operation, InvokeT invoke) const {
    return InvokeAlignedModel<OutputBatchT>(inputs, model_, label, operation,
                                            slot_name_, std::move(invoke));
  }

 private:
  std::shared_ptr<ModelT> model_;
  std::string slot_name_;
  std::string model_name_;
};

}  // namespace detail

class LlmCall : public detail::ModelCallBase<ILlmModel> {
 public:
  LlmCall() = default;
  explicit LlmCall(std::shared_ptr<Interface> model,
                   std::string slot_name = "generator",
                   std::string model_name = {})
      : ModelCallBase(std::move(model), std::move(slot_name),
                      std::move(model_name)) {}

  bool SupportsRandomSeed() const noexcept {
    return BoundModel() && BoundModel()->SupportsRandomSeed();
  }

  NodeResult<TextBatch> Generate(
      const TextBatch& inputs,
      const GenerateOptions& options = GenerateOptions{}) const {
    return Invoke<TextBatch>(
        inputs, "LLM", "generate",
        [&](Interface& model, TextBatch* outputs, std::string* diagnostic) {
          return model.Generate(inputs, options, outputs, diagnostic);
        });
  }
};

class EmbeddingCall : public detail::ModelCallBase<IEmbeddingModel> {
 public:
  EmbeddingCall() = default;
  explicit EmbeddingCall(std::shared_ptr<Interface> model,
                         std::string slot_name = "encoder",
                         std::string model_name = {})
      : ModelCallBase(std::move(model), std::move(slot_name),
                      std::move(model_name)) {}

  NodeResult<EmbeddingBatch> Embed(const TextBatch& inputs) const {
    return Invoke<EmbeddingBatch>(inputs, "Embedding", "embed",
                                  [&](Interface& model, EmbeddingBatch* outputs,
                                      std::string* diagnostic) {
                                    return model.Embed(inputs, outputs,
                                                       diagnostic);
                                  });
  }
};

class AsrCall : public detail::ModelCallBase<IAsrModel> {
 public:
  AsrCall() = default;
  explicit AsrCall(std::shared_ptr<Interface> model,
                   std::string slot_name = "transcriber",
                   std::string model_name = {})
      : ModelCallBase(std::move(model), std::move(slot_name),
                      std::move(model_name)) {}

  bool SupportsLanguage(std::string_view language) const noexcept {
    return BoundModel() && BoundModel()->SupportsLanguage(language);
  }

  NodeResult<TextBatch> Transcribe(const AudioPcmBatch& inputs,
                                   const TranscribeOptions& options) const {
    return Invoke<TextBatch>(
        inputs, "ASR", "transcribe",
        [&](Interface& model, TextBatch* outputs, std::string* diagnostic) {
          return model.Transcribe(inputs, options, outputs, diagnostic);
        });
  }
};

class OcrCall : public detail::ModelCallBase<IOcrModel> {
 public:
  OcrCall() = default;
  explicit OcrCall(std::shared_ptr<Interface> model,
                   std::string slot_name = "detector",
                   std::string model_name = {})
      : ModelCallBase(std::move(model), std::move(slot_name),
                      std::move(model_name)) {}

  NodeResult<OcrDocumentBatch> Recognize(const ImageFrameBatch& inputs) const {
    return Invoke<OcrDocumentBatch>(
        inputs, "OCR", "recognize",
        [&](Interface& model, OcrDocumentBatch* outputs,
            std::string* diagnostic) {
          return model.Recognize(inputs, outputs, diagnostic);
        });
  }
};

class RerankCall : public detail::ModelCallBase<IRerankModel> {
 public:
  RerankCall() = default;
  explicit RerankCall(std::shared_ptr<Interface> model,
                      std::string slot_name = "reranker",
                      std::string model_name = {})
      : ModelCallBase(std::move(model), std::move(slot_name),
                      std::move(model_name)) {}

  NodeResult<ScoreBatch> Score(const QueryCandidatesBatch& inputs) const {
    return Invoke<ScoreBatch>(
        inputs, "Rerank", "score",
        [&](Interface& model, ScoreBatch* outputs, std::string* diagnostic) {
          return model.Score(inputs, outputs, diagnostic);
        });
  }
};

struct NoModels {};

}  // namespace llm_edgeflow
