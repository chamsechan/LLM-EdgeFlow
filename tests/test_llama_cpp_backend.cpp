#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/backend_registry.h"
#include "engine/backends/kite_llm/kite_llm_backend.h"
#include "engine/models/qwen_causal_lm/qwen_causal_lm_model.h"

namespace alg_framework {
namespace {

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
}

TEST(LlamaCppBackendTest, KiteRegistrationMatchesConditionalBuild) {
  const auto definition = BackendRegistry::Instance().Find("kite_llm");
  auto registered = BackendRegistry::Instance().Create("kite_llm");
  EXPECT_EQ(definition.has_value(), registered != nullptr);
  if (definition.has_value()) {
    EXPECT_EQ(
        definition->supported_protocols,
        std::vector<ExecutionProtocol>({ExecutionProtocol::kTextGeneration}));
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

TEST(LlamaCppBackendTest, KiteExecutionTargetMustComeFromRunConfig) {
  KiteLlmBackend backend;
  BackendLoadSpec spec;
  spec.model_path = "./models/does-not-exist";
  spec.requested_protocol = ExecutionProtocol::kTextGeneration;
  spec.execution_target.platform = "CUDA";
  spec.execution_target.device_id = 1;
  std::string diagnostic;
  EXPECT_EQ(backend.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("run_config_file"), std::string::npos);
}

TEST(LlamaCppBackendTest, RealKiteSdkGenerationAndFixedSeedPolicy) {
  auto backend = BackendRegistry::Instance().Create("kite_llm");
  if (!backend) GTEST_SKIP() << "kiteLLM SDK support is disabled";
  const char* model_path = std::getenv("LLM_EDGEFLOW_TEST_KITELLM_MODEL");
  if (!model_path || !*model_path) {
    GTEST_SKIP() << "Set LLM_EDGEFLOW_TEST_KITELLM_MODEL for the real SDK gate";
  }

  BackendLoadSpec spec;
  spec.model_path = model_path;
  spec.requested_protocol = ExecutionProtocol::kTextGeneration;
  std::string diagnostic;
  auto base = backend->Load(spec, &diagnostic);
  ASSERT_NE(base, nullptr) << diagnostic;
  auto session = std::dynamic_pointer_cast<ITextGenerationSession>(base);
  ASSERT_NE(session, nullptr);

  GenerateOptions options;
  options.max_tokens = 8;
  std::string output;
  EXPECT_NE(session->Generate("Reply with OK.", false, options, 17, &output,
                              &diagnostic),
            0);
  EXPECT_NE(diagnostic.find("fixed random seed"), std::string::npos);

  diagnostic.clear();
  EXPECT_EQ(session->Generate("Reply with OK.", false, options, std::nullopt,
                              &output, &diagnostic),
            0)
      << diagnostic;
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
}  // namespace alg_framework
