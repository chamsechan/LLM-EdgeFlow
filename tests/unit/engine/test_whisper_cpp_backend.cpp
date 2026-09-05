#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "engine/backend_registry.h"
#include "engine/backends/whisper_cpp/whisper_cpp_backend.h"

namespace llm_edgeflow {
namespace {

TEST(WhisperCppBackendTest, RegistryAndDefinitionAreConsistentWithBuild) {
  const auto definition = BackendRegistry::Instance().Find("whisper_cpp");
  auto backend = BackendRegistry::Instance().Create("whisper_cpp");
  EXPECT_EQ(definition.has_value(), backend != nullptr);
  if (!definition) return;

  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(definition->backend_type, "whisper_cpp");
  EXPECT_EQ(
      definition->supported_protocols,
      std::vector<ExecutionProtocol>({ExecutionProtocol::kAudioTranscription}));
  EXPECT_EQ(definition->concurrency, InferenceConcurrency::kSerialized);
  ASSERT_EQ(definition->config_fields.size(), 1U);
  EXPECT_EQ(definition->config_fields[0].name, "n_threads");
  EXPECT_EQ(definition->config_fields[0].kind, ConfigValueKind::kInteger);
}

TEST(WhisperCppBackendTest, MissingInvalidPathAndUnknownConfigFailClosed) {
  WhisperCppBackend backend;

  std::string diagnostic;
  BackendLoadSpec missing;
  missing.model_path = "./models/does-not-exist.bin";
  EXPECT_EQ(backend.Load(missing, &diagnostic), nullptr);
  EXPECT_FALSE(diagnostic.empty());

  diagnostic.clear();
  BackendLoadSpec directory;
  directory.model_path = ".";
  EXPECT_EQ(backend.Load(directory, &diagnostic), nullptr);
  EXPECT_FALSE(diagnostic.empty());

  diagnostic.clear();
  BackendLoadSpec unknown;
  unknown.model_path = "./models/does-not-exist.bin";
  unknown.backend_config = {{"unknown_field", 123}};
  EXPECT_EQ(backend.Load(unknown, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("Unknown"), std::string::npos);

  diagnostic.clear();
  BackendLoadSpec invalid_threads;
  invalid_threads.model_path = "./models/does-not-exist.bin";
  invalid_threads.backend_config = {{"n_threads", 0}};
  EXPECT_EQ(backend.Load(invalid_threads, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("n_threads"), std::string::npos);

  diagnostic.clear();
  BackendLoadSpec wrong_protocol;
  wrong_protocol.model_path = "./models/does-not-exist.bin";
  wrong_protocol.requested_protocol = ExecutionProtocol::kTextGeneration;
  EXPECT_EQ(backend.Load(wrong_protocol, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("requested protocol"), std::string::npos);
}

TEST(WhisperCppBackendTest, UnsupportedExecutionTargetFailsBeforeFilesystem) {
  WhisperCppBackend backend;

  BackendLoadSpec spec;
  spec.model_path = "./models/does-not-exist.bin";
  spec.execution_target.platform = "NPU";
  spec.execution_target.device_id = 0;
  std::string diagnostic;
  EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("CPU"), std::string::npos);

  diagnostic.clear();
  spec.execution_target.platform = "CPU";
  spec.execution_target.device_id = 1;
  EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("device_id"), std::string::npos);
}

TEST(WhisperCppBackendTest, LoadExceptionBarrierProtectsEntireEntrypoint) {
  WhisperCppBackend backend;

  // Set a terminate handler to verify std::terminate is never called
  // (reproducing the exit code 86 scenario)
  static bool terminate_invoked = false;
  terminate_invoked = false;
  auto old_terminate = std::set_terminate([] {
    terminate_invoked = true;
    std::_Exit(86);
  });

  struct TerminateHandlerGuard {
    std::terminate_handler old_handler;
    ~TerminateHandlerGuard() { std::set_terminate(old_handler); }
  } guard{old_terminate};

  BackendLoadSpec spec;
  spec.model_path = "./models/does-not-exist.bin";

  // 1. bad_alloc exception at entrypoint (reproduces allocation failure during
  // NormalizePlatform / string copies)
  {
    backend.SetLoadHook([] { throw std::bad_alloc(); });
    std::string diagnostic;
    EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_NE(diagnostic.find("Exception loading whisper model"),
              std::string::npos);
    EXPECT_FALSE(terminate_invoked);
  }

  // 2. bad_alloc with nullptr diagnostic
  {
    backend.SetLoadHook([] { throw std::bad_alloc(); });
    EXPECT_EQ(backend.Load(spec, nullptr), nullptr);
    EXPECT_FALSE(terminate_invoked);
  }

  // 3. standard runtime_error at entrypoint
  {
    backend.SetLoadHook(
        [] { throw std::runtime_error("simulated entrypoint failure"); });
    std::string diagnostic;
    EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("simulated entrypoint failure"),
              std::string::npos);
    EXPECT_FALSE(terminate_invoked);
  }

  // 4. non-std exception at entrypoint
  {
    backend.SetLoadHook([] { throw 42; });
    std::string diagnostic;
    EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("Unknown exception loading whisper model"),
              std::string::npos);
    EXPECT_FALSE(terminate_invoked);
  }

  // 5. Clean hook reset allows normal failure handling
  backend.SetLoadHook(nullptr);
  std::string diagnostic;
  EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("whisper_cpp model file not found"),
            std::string::npos);
  EXPECT_FALSE(terminate_invoked);
}

#ifdef HAVE_WHISPERCPP
std::string FindWhisperModelPath() {
  const std::vector<std::string> candidates = {
      "models/ggml-base.bin",
      "models/ggml-tiny-q5_1.bin",
      "/tmp/edgeflow-whisper-review/ggml-tiny-q5_1.bin",
  };
  for (const auto& path : candidates) {
    if (std::filesystem::is_regular_file(path)) {
      return path;
    }
  }
  return "";
}

TEST(WhisperCppBackendTest, SessionLifecycleAndInference) {
  const std::string model_path = FindWhisperModelPath();
  if (model_path.empty()) {
    GTEST_SKIP() << "No whisper model file found for testing";
  }

  WhisperCppBackend backend;
  BackendLoadSpec spec;
  spec.model_path = model_path;
  spec.backend_config = {{"n_threads", 2}};
  spec.requested_protocol = ExecutionProtocol::kAudioTranscription;

  std::string diagnostic;
  auto session = backend.Load(spec, &diagnostic);
  ASSERT_NE(session, nullptr) << diagnostic;

  EXPECT_EQ(session->BackendType(), "whisper_cpp");
  EXPECT_EQ(session->Protocol(), ExecutionProtocol::kAudioTranscription);
  EXPECT_EQ(session->Concurrency(), InferenceConcurrency::kSerialized);
  EXPECT_EQ(session->GetBatchPolicy().max_batch_size, 1U);
  EXPECT_EQ(session->GetBatchPolicy().fixed_batch_size, 0U);

  auto asr_session =
      std::dynamic_pointer_cast<IAudioTranscriptionSession>(session);
  ASSERT_NE(asr_session, nullptr);

  EXPECT_TRUE(asr_session->SupportsLanguage("en"));
  EXPECT_TRUE(asr_session->SupportsLanguage("auto"));
  EXPECT_FALSE(asr_session->SupportsLanguage("unknown_lang"));

  // 1. Empty audio returns empty string
  AudioPcmPayload empty_audio;
  empty_audio.sample_rate = 16000;
  std::string output;
  diagnostic.clear();
  AudioTranscriptionOptions opts;
  opts.language = "auto";
  EXPECT_EQ(asr_session->Transcribe(empty_audio, opts, &output, &diagnostic),
            0);
  EXPECT_TRUE(output.empty());

  // 2. Audio with wrong sample rate fails
  AudioPcmPayload wrong_rate;
  wrong_rate.sample_rate = 8000;
  wrong_rate.pcm_data = std::vector<float>(16000, 0.01f);
  EXPECT_NE(asr_session->Transcribe(wrong_rate, opts, &output, &diagnostic), 0);

  // 3. Audio too short (< 1600 samples) fails
  AudioPcmPayload short_audio;
  short_audio.sample_rate = 16000;
  short_audio.pcm_data = std::vector<float>(1500, 0.01f);
  EXPECT_NE(asr_session->Transcribe(short_audio, opts, &output, &diagnostic),
            0);

  // 4. Null output pointer fails
  AudioPcmPayload valid_audio;
  valid_audio.sample_rate = 16000;
  valid_audio.pcm_data = std::vector<float>(16000, 0.01f);
  EXPECT_NE(asr_session->Transcribe(valid_audio, opts, nullptr, &diagnostic),
            0);

  // 5. Valid audio transcription
  const std::string audio_path = "data/audio/nav_001.f32";
  if (std::filesystem::is_regular_file(audio_path)) {
    std::ifstream ifs(audio_path, std::ios::binary);
    ASSERT_TRUE(ifs.is_open());
    const auto sz = std::filesystem::file_size(audio_path);
    std::vector<float> pcm(sz / sizeof(float));
    ifs.read(reinterpret_cast<char*>(pcm.data()), sz);

    AudioPcmPayload jfk_audio;
    jfk_audio.sample_rate = 16000;
    jfk_audio.pcm_data = std::move(pcm);

    opts.language = "en";
    EXPECT_EQ(asr_session->Transcribe(jfk_audio, opts, &output, &diagnostic),
              0);
    EXPECT_FALSE(output.empty());

    // 6. Max output bytes boundary test
    opts.max_output_bytes = 2;
    output.clear();
    EXPECT_NE(asr_session->Transcribe(jfk_audio, opts, &output, &diagnostic),
              0);
    EXPECT_TRUE(output.empty());

    // 7. Concurrent calls on same session (serialized by session mutex)
    opts.max_output_bytes = 65536;
    auto f1 = std::async(std::launch::async, [&]() {
      std::string out1, diag1;
      return asr_session->Transcribe(jfk_audio, opts, &out1, &diag1);
    });
    auto f2 = std::async(std::launch::async, [&]() {
      std::string out2, diag2;
      return asr_session->Transcribe(jfk_audio, opts, &out2, &diag2);
    });
    EXPECT_EQ(f1.get(), 0);
    EXPECT_EQ(f2.get(), 0);
  }
}
#endif

}  // namespace
}  // namespace llm_edgeflow
