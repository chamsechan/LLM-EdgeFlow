#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/backend_registry.h"
#include "engine/backends/whisper_cpp/whisper_cpp_backend.h"

namespace llm_edgeflow {
namespace {

TEST(WhisperCppBackendTest, ProtocolAndCpuTargetAreCheckedBeforeLoading) {
  WhisperCppBackend provider;
  EXPECT_EQ(provider.BackendType(), "whisper_cpp");
  BackendLoadSpec spec(ExecutionProtocol::kTextGeneration);
  std::string diagnostic;
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(
      diagnostic.find("only supports requested protocol audio_transcription"),
      std::string::npos);
  spec.requested_protocol = ExecutionProtocol::kAudioTranscription;
  spec.execution_target = {0, "CUDA"};
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("only supports CPU"), std::string::npos);
  spec.execution_target = {1, "cpu"};
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("device_id 0"), std::string::npos);
  spec.execution_target = {0, "cpu"};
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_EQ(diagnostic, "whisper_cpp model_file is empty");
}

#ifdef HAVE_WHISPERCPP
BackendLoadSpec WhisperSpec(const std::string& file) {
  BackendLoadSpec spec(ExecutionProtocol::kAudioTranscription);
  auto definition = BackendRegistry::Instance().Find("whisper_cpp");
  EXPECT_TRUE(definition);
  std::string diagnostic;
  if (definition)
    EXPECT_TRUE(
        definition->params.Parse({{"n_threads", 2}}, &spec.params, &diagnostic))
        << diagnostic;
  spec.model_file = file;
  spec.execution_target = {0, "CPU"};
  return spec;
}

TEST(WhisperCppBackendTest,
     EnabledRegistryAndVendorLoadFailurePublishNoSession) {
  const auto definition = BackendRegistry::Instance().Find("whisper_cpp");
  ASSERT_TRUE(definition);
  EXPECT_EQ(
      definition->supported_protocols,
      (std::vector<ExecutionProtocol>{ExecutionProtocol::kAudioTranscription}));
  EXPECT_EQ(definition->concurrency, InferenceConcurrency::kSerialized);
  auto provider = BackendRegistry::Instance().Create("whisper_cpp");
  ASSERT_NE(provider, nullptr);
  char name[] = "/tmp/edgeflow-invalid-whisper-XXXXXX";
  const int fd = mkstemp(name);
  ASSERT_GE(fd, 0);
  close(fd);
  struct FileGuard {
    const char* file;
    ~FileGuard() {
      std::error_code error;
      std::filesystem::remove(file, error);
    }
  } guard{name};
  std::ofstream(name, std::ios::binary) << "not a whisper model";
  std::string diagnostic;
  auto spec = WhisperSpec(std::string(name) + ".missing");
  EXPECT_EQ(provider->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("not regular file"), std::string::npos);
  spec.model_file = name;
  for (int attempt = 0; attempt < 2; ++attempt) {
    EXPECT_EQ(provider->Load(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("Failed to load whisper model"),
              std::string::npos);
  }
}

TEST(WhisperCppBackendTest, SuppliedModelRunsThroughNeutralAudioProtocol) {
  const char* file = std::getenv("EDGEFLOW_TEST_WHISPER_MODEL");
  if (!file || !*file) {
    RecordProperty("optional_runtime_asset", "EDGEFLOW_TEST_WHISPER_MODEL");
    GTEST_SKIP() << "EDGEFLOW_TEST_WHISPER_MODEL is not supplied";
  }
  WhisperCppBackend provider;
  std::string diagnostic;
  auto session = std::dynamic_pointer_cast<IAudioTranscriptionSession>(
      provider.Load(WhisperSpec(file), &diagnostic));
  ASSERT_NE(session, nullptr) << diagnostic;
  EXPECT_EQ(session->BackendType(), "whisper_cpp");
  EXPECT_EQ(session->Protocol(), ExecutionProtocol::kAudioTranscription);
  EXPECT_EQ(session->Concurrency(), InferenceConcurrency::kSerialized);
  EXPECT_EQ(session->GetBatchPolicy().max_batch_size, 1u);
  EXPECT_EQ(session->GetBatchPolicy().fixed_batch_size, 0u);
  EXPECT_TRUE(session->SupportsLanguage("en"));
  EXPECT_TRUE(session->SupportsLanguage("auto"));
  EXPECT_FALSE(session->SupportsLanguage("not-a-language"));
  AudioTranscriptionOptions options;
  options.language = "en";
  std::string output = "previous";
  EXPECT_EQ(session->Transcribe(AudioPcmPayload({}, 16000), options, &output,
                                &diagnostic),
            0);
  EXPECT_TRUE(output.empty());
  EXPECT_EQ(session->Transcribe(AudioPcmPayload({}, 8000), options, &output,
                                &diagnostic),
            -1);
  EXPECT_NE(diagnostic.find("16000 Hz"), std::string::npos);
  EXPECT_EQ(session->Transcribe(AudioPcmPayload(std::vector<float>(1600, 0.0f)),
                                options, &output, &diagnostic),
            0)
      << diagnostic;
  // Silence is a runtime smoke input; it makes no claim about recognition
  // accuracy.
  EXPECT_LE(output.size(), options.max_output_bytes);
  EXPECT_EQ(session->Transcribe(AudioPcmPayload({}, 16000), options, nullptr,
                                &diagnostic),
            -1);
}
#else
TEST(WhisperCppBackendTest,
     DisabledBackendHasNoRegistrationOrCreatableProvider) {
  EXPECT_FALSE(BackendRegistry::Instance().Find("whisper_cpp"));
  EXPECT_EQ(BackendRegistry::Instance().Create("whisper_cpp"), nullptr);
  char name[] = "/tmp/edgeflow-disabled-whisper-XXXXXX";
  const int fd = mkstemp(name);
  ASSERT_GE(fd, 0);
  close(fd);
  struct FileGuard {
    const char* file;
    ~FileGuard() {
      std::error_code error;
      std::filesystem::remove(file, error);
    }
  } guard{name};
  WhisperCppBackend provider;
  BackendLoadSpec spec(ExecutionProtocol::kAudioTranscription);
  spec.model_file = name;
  std::string diagnostic;
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_EQ(diagnostic, "whisper_cpp backend is not enabled in this build");
}
#endif

}  // namespace
}  // namespace llm_edgeflow
