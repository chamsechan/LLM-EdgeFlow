#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "contracts/inference_payloads.h"
#include "core/node_interface.h"
#include "core/session_context.h"
#include "engine/model_interface.h"

namespace llm_edgeflow {

inline bool InitNodeForTest(INode& node, const nlohmann::json& config,
                            SessionContext* session_ctx) {
  NodeInitContext init_ctx;
  init_ctx.config = &config;
  init_ctx.session_ctx = session_ctx;
  return node.Init(init_ctx);
}

inline bool InitNodeWithPlan(INode& node, const nlohmann::json& config,
                             SessionContext* session_ctx,
                             const ValidatedNodePlan* plan) {
  NodeInitContext init_ctx;
  init_ctx.config = &config;
  init_ctx.session_ctx = session_ctx;
  init_ctx.plan = plan;
  return node.Init(init_ctx);
}

namespace test {

class ControlledMockLlmModel final : public ILlmModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "mock_llm";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string cap = "llm";
    return cap;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 8; }

  int Generate(const TextBatch& prompts, const GenerateOptions&,
               TextBatch* outputs) noexcept override {
    if (fail_) {
      if (outputs) outputs->clear();
      return -8901;
    }
    if (!outputs) return -8902;
    outputs->clear();
    for (const auto& item : prompts) {
      outputs->emplace_back(item.req_id, item.sub_id,
                            "mock_answer:" + item.data);
    }
    if (return_wrong_count_ && !outputs->empty()) {
      outputs->pop_back();
    }
    if (corrupt_provenance_ && !outputs->empty()) {
      ++(*outputs)[0].sub_id;
    }
    return 0;
  }

  bool fail_ = false;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

class ControlledMockEmbeddingModel final : public IEmbeddingModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "mock_embedding";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string cap = "embedding";
    return cap;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 8; }

  int Embed(const TextBatch& inputs, const EmbeddingOptions&,
            EmbeddingBatch* outputs) noexcept override {
    if (fail_) {
      if (outputs) outputs->clear();
      return -8901;
    }
    if (!outputs) return -8902;
    outputs->clear();
    for (const auto& item : inputs) {
      outputs->emplace_back(item.req_id, item.sub_id,
                            std::vector<float>{0.1f, 0.2f, 0.3f});
    }
    if (return_wrong_count_ && !outputs->empty()) {
      outputs->pop_back();
    }
    if (corrupt_provenance_ && !outputs->empty()) {
      ++(*outputs)[0].sub_id;
    }
    return 0;
  }

  bool fail_ = false;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

class ControlledMockRerankModel final : public IRerankModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "mock_rerank";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string cap = "rerank";
    return cap;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 8; }

  int Score(const QueryCandidatesBatch& inputs,
            ScoreBatch* outputs) noexcept override {
    if (fail_) {
      if (outputs) outputs->clear();
      return -8901;
    }
    if (!outputs) return -8902;
    outputs->clear();
    for (const auto& item : inputs) {
      outputs->emplace_back(item.req_id, item.sub_id, 0.95f);
    }
    if (return_wrong_count_ && !outputs->empty()) {
      outputs->pop_back();
    }
    if (corrupt_provenance_ && !outputs->empty()) {
      ++(*outputs)[0].sub_id;
    }
    return 0;
  }

  bool fail_ = false;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

class ControlledMockOcrModel final : public IOcrModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "mock_ocr";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string cap = "ocr";
    return cap;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 8; }

  int Recognize(const ImageRefBatch& images,
                OcrDocumentBatch* outputs) noexcept override {
    if (fail_) {
      if (outputs) outputs->clear();
      return -8901;
    }
    if (!outputs) return -8902;
    outputs->clear();
    for (const auto& item : images) {
      OcrDocumentItem doc;
      doc.combined_text = "ocr:" + item.data;
      doc.boxes.push_back({0.0f, 0.0f, 10.0f, 10.0f, doc.combined_text, 0.99f});
      outputs->emplace_back(item.req_id, item.sub_id, std::move(doc));
    }
    if (return_wrong_count_ && !outputs->empty()) {
      outputs->pop_back();
    }
    if (corrupt_provenance_ && !outputs->empty()) {
      ++(*outputs)[0].sub_id;
    }
    return 0;
  }

  bool fail_ = false;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

class ControlledMockAsrModel final : public IAsrModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "mock_asr";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string cap = "asr";
    return cap;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 8; }

  int Transcribe(const AudioPcmBatch& audio,
                 TextBatch* outputs) noexcept override {
    if (fail_) {
      if (outputs) outputs->clear();
      return -8901;
    }
    if (!outputs) return -8902;
    outputs->clear();
    for (const auto& item : audio) {
      outputs->emplace_back(item.req_id, item.sub_id, "transcribed_audio");
    }
    if (return_wrong_count_ && !outputs->empty()) {
      outputs->pop_back();
    }
    if (corrupt_provenance_ && !outputs->empty()) {
      ++(*outputs)[0].sub_id;
    }
    return 0;
  }

  bool fail_ = false;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

}  // namespace test
}  // namespace llm_edgeflow
