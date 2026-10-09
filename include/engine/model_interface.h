#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
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

  virtual const std::string& ImplName() const noexcept = 0;
  virtual const std::string& ModelType() const noexcept = 0;

  // 仅描述 Model 语义上的可重入性。运行时规划会结合所选 Backend 的
  // 并发度，取两者中更严格的一个。
  virtual InferenceConcurrency Concurrency() const noexcept = 0;
};

// 能力调用在入口清空可选的诊断信息，失败时写入原因。诊断信息归本次调用方
// 所有，Model 不保留可变的 last-error 状态。整数返回码和输出回滚语义不变。

/**
 * @brief Embedding 向量化模型能力接口
 */
class IEmbeddingModel : public IModel {
 public:
  virtual int Embed(const TextBatch& inputs, EmbeddingBatch* outputs,
                    std::string* diagnostic = nullptr) noexcept = 0;
};

/**
 * @brief Rerank 语义精排打分模型能力接口
 */
class IRerankModel : public IModel {
 public:
  virtual int Score(const QueryCandidatesBatch& inputs, ScoreBatch* outputs,
                    std::string* diagnostic = nullptr) noexcept = 0;
};

/**
 * @brief LLM 大语言模型生成能力接口
 */
class ILlmModel : public IModel {
 public:
  virtual bool SupportsRandomSeed() const noexcept { return false; }
  virtual int Generate(const TextBatch& prompts, const GenerateOptions& options,
                       TextBatch* outputs,
                       std::string* diagnostic = nullptr) noexcept = 0;
};

/**
 * @brief OCR 视觉文档检测与识别模型能力接口
 */
class IOcrModel : public IModel {
 public:
  virtual int Recognize(const ImageRefBatch& images, OcrDocumentBatch* outputs,
                        std::string* diagnostic = nullptr) noexcept = 0;
};

/**
 * @brief ASR 语音转写模型能力接口
 */
class IAsrModel : public IModel {
 public:
  virtual bool SupportsLanguage(std::string_view language) const noexcept = 0;
  virtual int Transcribe(const AudioPcmBatch& audio,
                         const TranscribeOptions& options, TextBatch* outputs,
                         std::string* diagnostic = nullptr) noexcept = 0;
};

}  // namespace llm_edgeflow
