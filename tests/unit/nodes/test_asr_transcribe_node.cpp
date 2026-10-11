#include <gtest/gtest.h>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "engine/model_interface.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {
class RecordingAsr final : public IAsrModel {
 public:
  const std::string& ImplName() const noexcept override { return impl; }
  const std::string& ModelType() const noexcept override { return type; }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  bool SupportsLanguage(std::string_view language) const noexcept override {
    ++language_checks;
    checked_language = std::string(language);
    return supports_language;
  }
  int Transcribe(const AudioPcmBatch& audio, const TranscribeOptions& options,
                 TextBatch* output, std::string* diagnostic) noexcept override {
    if (diagnostic) diagnostic->clear();
    calls.push_back(audio);
    languages.push_back(options.language);
    output->clear();
    if (fail) {
      if (diagnostic) *diagnostic = "audio decoder failed";
      return -42;
    }
    for (size_t i = 0; i < audio.size(); ++i)
      output->emplace_back(audio[i].req_id, audio[i].sub_id, responses.at(i));
    return 0;
  }
  std::string impl = "node_recording_asr", type = "asr";
  mutable int language_checks = 0;
  mutable std::string checked_language;
  bool supports_language = true, fail = false;
  std::vector<std::string> responses = {"你好", "hello"};
  std::vector<AudioPcmBatch> calls;
  std::vector<std::string> languages;
};

TEST(AsrTranscribeNodeTest,
     ChecksLanguageOnceAndPassesAudioAndOptionsForEachCall) {
  SessionContext session;
  auto model = std::make_shared<RecordingAsr>();
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "speech", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init("asr_transcribe",
                        {{"bind_model", "speech"}, {"language", "en"}}, session,
                        {{"audio", "audio", ""}}, {{"text", "text", ""}}))
      << node.diagnostic;
  EXPECT_EQ(model->language_checks, 1);
  EXPECT_EQ(model->checked_language, "en");
  const AudioPcmBatch audio = {{9, 4, AudioPcmPayload({0.5f, -0.25f}, 32000)},
                               {3, 7, AudioPcmPayload({0.75f}, 16000)}};
  for (int repeat = 0; repeat < 2; ++repeat) {
    AlgContext context;
    ASSERT_TRUE(context.Publish("audio", audio));
    ASSERT_EQ(node.node->Process(&context), 0);
    const auto* output = context.Read<TextBatch>("text");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 2u);
    EXPECT_EQ((*output)[0].data, "你好");
    EXPECT_EQ((*output)[0].req_id, 9u);
    EXPECT_EQ((*output)[0].sub_id, 4u);
    EXPECT_EQ((*output)[1].data, "hello");
    EXPECT_EQ((*output)[1].req_id, 3u);
  }
  EXPECT_EQ(model->language_checks, 1);
  EXPECT_EQ(model->languages, (std::vector<std::string>{"en", "en"}));
  ASSERT_EQ(model->calls.size(), 2u);
  EXPECT_EQ(model->calls[0][0].data.sample_rate, 32000);
  EXPECT_EQ(model->calls[0][0].data.pcm_data,
            (std::vector<float>{0.5f, -0.25f}));
  EXPECT_EQ(model->calls[1][1].sub_id, 7u);
}

TEST(AsrTranscribeNodeTest,
     UnsupportedLanguageIsRejectedBeforeAnyTranscription) {
  SessionContext session;
  auto model = std::make_shared<RecordingAsr>();
  model->supports_language = false;
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "speech", model));
  test_support::NodeProbe node;
  EXPECT_FALSE(node.Init(
      "asr_transcribe", {{"bind_model", "speech"}, {"language", "fr"}}, session,
      {{"audio", "audio", ""}}, {{"text", "text", ""}}));
  EXPECT_EQ(model->language_checks, 1);
  EXPECT_EQ(model->checked_language, "fr");
  EXPECT_TRUE(model->calls.empty());
  EXPECT_NE(node.diagnostic.find("fr"), std::string::npos);
}
}  // namespace
}  // namespace llm_edgeflow
