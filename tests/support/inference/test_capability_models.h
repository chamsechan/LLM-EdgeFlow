#pragma once

#include <string>
#include <utility>
#include <vector>

#include "engine/fixed_batch_executor.h"
#include "engine/model_interface.h"

namespace alg_framework {
namespace test {

/**
 * @brief 仅供 Node 契约测试的中性 OCR typed Model。
 *
 * 不注册到 ModelRegistry，不链接到 alg_sdk，不模拟任何业务回答。
 */
class TestOcrModel final : public IOcrModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "test_ocr_model";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "ocr";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 2; }

  int Recognize(const ImageRefBatch& images,
                OcrDocumentBatch* outputs) noexcept override {
    if (fail_) {
      if (outputs) outputs->clear();
      return -1;
    }
    const BatchPolicy policy{GetMaxBatchSize(), GetMaxBatchSize()};
    const int result =
        FixedBatchExecutor::Execute<std::string, OcrDocumentItem>(
            images, policy,
            [&images](const BatchSlice& slice,
                      std::vector<OcrDocumentItem>* batch_outputs) {
              batch_outputs->clear();
              batch_outputs->reserve(slice.execution_count);
              for (size_t i = 0; i < slice.execution_count; ++i) {
                OcrDocumentItem document;
                if (i < slice.valid_count) {
                  const std::string& image_ref = images[slice.offset + i].data;
                  const std::string text = "recognized:" + image_ref;
                  document.boxes.push_back(
                      {1.0f, 2.0f, 3.0f, 4.0f, text, 0.95f});
                  document.combined_text = text;
                }
                batch_outputs->push_back(std::move(document));
              }
              return 0;
            },
            outputs);
    if (result != 0 || !outputs) return result;
    if (return_wrong_count_ && !outputs->empty()) outputs->pop_back();
    if (corrupt_provenance_ && !outputs->empty()) ++(*outputs)[0].sub_id;
    return 0;
  }

  bool fail_ = false;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

/**
 * @brief 仅供 Node 契约测试的中性 ASR typed Model。
 */
class TestAsrModel final : public IAsrModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string type = "test_asr_model";
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "asr";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 2; }

  int Transcribe(const AudioPcmBatch& audio,
                 TextBatch* outputs) noexcept override {
    if (fail_) {
      if (outputs) outputs->clear();
      return -1;
    }
    const BatchPolicy policy{GetMaxBatchSize(), GetMaxBatchSize()};
    const int result =
        FixedBatchExecutor::Execute<AudioPcmPayload, std::string>(
            audio, policy,
            [&audio](const BatchSlice& slice,
                     std::vector<std::string>* batch_outputs) {
              batch_outputs->clear();
              batch_outputs->reserve(slice.execution_count);
              for (size_t i = 0; i < slice.execution_count; ++i) {
                if (i < slice.valid_count) {
                  const auto& sample = audio[slice.offset + i].data;
                  batch_outputs->push_back(
                      "transcript:" + std::to_string(sample.sample_rate) + ":" +
                      std::to_string(sample.pcm_data.size()));
                } else {
                  batch_outputs->emplace_back();
                }
              }
              return 0;
            },
            outputs);
    if (result != 0 || !outputs) return result;
    if (return_wrong_count_ && !outputs->empty()) outputs->pop_back();
    if (corrupt_provenance_ && !outputs->empty()) ++(*outputs)[0].sub_id;
    return 0;
  }

  bool fail_ = false;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

}  // namespace test
}  // namespace alg_framework
