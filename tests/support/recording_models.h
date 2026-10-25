#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "contracts/error_codes.h"
#include "engine/model_interface.h"

namespace llm_edgeflow::test_support {

// Node 测试使用的确定性 Model：记录每次调用，第 n 次调用的第 i 条输入得到
// replies[n][i]，并保留该条输入的 (req_id, sub_id)。fail_call 从 1 开始计数，
// 0 表示从不失败。记录与按调用次数选择回答都不加锁，因此声明为串行；
// 并发测试和需要额外观测点（语言检查、失败倒计数、错位输出）的测试
// 继续在本文件之外编写专用替身。
template <typename Interface, typename InputBatch, typename Reply>
class RecordingModel : public Interface {
 public:
  explicit RecordingModel(std::string type)
      : impl_("recording_" + type), type_(std::move(type)) {}

  const std::string& ImplName() const noexcept override { return impl_; }
  const std::string& ModelType() const noexcept override { return type_; }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }

  std::vector<std::vector<Reply>> replies;
  std::vector<InputBatch> calls;
  std::size_t fail_call = 0;
  int failure_code = error::kInferenceFailed;
  std::string failure_message = "fixture model failure";

 protected:
  template <typename OutputBatch>
  int Answer(const InputBatch& inputs, OutputBatch* outputs,
             std::string* diagnostic) noexcept {
    if (diagnostic) diagnostic->clear();
    calls.push_back(inputs);
    outputs->clear();
    const std::size_t call = calls.size();
    if (fail_call == call) {
      if (diagnostic) *diagnostic = failure_message;
      return failure_code;
    }
    if (call > replies.size() || replies[call - 1].size() != inputs.size()) {
      if (diagnostic) *diagnostic = "fixture replies do not cover this call";
      return error::kInferenceFailed;
    }
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      outputs->emplace_back(inputs[i].req_id, inputs[i].sub_id,
                            replies[call - 1][i]);
    }
    return 0;
  }

 private:
  std::string impl_;
  std::string type_;
};

class RecordingLlm final
    : public RecordingModel<ILlmModel, TextBatch, std::string> {
 public:
  RecordingLlm() : RecordingModel("llm") {}
  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs, std::string* diagnostic) noexcept override {
    call_options.push_back(options);
    return Answer(prompts, outputs, diagnostic);
  }
  std::vector<GenerateOptions> call_options;
};

class RecordingEmbedding final
    : public RecordingModel<IEmbeddingModel, TextBatch, std::vector<float>> {
 public:
  RecordingEmbedding() : RecordingModel("embedding") {}
  int Embed(const TextBatch& inputs, EmbeddingBatch* outputs,
            std::string* diagnostic) noexcept override {
    return Answer(inputs, outputs, diagnostic);
  }
};

class RecordingAsr final
    : public RecordingModel<IAsrModel, AudioPcmBatch, std::string> {
 public:
  RecordingAsr() : RecordingModel("asr") {}
  bool SupportsLanguage(std::string_view) const noexcept override {
    return true;
  }
  int Transcribe(const AudioPcmBatch& audio, const TranscribeOptions& options,
                 TextBatch* outputs,
                 std::string* diagnostic) noexcept override {
    call_options.push_back(options);
    return Answer(audio, outputs, diagnostic);
  }
  std::vector<TranscribeOptions> call_options;
};

class RecordingOcr final
    : public RecordingModel<IOcrModel, ImageFrameBatch, OcrDocumentItem> {
 public:
  RecordingOcr() : RecordingModel("ocr") {}
  int Recognize(const ImageFrameBatch& images, OcrDocumentBatch* outputs,
                std::string* diagnostic) noexcept override {
    return Answer(images, outputs, diagnostic);
  }
};

class RecordingRerank final
    : public RecordingModel<IRerankModel, QueryCandidatesBatch, float> {
 public:
  RecordingRerank() : RecordingModel("rerank") {}
  int Score(const QueryCandidatesBatch& inputs, ScoreBatch* outputs,
            std::string* diagnostic) noexcept override {
    return Answer(inputs, outputs, diagnostic);
  }
};

}  // namespace llm_edgeflow::test_support
