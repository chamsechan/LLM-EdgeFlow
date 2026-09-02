#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "contracts/inference_payloads.h"
#include "engine/inference_definition.h"

namespace llm_edgeflow {

/**
 * @brief 所有模型语义对象的统一抽象基类
 */
class IModel {
 public:
  virtual ~IModel() = default;

  virtual const std::string& ModelType() const noexcept = 0;
  virtual const std::string& Capability() const noexcept = 0;

  // Describes only Model semantic reentrancy. Runtime planning combines this
  // value with the selected Backend concurrency and applies the stricter one.
  virtual InferenceConcurrency Concurrency() const noexcept = 0;
  virtual size_t GetMaxBatchSize() const noexcept = 0;
};

/**
 * @brief Embedding 向量化模型能力接口
 */
class IEmbeddingModel : public IModel {
 public:
  virtual int Embed(const TextBatch& inputs, const EmbeddingOptions& options,
                    EmbeddingBatch* outputs) noexcept = 0;
};

/**
 * @brief Rerank 语义精排打分模型能力接口
 */
class IRerankModel : public IModel {
 public:
  virtual int Score(const QueryCandidatesBatch& inputs,
                    ScoreBatch* outputs) noexcept = 0;
};

/**
 * @brief LLM 大语言模型生成能力接口
 */
class ILlmModel : public IModel {
 public:
  virtual int Generate(const TextBatch& prompts, const GenerateOptions& options,
                       TextBatch* outputs) noexcept = 0;
};

/**
 * @brief OCR 视觉文档检测与识别模型能力接口
 */
class IOcrModel : public IModel {
 public:
  virtual int Recognize(const ImageRefBatch& images,
                        OcrDocumentBatch* outputs) noexcept = 0;
};

/**
 * @brief ASR 语音转写模型能力接口
 */
class IAsrModel : public IModel {
 public:
  virtual int Transcribe(const AudioPcmBatch& audio,
                         TextBatch* outputs) noexcept = 0;
};

}  // namespace llm_edgeflow
