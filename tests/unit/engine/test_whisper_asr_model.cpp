#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/models/whisper_asr/whisper_asr_model.h"
#include "tests/support/model_probe.h"

namespace llm_edgeflow {
namespace {

class AudioSession final : public IAudioTranscriptionSession {
 public:
  const std::string& BackendType() const noexcept override { return backend; }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kAudioTranscription;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return {1, 0}; }
  bool SupportsLanguage(std::string_view language) const noexcept override {
    return language == supported_language;
  }
  int Transcribe(const AudioPcmPayload& audio,
                 const AudioTranscriptionOptions& options, std::string* output,
                 std::string* diagnostic) noexcept override {
    ++calls;
    observed_audio = audio;
    observed_options = options;
    *output = response;
    *diagnostic = reason;
    return code;
  }

  const std::string backend = "neutral_audio_probe";
  std::string supported_language = "en";
  int calls = 0;
  int code = 0;
  std::string reason;
  std::string response = " \t你好\r\n";
  AudioPcmPayload observed_audio;
  AudioTranscriptionOptions observed_options;
};

TEST(WhisperAsrModelTest, OriginalPcmLanguageAndByteLimitReachSession) {
  auto session = std::make_shared<AudioSession>();
  std::string diagnostic;
  auto model = test_support::CreateModel<WhisperAsrModel>(
      session, {{"max_audio_seconds", 1}, {"max_output_bytes", 6}},
      &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  const std::vector<float> samples(1600, 0.25f);
  TextBatch output;
  ASSERT_EQ(model->Transcribe({{3, 8, {samples, 16000}}}, {"en"}, &output,
                              &diagnostic),
            0)
      << diagnostic;
  EXPECT_EQ(session->observed_audio.pcm_data, samples);
  EXPECT_EQ(session->observed_audio.sample_rate, 16000);
  EXPECT_EQ(session->observed_options.language, "en");
  EXPECT_EQ(session->observed_options.max_output_bytes, 6U);
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(output[0].data, "你好");
  session->response = "changed";
  EXPECT_EQ(output[0].data, "你好");
  EXPECT_TRUE(diagnostic.empty());
  EXPECT_TRUE(model->SupportsLanguage("en"));
  EXPECT_FALSE(model->SupportsLanguage("zh"));
  session->supported_language = "zh";
  EXPECT_TRUE(model->SupportsLanguage("zh"));
  EXPECT_FALSE(model->SupportsLanguage("en"));
}

TEST(WhisperAsrModelTest, AudioValidationCoversDurationRateAndFiniteAmplitude) {
  struct Case {
    const char* name;
    size_t samples;
    int rate;
    float amplitude;
    const char* detail;
  };
  const std::vector<Case> cases = {
      {"shorter than 100ms", 1599, 16000, 0, "at least 1600 samples"},
      {"beyond configured second", 16001, 16000, 0, "duration limit"},
      {"wrong sample rate", 1600, 8000, 0, "16000 Hz"},
      {"wrong rate even when empty", 0, 8000, 0, "16000 Hz"},
      {"above one", 1600, 16000, 1.25f, "within [-1, 1]"},
      {"below minus one", 1600, 16000, -1.25f, "within [-1, 1]"},
      {"NaN", 1600, 16000, std::numeric_limits<float>::quiet_NaN(), "finite"},
      {"infinity", 1600, 16000, std::numeric_limits<float>::infinity(),
       "finite"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto session = std::make_shared<AudioSession>();
    std::string diagnostic;
    auto model = test_support::CreateModel<WhisperAsrModel>(
        session, {{"max_audio_seconds", 1}}, &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    AudioPcmPayload audio(std::vector<float>(test.samples, test.amplitude),
                          test.rate);
    TextBatch output;
    EXPECT_EQ(model->Transcribe({{0, 0, audio}}, {}, &output, &diagnostic), -1);
    EXPECT_EQ(session->calls, 0);
    EXPECT_TRUE(output.empty());
    EXPECT_NE(diagnostic.find(test.detail), std::string::npos) << diagnostic;
  }
}

TEST(WhisperAsrModelTest, EntireBatchIsValidatedBeforeFirstInference) {
  auto session = std::make_shared<AudioSession>();
  std::string diagnostic;
  auto model = test_support::CreateModel<WhisperAsrModel>(
      session, nlohmann::json::object(), &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  AudioPcmBatch audio = {
      {0, 0, {std::vector<float>(1600, 0.25f), 16000}},
      {1, 0, {std::vector<float>(1600, 0.25f), 16000}},
  };
  audio[1].data.pcm_data.back() = 1.25f;
  TextBatch output;
  EXPECT_EQ(model->Transcribe(audio, {}, &output, &diagnostic), -1);
  EXPECT_EQ(session->calls, 0);
  EXPECT_NE(diagnostic.find("within [-1, 1]"), std::string::npos);
}

TEST(WhisperAsrModelTest, InclusiveAmplitudeAndDurationBoundsAndEmptyAudio) {
  auto session = std::make_shared<AudioSession>();
  session->response = "ok";
  std::string diagnostic;
  auto model = test_support::CreateModel<WhisperAsrModel>(
      session, {{"max_audio_seconds", 1}}, &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  for (const auto& samples :
       {std::vector<float>(1600, -1), std::vector<float>(16000, 1)}) {
    TextBatch output;
    ASSERT_EQ(
        model->Transcribe({{0, 0, {samples, 16000}}}, {}, &output, &diagnostic),
        0)
        << diagnostic;
    ASSERT_EQ(output.size(), 1U);
    EXPECT_EQ(output[0].data, "ok");
  }
  EXPECT_EQ(session->calls, 2);
  TextBatch output;
  ASSERT_EQ(model->Transcribe({{0, 0, {{}, 16000}}}, {}, &output, &diagnostic),
            0);
  ASSERT_EQ(output.size(), 1U);
  EXPECT_TRUE(output[0].data.empty());
  EXPECT_EQ(session->calls, 2);
}

TEST(WhisperAsrModelTest,
     StrictTextValidationUsesTrimmedUtf8BytesWithoutTruncation) {
  struct Case {
    const char* name;
    std::string response;
    int limit;
    int code;
    const char* expected;
  };
  const std::vector<Case> cases = {
      {"six byte text exceeds five", " \t你好\r\n", 5, -1,
       "Transcription exceeds max_output_bytes"},
      {"incomplete UTF8", std::string("A\xe4\xb8", 3), 6, -1,
       "Transcription is not valid UTF-8"},
      {"embedded NUL", std::string("a\0b", 3), 6, -1,
       "Transcription contains embedded NUL byte"},
      {"ASCII whitespace only", " \t\r\n", 1, 0, ""},
      {"Unicode whitespace retained", "　", 3, 0, "　"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto session = std::make_shared<AudioSession>();
    session->response = test.response;
    std::string diagnostic;
    auto model = test_support::CreateModel<WhisperAsrModel>(
        session, {{"max_output_bytes", test.limit}}, &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    TextBatch output;
    EXPECT_EQ(model->Transcribe({{0, 0, {std::vector<float>(1600, 0), 16000}}},
                                {}, &output, &diagnostic),
              test.code);
    if (test.code == 0) {
      ASSERT_EQ(output.size(), 1U);
      EXPECT_EQ(output[0].data, test.expected);
      EXPECT_TRUE(diagnostic.empty());
    } else {
      EXPECT_EQ(diagnostic, test.expected);
      EXPECT_TRUE(output.empty());
    }
  }
}

TEST(WhisperAsrModelTest, SessionFailureKeepsDetailAndReturnsModelFailureCode) {
  auto session = std::make_shared<AudioSession>();
  session->code = -719;
  session->reason = "acoustic decoder failed";
  std::string diagnostic;
  auto model = test_support::CreateModel<WhisperAsrModel>(
      session, nlohmann::json::object(), &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  TextBatch output;
  EXPECT_EQ(model->Transcribe({{0, 0, {std::vector<float>(1600, 0), 16000}}},
                              {}, &output, &diagnostic),
            -1);
  EXPECT_EQ(diagnostic, "acoustic decoder failed");
  EXPECT_TRUE(output.empty());
}

}  // namespace
}  // namespace llm_edgeflow
