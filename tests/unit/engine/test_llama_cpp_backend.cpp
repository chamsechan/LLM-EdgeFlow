#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "contracts/error_codes.h"
#include "engine/backend_registry.h"
#include "engine/backends/llama_cpp/llama_cpp_backend.h"
#include "engine/text_generation/common_autoregressive_generator.h"

namespace llm_edgeflow {
namespace {

class FailingDecoder final : public text_generation::IAutoregressiveDecoder {
 public:
  explicit FailingDecoder(int failure_stage) : failure_stage_(failure_stage) {}
  int Encode(const std::string&, bool, std::vector<int32_t>* tokens,
             std::string* diagnostic) noexcept override {
    *tokens = {1};
    if (failure_stage_ != 0) return 0;
    *diagnostic = "encode failed";
    return error::kInferenceFailed;
  }
  int DecodeToken(int32_t, std::string* piece,
                  std::string* diagnostic) noexcept override {
    *piece = "prefix";
    if (failure_stage_ != 2) return 0;
    *diagnostic = "decode failed";
    return error::kInferenceFailed;
  }
  bool IsEndToken(int32_t) const noexcept override { return false; }
  size_t MaxContextTokens() const noexcept override { return 8; }
  int Evaluate(const std::vector<int32_t>&, std::vector<float>* logits,
               std::string* diagnostic) noexcept override {
    ++evaluations_;
    *logits = {0.0f, 1.0f};
    if (failure_stage_ != 1 && !(failure_stage_ == 3 && evaluations_ == 2))
      return 0;
    *diagnostic = "evaluate failed";
    return error::kInferenceFailed;
  }

 private:
  int failure_stage_;
  int evaluations_ = 0;
};

TEST(LlamaCppBackendTest,
     GeneratorPreservesDecoderFailureAndClearsPartialText) {
  const char* details[] = {"encode failed", "evaluate failed", "decode failed",
                           "evaluate failed"};
  for (int stage = 0; stage < 4; ++stage) {
    SCOPED_TRACE(stage);
    FailingDecoder decoder(stage);
    GenerateOptions options;
    options.max_tokens = 2;
    options.temperature = 0.0f;
    std::string output = "previous output", diagnostic;
    EXPECT_EQ(text_generation::CommonAutoregressiveGenerator::Generate(
                  decoder, "prompt", false, options, 0, &output, &diagnostic),
              error::kInferenceFailed);
    EXPECT_TRUE(output.empty());
    EXPECT_EQ(diagnostic, details[stage]);
  }
}

TEST(LlamaCppBackendTest, StopWordLookupUsesEarliestMatchRegardlessOfOrder) {
  EXPECT_EQ(text_generation::FindFirstStop("before<end>after<stop>",
                                           {"<stop>", "missing", "<end>"}),
            6u);
  EXPECT_EQ(text_generation::FindFirstStop("<end>after", {"<end>"}), 0u);
  EXPECT_EQ(text_generation::FindFirstStop("text", {"missing"}),
            std::string::npos);
  EXPECT_EQ(text_generation::FindFirstStop("text", {}), std::string::npos);
}

TEST(LlamaCppBackendTest, GeneratorStopsOnWordSpanningDecodedPieces) {
  // 解码器每一步都输出 "prefix"；"fixpre" 跨越两步。
  FailingDecoder decoder(-1);
  GenerateOptions options;
  options.max_tokens = 3;
  options.temperature = 0.0f;
  options.stop_words = {"missing", "fixpre"};
  std::string output, diagnostic;
  ASSERT_EQ(text_generation::CommonAutoregressiveGenerator::Generate(
                decoder, "prompt", false, options, 0, &output, &diagnostic),
            error::kOk);
  EXPECT_EQ(output, "pre");

  options.stop_words.clear();
  ASSERT_EQ(text_generation::CommonAutoregressiveGenerator::Generate(
                decoder, "prompt", false, options, 0, &output, &diagnostic),
            error::kOk);
  EXPECT_EQ(output, "prefixprefixprefix");
}

#ifdef HAVE_LLAMACPP
class InvalidGguf {
 public:
  InvalidGguf() {
    char name[] = "/tmp/edgeflow-invalid-gguf-XXXXXX";
    const int fd = mkstemp(name);
    if (fd < 0) throw std::runtime_error("Cannot create invalid GGUF fixture");
    close(fd);
    file_ = name;
    std::ofstream(file_, std::ios::binary) << "not a GGUF resource";
  }
  ~InvalidGguf() {
    std::error_code error;
    std::filesystem::remove(file_, error);
  }
  const std::string& File() const { return file_; }

 private:
  std::string file_;
};

BackendLoadSpec LlamaSpec(
    const std::string& file,
    const nlohmann::json& params = nlohmann::json::object()) {
  BackendLoadSpec spec(ExecutionProtocol::kTextGeneration);
  const auto definition = BackendRegistry::Instance().Find("llama_cpp");
  EXPECT_TRUE(definition.has_value());
  std::string diagnostic;
  if (definition) {
    EXPECT_TRUE(definition->params.Parse(params, &spec.params, &diagnostic))
        << diagnostic;
  }
  spec.model_file = file;
  spec.execution_target = {0, "CPU"};
  return spec;
}

TEST(LlamaCppBackendTest,
     RealProviderRejectsAbsentAndInvalidWeightsWithoutPublishingSession) {
  auto provider = BackendRegistry::Instance().Create("llama_cpp");
  ASSERT_NE(provider, nullptr);
  std::string diagnostic;
  auto spec = LlamaSpec("");
  EXPECT_EQ(provider->Load(spec, &diagnostic), nullptr);
  EXPECT_EQ(diagnostic, "llama.cpp model path is empty");
  InvalidGguf invalid;
  spec.model_file = invalid.File() + ".missing";
  EXPECT_EQ(provider->Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("not a regular file"), std::string::npos);
  spec.model_file = invalid.File();
  // 两次尝试都进入实际的厂商加载器；任何一次失败都不得留下 Session。
  for (int attempt = 0; attempt < 2; ++attempt) {
    diagnostic.clear();
    EXPECT_EQ(provider->Load(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("failed to load GGUF"), std::string::npos)
        << diagnostic;
  }
}

TEST(LlamaCppBackendTest,
     LoadConsumesParsedParametersAndRejectsContradictoryExecutionTarget) {
  LlamaCppBackend provider;
  InvalidGguf invalid;
  std::string diagnostic;
  auto spec = LlamaSpec(invalid.File());
  spec.execution_target = {1, "CPU"};
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("device_id 0"), std::string::npos);
  spec.execution_target = {-1, "UNKNOWN"};
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("non-negative"), std::string::npos);
  spec.execution_target = {0, "CUDA"};
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("requires n_gpu_layers > 0"), std::string::npos);
  spec = LlamaSpec(invalid.File(), {{"n_gpu_layers", 1}});
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("requires a GPU execution platform"),
            std::string::npos);
  auto definition = BackendRegistry::Instance().Find("llama_cpp");
  ASSERT_TRUE(definition);
  std::shared_ptr<const ParameterValues> params;
  EXPECT_FALSE(definition->params.Parse(
      {{"context_size", 32}, {"decode_batch_size", 33}}, &params, &diagnostic));
  EXPECT_EQ(params, nullptr);
  EXPECT_NE(diagnostic.find("must not exceed context_size"), std::string::npos);
  ASSERT_TRUE(
      definition->params.Parse(nlohmann::json::object(), &params, &diagnostic));
  EXPECT_EQ(params->Effective(), (nlohmann::json{{"context_size", 2048},
                                                 {"decode_batch_size", 512},
                                                 {"n_threads", 0},
                                                 {"n_threads_batch", 0},
                                                 {"n_gpu_layers", 0},
                                                 {"check_tensors", false}}));
  struct Case {
    nlohmann::json config;
    int64_t decode_batch_size;
  };
  for (const auto& item :
       {Case{{{"context_size", 128}}, 128},
        Case{{{"context_size", 1048576}}, 512},
        Case{{{"context_size", 128}, {"decode_batch_size", 64}}, 64}}) {
    SCOPED_TRACE(item.config.dump());
    ASSERT_TRUE(definition->params.Parse(item.config, &params, &diagnostic))
        << diagnostic;
    EXPECT_EQ(params->Integer("decode_batch_size"), item.decode_batch_size);
  }
}

TEST(LlamaCppBackendTest,
     SuppliedGgufProducesSeededOwnedTextThroughNeutralSession) {
  const char* file = std::getenv("EDGEFLOW_TEST_LLAMA_GGUF");
  if (!file || !*file) {
    RecordProperty("optional_runtime_asset", "EDGEFLOW_TEST_LLAMA_GGUF");
    GTEST_SKIP() << "EDGEFLOW_TEST_LLAMA_GGUF is not supplied";
  }
  std::string diagnostic;
  auto provider = BackendRegistry::Instance().Create("llama_cpp");
  ASSERT_NE(provider, nullptr);
  auto base = provider->Load(LlamaSpec(file, {{"context_size", 256},
                                              {"decode_batch_size", 64},
                                              {"n_threads", 2},
                                              {"n_threads_batch", 2}}),
                             &diagnostic);
  auto session = std::dynamic_pointer_cast<ITextGenerationSession>(base);
  ASSERT_NE(session, nullptr) << diagnostic;
  EXPECT_EQ(session->BackendType(), "llama_cpp");
  EXPECT_EQ(session->Protocol(), ExecutionProtocol::kTextGeneration);
  EXPECT_EQ(session->Concurrency(), InferenceConcurrency::kSerialized);
  EXPECT_EQ(session->GetBatchPolicy().max_batch_size, 1u);
  EXPECT_EQ(session->GetBatchPolicy().fixed_batch_size, 0u);
  EXPECT_TRUE(session->SupportsRandomSeed());
  GenerateOptions options;
  options.max_tokens = 4;
  options.temperature = 0.0f;
  options.top_p = 1.0f;
  std::string first = "previous";
  ASSERT_EQ(session->Generate("Hello", true, options, 17, &first, &diagnostic),
            0)
      << diagnostic;
  std::string second;
  ASSERT_EQ(session->Generate("Hello", true, options, 17, &second, &diagnostic),
            0)
      << diagnostic;
  EXPECT_EQ(first, second);
  EXPECT_EQ(session->Generate("Hello", true, options, 17, nullptr, &diagnostic),
            error::kInvalidArgument);
  const auto retained = first;
  provider.reset();
  base.reset();
  session.reset();
  EXPECT_EQ(first, retained);
}
#endif

TEST(LlamaCppBackendTest, ProtocolAndBuildAvailabilityAreExplicit) {
  LlamaCppBackend provider;
  EXPECT_EQ(provider.BackendType(), "llama_cpp");
  BackendLoadSpec wrong(ExecutionProtocol::kTensorGraph);
  std::string diagnostic;
  EXPECT_EQ(provider.Load(wrong, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("does not support requested protocol"),
            std::string::npos);
#ifdef HAVE_LLAMACPP
  const auto definition = BackendRegistry::Instance().Find("llama_cpp");
  ASSERT_TRUE(definition);
  EXPECT_EQ(
      definition->supported_protocols,
      (std::vector<ExecutionProtocol>{ExecutionProtocol::kTextGeneration}));
  EXPECT_EQ(definition->concurrency, InferenceConcurrency::kSerialized);
#else
  EXPECT_FALSE(BackendRegistry::Instance().Find("llama_cpp"));
  EXPECT_EQ(BackendRegistry::Instance().Create("llama_cpp"), nullptr);
  BackendLoadSpec disabled(ExecutionProtocol::kTextGeneration);
  EXPECT_EQ(provider.Load(disabled, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("not compiled"), std::string::npos);
#endif
}

}  // namespace
}  // namespace llm_edgeflow
