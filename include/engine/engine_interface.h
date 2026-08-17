#pragma once

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/traceable_item.h"

namespace alg_framework {

/**
 * @brief 所有底层硬件模型引擎的基类抽象
 */
class IModelEngine {
 public:
  virtual ~IModelEngine() = default;

  /**
   * @brief 加载模型与底层硬件资源配置
   * @param model_path 模型文件或权重路径
   * @param engine_config 引擎专属配置 (如 npu_core_id, precision, max_seq_len)
   */
  virtual bool Load(const std::string& model_path,
                    const nlohmann::json& engine_config) = 0;

  /**
   * @brief 获取底层模型固化编译的 Max Batch Size (静态 Batch 大小)
   */
  virtual size_t GetMaxBatchSize() const = 0;

  /**
   * @brief 引擎类型名称标识
   */
  virtual const std::string& EngineType() const = 0;
};

/**
 * @brief Embedding 向量提取模型引擎纯虚抽象
 */
class IEmbeddingEngine : public IModelEngine {
 public:
  /**
   * @brief 具备溯源能力的批量 Embedding 计算接口
   * @param input_texts 带有 req_id 和 sub_id 标签的输入文本数组
   * @param output_embeddings 计算得到的带有对应标签的 Embedding 向量数组
   */
  virtual int InferTraceableBatch(
      const std::vector<TraceableItem<std::string>>& input_texts,
      std::vector<TraceableItem<std::vector<float>>>* output_embeddings) = 0;
};

/**
 * @brief Rerank / Cross-Encoder 语义精排打分模型引擎纯虚抽象
 */
class IRerankEngine : public IModelEngine {
 public:
  struct PairInput {
    std::string query;
    std::string passage;
  };

  /**
   * @brief 具备溯源能力的批量 (Query, Passage) 精排打分接口
   */
  virtual int ScoreTraceableBatch(
      const std::vector<TraceableItem<PairInput>>& input_pairs,
      std::vector<TraceableItem<float>>* output_scores) = 0;
};

/**
 * @brief LLM 大语言模型推理引擎纯虚抽象
 */
class ILlmEngine : public IModelEngine {
 public:
  struct GenerateOption {
    int max_tokens = 128;
    float temperature = 0.7f;
    float top_p = 0.9f;
    std::vector<std::string> stop_words;
  };

  /**
   * @brief 单条 Prompt 推理生成
   */
  virtual int Generate(const std::string& prompt, const GenerateOption& opt,
                       std::string* output_text) = 0;

  /**
   * @brief 具备溯源能力的批量 LLM 文本生成接口
   */
  virtual int InferTraceableBatch(
      const std::vector<TraceableItem<std::string>>& input_prompts,
      const GenerateOption& opt,
      std::vector<TraceableItem<std::string>>* output_texts) = 0;
};

/**
 * @brief OCR / 视觉文档检测与文字识别模型引擎纯虚抽象
 */
class IOcrEngine : public IModelEngine {
 public:
  struct OcrBoxItem {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::string text;
    float confidence = 0.0f;
  };

  /**
   * @brief 具备溯源能力的批量图片 OCR 检测与识别接口
   */
  virtual int InferTraceableBatch(
      const std::vector<TraceableItem<std::string>>& input_image_paths,
      std::vector<TraceableItem<std::vector<OcrBoxItem>>>* output_boxes) = 0;
};

/**
 * @brief 语音识别 ASR (Speech-to-Text) 时序模型引擎纯虚抽象
 */
class IAudioAsrEngine : public IModelEngine {
 public:
  struct AudioPcmData {
    std::vector<float> pcm_data;
    int sample_rate = 16000;
  };

  /**
   * @brief 具备溯源能力的批量音频 PCM 语音转写文本接口
   */
  virtual int InferTraceableBatch(
      const std::vector<TraceableItem<AudioPcmData>>& input_audio,
      std::vector<TraceableItem<std::string>>* output_transcripts) = 0;
};

}  // namespace alg_framework
