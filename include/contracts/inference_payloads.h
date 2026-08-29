#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "contracts/traceable_item.h"

namespace alg_framework {

// ==============================================================================
// 1. 标准可溯源推理载荷 (Standard Traceable Inference Payloads)
// ==============================================================================

/**
 * @brief 标准可溯源文本批次 (TextBatch)
 */
using TextBatch = std::vector<TraceableItem<std::string>>;

/**
 * @brief 标准可溯源浮点特征向量批次 (EmbeddingBatch)
 */
using EmbeddingBatch = std::vector<TraceableItem<std::vector<float>>>;

/**
 * @brief 查询-候选样本对载荷 (QueryCandidatePair)
 */
struct QueryCandidatePair {
  std::string query;
  std::string candidate;

  QueryCandidatePair() = default;
  QueryCandidatePair(std::string q, std::string c)
      : query(std::move(q)), candidate(std::move(c)) {}
};

/**
 * @brief 标准可溯源精排样本对批次 (QueryCandidatesBatch)
 */
using QueryCandidatesBatch = std::vector<TraceableItem<QueryCandidatePair>>;

/**
 * @brief 标准可溯源打分批次 (ScoreBatch)
 */
using ScoreBatch = std::vector<TraceableItem<float>>;

/**
 * @brief 音频 PCM 浮点时序载荷 (AudioPcmPayload)
 */
struct AudioPcmPayload {
  std::vector<float> pcm_data;
  int sample_rate = 16000;

  AudioPcmPayload() = default;
  AudioPcmPayload(std::vector<float> data, int rate = 16000)
      : pcm_data(std::move(data)), sample_rate(rate) {}
};

/**
 * @brief 标准可溯源音频批次 (AudioPcmBatch)
 */
using AudioPcmBatch = std::vector<TraceableItem<AudioPcmPayload>>;

/**
 * @brief 图像文件路径或引用批次 (ImageRefBatch) - 具有强类型特质
 */
struct ImageRefBatch : public std::vector<TraceableItem<std::string>> {
  using std::vector<TraceableItem<std::string>>::vector;
  ImageRefBatch() = default;
  ImageRefBatch(std::vector<TraceableItem<std::string>> v)
      : std::vector<TraceableItem<std::string>>(std::move(v)) {}
};

/**
 * @brief OCR 矩形边界与文本识别框 (OcrBoxRecord)
 */
struct OcrBoxRecord {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  std::string text;
  float confidence = 0.0f;
};

/**
 * @brief OCR 文档识别结果项 (OcrDocumentItem)
 */
struct OcrDocumentItem {
  std::vector<OcrBoxRecord> boxes;
  std::string combined_text;
};

/**
 * @brief 标准可溯源 OCR 识别文档批次 (OcrDocumentBatch)
 */
using OcrDocumentBatch = std::vector<TraceableItem<OcrDocumentItem>>;

// ==============================================================================
// 2. 推理选项契约 (Inference Options Contracts)
// ==============================================================================

/**
 * @brief 向量提取推理选项
 */
struct EmbeddingOptions {
  bool normalize = true;
};

/**
 * @brief LLM 文本生成推理选项
 */
struct GenerateOptions {
  int max_tokens = 128;
  float temperature = 0.7f;
  float top_p = 0.9f;
  std::vector<std::string> stop_words;
};

}  // namespace alg_framework
