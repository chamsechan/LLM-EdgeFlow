#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/backend_registry.h"
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
  EXPECT_EQ(definition->supported_protocols,
            std::vector<ExecutionProtocol>({ExecutionProtocol::kCausalLm}));
  EXPECT_EQ(definition->concurrency, InferenceConcurrency::kSerialized);
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
  std::ifstream header("src/engine/backends/llama_cpp/llama_cpp_backend.h");
  ASSERT_TRUE(header.is_open());
  const std::string source((std::istreambuf_iterator<char>(header)),
                           std::istreambuf_iterator<char>());
  EXPECT_EQ(source.find("llama.h"), std::string::npos);
  EXPECT_EQ(source.find("llama_model"), std::string::npos);
  EXPECT_EQ(source.find("llama_context"), std::string::npos);
}

TEST(LlamaCppBackendTest, RealGgufLoadCodecAndEvaluate) {
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
  auto session = std::dynamic_pointer_cast<ICausalLmSession>(base_session);
  ASSERT_NE(session, nullptr);
  auto state = session->CreateSequence(&diagnostic);
  ASSERT_NE(state, nullptr) << diagnostic;
  std::vector<int32_t> tokens;
  ASSERT_EQ(session->TokenCodec().Encode("hello", false, &tokens, &diagnostic),
            0)
      << diagnostic;
  ASSERT_FALSE(tokens.empty());
  std::vector<float> logits;
  EXPECT_EQ(state->Evaluate(tokens, &logits, &diagnostic), 0) << diagnostic;
  EXPECT_FALSE(logits.empty());

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
