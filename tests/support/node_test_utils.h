#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "contracts/inference_payloads.h"
#include "core/node_interface.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "core/validated_node_plan.h"
#include "engine/model_interface.h"
#include "tests/support/node_plan_fixture.h"

namespace llm_edgeflow {

// The caller keeps the session alive through Node destruction, as in
// production.
struct NodeFixturePlans {
  std::mutex mutex;
  std::vector<std::shared_ptr<ValidatedNodePlan>> plans;
};

inline bool InitNodeForTest(
    INode& node, const nlohmann::json& config, SessionContext* session_ctx,
    std::string* diagnostic = nullptr,
    const std::unordered_set<std::string>& omitted = {}) {
  if (!session_ctx) return false;
  auto plan =
      PrepareNodePlanForTest(node.Name(), config, omitted, "", "", diagnostic);
  if (!plan) return false;
  auto owner = session_ctx->GetOrCreateResource(
      SessionResourceKey<NodeFixturePlans>{"node_fixture_plans"},
      [] { return std::make_shared<NodeFixturePlans>(); });
  {
    std::lock_guard<std::mutex> lock(owner->mutex);
    owner->plans.push_back(plan);
  }
  return node.Init({plan.get(), session_ctx, diagnostic});
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

  int Generate(const TextBatch& prompts, const GenerateOptions&,
               TextBatch* outputs,
               std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
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

  int Embed(const TextBatch& inputs, const EmbeddingOptions&,
            EmbeddingBatch* outputs,
            std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
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

  int Score(const QueryCandidatesBatch& inputs, ScoreBatch* outputs,
            std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
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

  int Recognize(const ImageRefBatch& images, OcrDocumentBatch* outputs,
                std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
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

  int Transcribe(const AudioPcmBatch& audio, TextBatch* outputs,
                 std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
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
