#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/backend_registry.h"
#include "engine/backends/kite_llm/kite_llm_backend.h"
#include "engine/models/qwen_causal_lm/qwen_causal_lm_model.h"

namespace llm_edgeflow {
namespace {

class KiteTestDirectory final {
 public:
  KiteTestDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("edgeflow-kite-test-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(path)) {
      throw std::runtime_error("Cannot create kiteLLM test directory");
    }
  }
  ~KiteTestDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

TEST(LlamaCppBackendTest, RegistryAndDefinitionAreConsistentWithBuild) {
  const auto definition = BackendRegistry::Instance().Find("llama_cpp");
  auto backend = BackendRegistry::Instance().Create("llama_cpp");
  EXPECT_EQ(definition.has_value(), backend != nullptr);
  if (!definition) return;

  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(definition->backend_type, "llama_cpp");
  EXPECT_EQ(
      definition->supported_protocols,
      std::vector<ExecutionProtocol>({ExecutionProtocol::kTextGeneration}));
  EXPECT_EQ(definition->concurrency, InferenceConcurrency::kSerialized);
  ASSERT_EQ(definition->config_fields.size(), 6U);
  EXPECT_EQ(definition->config_fields[0].name, "context_size");
  EXPECT_EQ(definition->config_fields[1].name, "decode_batch_size");
  EXPECT_EQ(definition->config_fields[2].name, "n_threads");
  EXPECT_EQ(definition->config_fields[3].name, "n_threads_batch");
  EXPECT_EQ(definition->config_fields[4].name, "n_gpu_layers");
  EXPECT_EQ(definition->config_fields[5].name, "check_tensors");
}

TEST(LlamaCppBackendTest, MissingInvalidPathAndUnknownConfigFailClosed) {
  auto backend = BackendRegistry::Instance().Create("llama_cpp");
  if (!backend) GTEST_SKIP() << "llama.cpp support is disabled";

  std::string diagnostic;
  BackendLoadSpec missing;
  missing.model_path = "./models/does-not-exist.gguf";
  EXPECT_EQ(backend->Load(missing, &diagnostic), nullptr);
  EXPECT_FALSE(diagnostic.empty());

  diagnostic.clear();
  BackendLoadSpec directory;
  directory.model_path = ".";
  EXPECT_EQ(backend->Load(directory, &diagnostic), nullptr);
  EXPECT_FALSE(diagnostic.empty());

  diagnostic.clear();
  BackendLoadSpec unknown;
  unknown.model_path = "./models/does-not-exist.gguf";
  unknown.backend_config = {{"business_answer", true}};
  EXPECT_EQ(backend->Load(unknown, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("Unknown"), std::string::npos);

  diagnostic.clear();
  BackendLoadSpec wrong_protocol;
  wrong_protocol.model_path = "./models/does-not-exist.gguf";
  wrong_protocol.requested_protocol = ExecutionProtocol::kTensorGraph;
  EXPECT_EQ(backend->Load(wrong_protocol, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("requested protocol"), std::string::npos);
}

TEST(LlamaCppBackendTest, UnsupportedExecutionTargetFailsBeforeFilesystem) {
  auto backend = BackendRegistry::Instance().Create("llama_cpp");
  if (!backend) GTEST_SKIP() << "llama.cpp support is disabled";

  BackendLoadSpec spec;
  spec.model_path = "./models/does-not-exist.gguf";
  spec.execution_target.platform = "AX650";
  spec.execution_target.device_id = 7;
  std::string diagnostic;
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("does not support requested platform"),
            std::string::npos);
}

TEST(LlamaCppBackendTest, PublicHeaderDoesNotExposeVendorTypes) {
  const auto read = [](const std::string& path) {
    std::ifstream header(path);
    EXPECT_TRUE(header.is_open()) << path;
    return std::string((std::istreambuf_iterator<char>(header)),
                       std::istreambuf_iterator<char>());
  };
  const std::string llama =
      read("src/engine/backends/llama_cpp/llama_cpp_backend.h");
  EXPECT_EQ(llama.find("llama.h"), std::string::npos);
  EXPECT_EQ(llama.find("llama_model"), std::string::npos);
  EXPECT_EQ(llama.find("llama_context"), std::string::npos);

  const std::string onnx =
      read("src/engine/backends/onnxruntime/onnxruntime_backend.h");
  EXPECT_EQ(onnx.find("Ort::"), std::string::npos);
  EXPECT_EQ(onnx.find("#include <onnxruntime_cxx_api.h>"), std::string::npos);
  EXPECT_EQ(onnx.find("#include \"onnxruntime_cxx_api.h\""), std::string::npos);

  const std::string kite =
      read("src/engine/backends/kite_llm/kite_llm_backend.h");
  EXPECT_EQ(kite.find("kitellm_edgeflow"), std::string::npos);
  EXPECT_EQ(kite.find("kiteLLM.h"), std::string::npos);
}

TEST(LlamaCppBackendTest, KiteRegistrationMatchesConditionalBuild) {
  const auto definition = BackendRegistry::Instance().Find("kite_llm");
  auto registered = BackendRegistry::Instance().Create("kite_llm");
  EXPECT_EQ(definition.has_value(), registered != nullptr);
  if (definition.has_value()) {
    EXPECT_EQ(definition->supported_protocols,
              std::vector<ExecutionProtocol>(
                  {ExecutionProtocol::kTextGeneration,
                   ExecutionProtocol::kImageTextGeneration,
                   ExecutionProtocol::kGeneratedTokenEmbedding}));
    EXPECT_EQ(definition->concurrency, InferenceConcurrency::kSerialized);
    ASSERT_EQ(definition->config_fields.size(), 1U);
    EXPECT_EQ(definition->config_fields[0].name, "run_config_file");
    return;
  }

  KiteLlmBackend unavailable;
  BackendLoadSpec spec;
  spec.model_path = "./models/does-not-exist";
  spec.requested_protocol = ExecutionProtocol::kTextGeneration;
  std::string diagnostic;
  EXPECT_EQ(unavailable.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("not compiled"), std::string::npos);
}

TEST(LlamaCppBackendTest, KiteRejectsUnsupportedExecutionTargets) {
  KiteLlmBackend backend;
  BackendLoadSpec spec;
  spec.model_path = "./models/does-not-exist";
  spec.requested_protocol = ExecutionProtocol::kTextGeneration;
  std::string diagnostic;
  for (const char* platform : {"CUDA", "ASCEND_310P", "AX650", "invalid"}) {
    spec.execution_target.platform = platform;
    EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("requested platform"), std::string::npos);
  }
  spec.execution_target.platform = "CPU";
  spec.execution_target.device_id = 1;
  EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("only accepts device_id 0"), std::string::npos);
  spec.execution_target.platform.clear();
  spec.execution_target.device_id = -2;
  EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("device_id must be >= -1"), std::string::npos);
}

TEST(LlamaCppBackendTest, KiteAcceptsCpuAndUnspecifiedExecutionTargets) {
  KiteLlmBackend backend;
  BackendLoadSpec spec;
  spec.model_path = "./models/does-not-exist";
  std::string diagnostic;
  for (const char* platform : {"", "UNKNOWN", "CPU", "cpu_generic"}) {
    spec.execution_target.platform = platform;
    for (const auto device : {std::optional<int>{}, std::optional<int>{-1},
                              std::optional<int>{0}}) {
      spec.execution_target.device_id = device;
      EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
      // A valid target reaches model validation (or the unavailable SDK).
      EXPECT_TRUE(diagnostic.find("regular file") != std::string::npos ||
                  diagnostic.find("not compiled") != std::string::npos)
          << diagnostic;
    }
  }
}

TEST(LlamaCppBackendTest, KiteRejectsInvalidModelsAndRunConfigs) {
  auto backend = BackendRegistry::Instance().Create("kite_llm");
  if (!backend) GTEST_SKIP() << "kiteLLM SDK support is disabled";
  KiteTestDirectory temporary;
  const auto model = temporary.path / "invalid.gguf";
  std::ofstream(model) << "not a GGUF model";
  BackendLoadSpec spec;
  spec.model_path = model.string();
  std::string diagnostic;
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("model load failed"), std::string::npos);
  spec.execution_target.platform = "cpu_generic";
  spec.execution_target.device_id = 0;
  std::ofstream(temporary.path / "gpu.json")
      << R"({"schema_version":1,"model":{"gpu_layers":1}})";
  spec.backend_config = {{"run_config_file", "gpu.json"}};
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("conflicts"), std::string::npos);
  // A CPU-compatible file reaches the native model loader.
  std::ofstream(temporary.path / "cpu.json")
      << R"({"schema_version":1,"model":{"gpu_layers":0}})";
  spec.backend_config = {{"run_config_file", "cpu.json"}};
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("model load failed"), std::string::npos);
  spec.backend_config = {{"unknown", true}};
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("Unknown"), std::string::npos);
  spec.backend_config = {{"run_config_file", "../outside.json"}};
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("traverse"), std::string::npos);
  spec.backend_config = {{"run_config_file", "/outside.json"}};
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("relative"), std::string::npos);
  spec.backend_config = {{"run_config_file", "missing.json"}};
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("does not exist"), std::string::npos);
  std::ofstream(temporary.path / "invalid.json") << "invalid JSON";
  spec.backend_config = {{"run_config_file", "invalid.json"}};
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("model load failed"), std::string::npos);
  spec.model_path = temporary.path.string();
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("regular file"), std::string::npos);
}

TEST(LlamaCppBackendTest, RealKiteSdkGenerationAndFixedSeedPolicy) {
  auto backend = BackendRegistry::Instance().Create("kite_llm");
  if (!backend) GTEST_SKIP() << "kiteLLM SDK support is disabled";
  const char* model_path = std::getenv("LLM_EDGEFLOW_TEST_KITELLM_MODEL");
  if (!model_path || !*model_path) {
    GTEST_SKIP() << "Set LLM_EDGEFLOW_TEST_KITELLM_MODEL for the real SDK gate";
  }

  KiteTestDirectory temporary;
  std::filesystem::create_symlink(std::filesystem::absolute(model_path),
                                  temporary.path / "model.gguf");
  std::ofstream(temporary.path / "run.json")
      << R"({"schema_version":1,"model":{"context_size":256,"threads":2,"threads_batch":2},"logging":{"level":"error"}})";
  BackendLoadSpec spec;
  spec.model_path = (temporary.path / "model.gguf").string();
  spec.backend_config = {{"run_config_file", "run.json"}};
  spec.requested_protocol = ExecutionProtocol::kTextGeneration;
  std::string diagnostic;
  auto base = backend->Load(spec, &diagnostic);
  ASSERT_NE(base, nullptr) << diagnostic;
  auto session = std::dynamic_pointer_cast<ITextGenerationSession>(base);
  ASSERT_NE(session, nullptr);

  GenerateOptions options;
  options.max_tokens = 8;
  options.temperature = 0.0f;
  options.top_k = 0;
  options.top_p = 1.0f;
  std::string output;
  EXPECT_NE(session->Generate("Reply with OK.", false, options, 17, &output,
                              &diagnostic),
            0);
  EXPECT_NE(diagnostic.find("fixed random seed"), std::string::npos);
  EXPECT_NE(
      session->Generate("", false, options, std::nullopt, &output, &diagnostic),
      0);
  EXPECT_NE(session->Generate("hello", false, options, std::nullopt, nullptr,
                              &diagnostic),
            0);
  auto invalid = options;
  invalid.max_tokens = 0;
  EXPECT_NE(session->Generate("hello", false, invalid, std::nullopt, &output,
                              &diagnostic),
            0);

  diagnostic.clear();
  const std::string prompt =
      "<|im_start|>user\nReply with OK.<|im_end|>\n<|im_start|>assistant\n";
  ASSERT_EQ(session->Generate(prompt, false, options, std::nullopt, &output,
                              &diagnostic),
            0)
      << diagnostic;
  ASSERT_FALSE(output.empty());
  const std::string baseline = output;
  options.stop_words = {baseline};
  ASSERT_EQ(session->Generate(prompt, false, options, std::nullopt, &output,
                              &diagnostic),
            0)
      << diagnostic;
  EXPECT_TRUE(output.empty());
  options.stop_words.clear();
  auto generate = [&]() {
    std::string result, error;
    EXPECT_EQ(session->Generate(prompt, false, options, std::nullopt, &result,
                                &error),
              0)
        << error;
    return result;
  };
  auto first = std::async(std::launch::async, generate);
  auto second = std::async(std::launch::async, generate);
  EXPECT_EQ(first.get(), baseline);
  EXPECT_EQ(second.get(), baseline);

  // Releasing one session must not deinitialize a second live model handle.
  auto another = std::dynamic_pointer_cast<ITextGenerationSession>(
      backend->Load(spec, &diagnostic));
  ASSERT_NE(another, nullptr) << diagnostic;
  base.reset();
  session.reset();
  EXPECT_EQ(another->Generate(prompt, true, options, std::nullopt, &output,
                              &diagnostic),
            0)
      << diagnostic;
  EXPECT_FALSE(output.empty());
  another.reset();
  EXPECT_NE(backend->Load(spec, &diagnostic), nullptr) << diagnostic;

  // Exercise the native setter, both with and without an optional run-config.
  // Release each session before changing native load parameters for this model.
  for (const bool with_config : {true, false}) {
    if (!with_config) spec.backend_config = nlohmann::json::object();
    spec.execution_target.platform = "CPU";
    spec.execution_target.device_id = 0;
    auto explicit_cpu = std::dynamic_pointer_cast<ITextGenerationSession>(
        backend->Load(spec, &diagnostic));
    ASSERT_NE(explicit_cpu, nullptr) << diagnostic;
    ASSERT_EQ(explicit_cpu->Generate(prompt, false, options, std::nullopt,
                                     &output, &diagnostic),
              0)
        << diagnostic;
    EXPECT_FALSE(output.empty());
  }
  spec.execution_target.platform.clear();
  spec.execution_target.device_id = -1;
  EXPECT_NE(backend->Load(spec, &diagnostic), nullptr) << diagnostic;
  // A nonexistent native index must fail instead of silently using the CPU.
  spec.execution_target.device_id = std::numeric_limits<int>::max();
  EXPECT_EQ(backend->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("model load failed"), std::string::npos);
}

TEST(LlamaCppBackendTest, RealGgufLoadAndTextGeneration) {
  const char* model_path = std::getenv("LLM_EDGEFLOW_TEST_GGUF_MODEL");
  if (!model_path || !*model_path) {
    GTEST_SKIP() << "Set LLM_EDGEFLOW_TEST_GGUF_MODEL for the real GGUF gate";
  }
  ASSERT_TRUE(std::filesystem::is_regular_file(model_path));

  auto backend = BackendRegistry::Instance().Create("llama_cpp");
  ASSERT_NE(backend, nullptr);
  BackendLoadSpec spec;
  spec.model_path = model_path;
  spec.backend_config = {
      {"context_size", 128}, {"decode_batch_size", 128}, {"n_gpu_layers", 0}};
  std::string diagnostic;
  auto base_session = backend->Load(spec, &diagnostic);
  ASSERT_NE(base_session, nullptr) << diagnostic;
  auto session =
      std::dynamic_pointer_cast<ITextGenerationSession>(base_session);
  ASSERT_NE(session, nullptr);
  GenerateOptions direct_options;
  direct_options.max_tokens = 8;
  direct_options.temperature = 0.0f;
  direct_options.top_p = 1.0f;
  std::string direct_output;
  EXPECT_EQ(session->Generate("hello", false, direct_options, 17,
                              &direct_output, &diagnostic),
            0)
      << diagnostic;
  EXPECT_FALSE(direct_output.empty());

  QwenCausalLmModel model(session, "", false, 17);
  GenerateOptions options;
  options.max_tokens = 8;
  options.temperature = 0.0f;
  options.top_p = 1.0f;
  TextBatch outputs;
  EXPECT_EQ(model.Generate({{1, 0, "Reply with OK."}}, options, &outputs), 0);
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_FALSE(outputs[0].data.empty());
}

}  // namespace

TEST(LlamaCppBackendTest, KiteImageProtocolRequiresSafeProjectorConfig) {
  auto backend = BackendRegistry::Instance().Create("kite_llm");
  if (!backend) GTEST_SKIP();
  KiteTestDirectory temporary;
  std::ofstream(temporary.path / "model.gguf") << "invalid";
  BackendLoadSpec spec;
  spec.model_path = (temporary.path / "model.gguf").string();
  spec.requested_protocol = ExecutionProtocol::kImageTextGeneration;
  std::string error;
  EXPECT_EQ(backend->Load(spec, &error), nullptr);
  EXPECT_NE(error.find("vision.mmproj"), std::string::npos);
  spec.backend_config = {{"run_config_file", "run.json"}};
  std::ofstream(temporary.path / "run.json")
      << R"({"schema_version":1,"vision":{"mmproj":"../escape.gguf"}})";
  EXPECT_EQ(backend->Load(spec, &error), nullptr);
  EXPECT_NE(error.find("traverse"), std::string::npos);
  std::ofstream(temporary.path / "run.json")
      << R"({"schema_version":1,"vision":{"mmproj":"missing.gguf"}})";
  EXPECT_EQ(backend->Load(spec, &error), nullptr);
  EXPECT_NE(error.find("does not exist"), std::string::npos);
}

TEST(LlamaCppBackendTest, RealKiteImageTextGeneration) {
  auto backend = BackendRegistry::Instance().Create("kite_llm");
  const char* model = std::getenv("LLM_EDGEFLOW_TEST_KITELLM_VISION_MODEL");
  const char* config = std::getenv("LLM_EDGEFLOW_TEST_KITELLM_VISION_CONFIG");
  if (!backend || !model || !config)
    GTEST_SKIP() << "Set Kite vision model/config for real image gate";
  BackendLoadSpec spec;
  spec.model_path = model;
  spec.backend_config = {{"run_config_file", config}};
  spec.execution_target = {0, "CPU"};
  spec.requested_protocol = ExecutionProtocol::kImageTextGeneration;
  std::string error;
  auto session = std::dynamic_pointer_cast<IImageTextGenerationSession>(
      backend->Load(spec, &error));
  ASSERT_NE(session, nullptr) << error;
  EXPECT_EQ(session->Protocol(), ExecutionProtocol::kImageTextGeneration);
  ImageTextInput input;
  input.prompt = "What color is this image? Answer with one color.";
  input.width = 256;
  input.height = 256;
  input.patch_size = 16;
  input.rgb_chw.assign(3 * 256 * 256, 0);
  std::fill_n(input.rgb_chw.begin(), 256 * 256, 255);
  GenerateOptions options;
  options.temperature = 0;
  options.top_p = 1;
  options.max_tokens = 32;
  std::string output;
  ASSERT_EQ(session->Generate(input, options, &output, &error), 0) << error;
  std::transform(output.begin(), output.end(), output.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  EXPECT_NE(output.find("red"), std::string::npos) << output;
  input.rgb_chw.pop_back();
  EXPECT_NE(session->Generate(input, options, &output, &error), 0);
  EXPECT_TRUE(output.empty());
  EXPECT_NE(session->Generate(input, options, nullptr, &error), 0);
}

}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST(LlamaCppBackendTest, RealKiteGeneratedTokenEmbeddings) {
  auto backend = BackendRegistry::Instance().Create("kite_llm");
  if (!backend) GTEST_SKIP() << "kiteLLM SDK support is disabled";
  const char* model_path = std::getenv("LLM_EDGEFLOW_TEST_KITELLM_MODEL");
  if (!model_path || !*model_path)
    GTEST_SKIP() << "Set LLM_EDGEFLOW_TEST_KITELLM_MODEL";
  KiteTestDirectory temporary;
  std::filesystem::create_symlink(std::filesystem::absolute(model_path),
                                  temporary.path / "model.gguf");
  std::ofstream(temporary.path / "run.json")
      << R"({"schema_version":1,"model":{"context_size":256,"threads":2,"threads_batch":2},"logging":{"level":"error"}})";
  BackendLoadSpec spec;
  spec.model_path = (temporary.path / "model.gguf").string();
  spec.backend_config = {{"run_config_file", "run.json"}};
  spec.requested_protocol = ExecutionProtocol::kGeneratedTokenEmbedding;
  std::string error;
  auto session = std::dynamic_pointer_cast<IGeneratedTokenEmbeddingSession>(
      backend->Load(spec, &error));
  ASSERT_NE(session, nullptr) << error;
  EXPECT_EQ(session->Protocol(), ExecutionProtocol::kGeneratedTokenEmbedding);
  const std::string prompt =
      "<|im_start|>user\nCount from one to "
      "five.<|im_end|>\n<|im_start|>assistant\n";
  GeneratedTokenEmbeddings output;
  ASSERT_EQ(session->GenerateEmbeddings(prompt, false, 2, &output, &error), 0)
      << error;
  ASSERT_EQ(output.values.size(), 2U);
  ASSERT_EQ(output.token_ids.size(), output.values.size());
  const auto expected = output;
  ASSERT_FALSE(output.values[0].empty());
  for (const auto& row : output.values) {
    ASSERT_EQ(row.size(), output.values[0].size());
    double norm = 0;
    for (float value : row) {
      ASSERT_TRUE(std::isfinite(value));
      norm += double(value) * value;
    }
    EXPECT_GT(norm, 0);
  }
  auto run = [&] {
    GeneratedTokenEmbeddings result;
    std::string diagnostic;
    EXPECT_EQ(
        session->GenerateEmbeddings(prompt, false, 2, &result, &diagnostic), 0)
        << diagnostic;
    return result;
  };
  auto first = std::async(std::launch::async, run);
  auto second = std::async(std::launch::async, run);
  for (auto result : {first.get(), second.get()}) {
    EXPECT_EQ(result.token_ids, expected.token_ids);
    ASSERT_EQ(result.values.size(), expected.values.size());
    for (size_t r = 0; r < result.values.size(); ++r) {
      ASSERT_EQ(result.values[r].size(), expected.values[r].size());
      for (size_t d = 0; d < result.values[r].size(); ++d)
        EXPECT_NEAR(result.values[r][d], expected.values[r][d], 1e-5f);
    }
  }
  for (int limit : {0, 65}) {
    EXPECT_NE(
        session->GenerateEmbeddings(prompt, false, limit, &output, &error), 0);
    EXPECT_TRUE(output.values.empty());
    EXPECT_TRUE(output.token_ids.empty());
  }
  EXPECT_NE(session->GenerateEmbeddings("", false, 1, &output, &error), 0);
  EXPECT_NE(session->GenerateEmbeddings(prompt, false, 1, nullptr, &error), 0);
}
}  // namespace llm_edgeflow
