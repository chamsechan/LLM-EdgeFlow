#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "contracts/error_codes.h"
#include "engine/backend_registry.h"
#include "engine/backends/inferlet/inferlet_backend.h"

namespace llm_edgeflow {
namespace {

TEST(InferletBackendTest, UnsupportedProtocolAndExecutionTargetAreExplicit) {
  InferletBackend provider;
  EXPECT_EQ(provider.BackendType(), "inferlet");
  BackendLoadSpec spec(ExecutionProtocol::kTensorGraph);
  std::string diagnostic;
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("Inferlet"), std::string::npos);
  EXPECT_NE(diagnostic.find("tensor_graph"), std::string::npos);
  spec.requested_protocol = ExecutionProtocol::kTextGeneration;
  for (const auto& target :
       std::vector<ExecutionTarget>{{0, "CUDA"}, {-2, "CPU"}, {1, "cpu"}}) {
    spec.execution_target = target;
    diagnostic.clear();
    EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("Inferlet"), std::string::npos);
    EXPECT_TRUE(diagnostic.find("CPU") != std::string::npos ||
                diagnostic.find("device_id") != std::string::npos)
        << diagnostic;
  }
}

#ifdef HAVE_INFERLET
class InferletFiles {
 public:
  InferletFiles() {
    char name[] = "/tmp/edgeflow-inferlet-XXXXXX";
    if (!mkdtemp(name))
      throw std::runtime_error("Cannot create Inferlet fixture");
    directory_ = name;
    std::ofstream(Model(), std::ios::binary) << "not GGUF weights";
  }
  ~InferletFiles() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  std::string Model() const { return (directory_ / "model.gguf").string(); }
  std::string Config(const std::string& text) const {
    const auto path = directory_ / "run.json";
    std::ofstream(path) << text;
    return path.string();
  }

 private:
  std::filesystem::path directory_;
};

BackendLoadSpec InferletSpec(
    ExecutionProtocol protocol, const std::string& file,
    const nlohmann::json& params = nlohmann::json::object()) {
  BackendLoadSpec spec(protocol);
  const auto definition = BackendRegistry::Instance().Find("inferlet");
  EXPECT_TRUE(definition);
  std::string diagnostic;
  if (definition) {
    EXPECT_TRUE(definition->params.Parse(params, &spec.params, &diagnostic))
        << diagnostic;
  }
  spec.model_file = file;
  spec.execution_target = {0, "CPU"};
  return spec;
}

void ExpectSessionContract(const IBackendSession& session,
                           ExecutionProtocol protocol) {
  EXPECT_EQ(session.BackendType(), "inferlet");
  EXPECT_EQ(session.Protocol(), protocol);
  EXPECT_EQ(session.Concurrency(), InferenceConcurrency::kSerialized);
  EXPECT_EQ(session.GetBatchPolicy().max_batch_size, 1u);
  EXPECT_EQ(session.GetBatchPolicy().fixed_batch_size, 0u);
}

TEST(InferletBackendTest, DefinitionDeclaresProtocolsAndOptionalFileParameter) {
  const auto definition = BackendRegistry::Instance().Find("inferlet");
  ASSERT_TRUE(definition);
  EXPECT_NE(BackendRegistry::Instance().Create("inferlet"), nullptr);
  EXPECT_EQ(definition->supported_protocols,
            (std::vector<ExecutionProtocol>{
                ExecutionProtocol::kTextGeneration,
                ExecutionProtocol::kImageTextGeneration,
                ExecutionProtocol::kGeneratedTokenEmbedding}));
  EXPECT_EQ(definition->concurrency, InferenceConcurrency::kSerialized);
  ASSERT_EQ(definition->params.Fields().size(), 1u);
  EXPECT_EQ(definition->params.Fields()[0].name, "run_config_file");
  EXPECT_TRUE(definition->params.Fields()[0].file);
  std::shared_ptr<const ParameterValues> params;
  std::string diagnostic;
  ASSERT_TRUE(
      definition->params.Parse(nlohmann::json::object(), &params, &diagnostic));
  EXPECT_EQ(params->Effective(), nlohmann::json::object());
  EXPECT_FALSE(definition->params.Parse({{"run_config_file", 42}}, &params,
                                        &diagnostic));
}

TEST(InferletBackendTest, RejectsMissingResourcesAndInvalidRunConfiguration) {
  InferletBackend provider;
  InferletFiles files;
  auto spec = InferletSpec(ExecutionProtocol::kTextGeneration, "");
  std::string diagnostic;
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("model path is empty"), std::string::npos);
  spec.model_file = files.Model() + ".missing";
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("not a regular file"), std::string::npos);
  spec = InferletSpec(ExecutionProtocol::kTextGeneration, files.Model(),
                      {{"run_config_file", files.Model() + ".missing"}});
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("run-config"), std::string::npos);
  for (const auto& config : {"{broken", "[]", "null", "42"}) {
    SCOPED_TRACE(config);
    spec = InferletSpec(ExecutionProtocol::kTextGeneration, files.Model(),
                        {{"run_config_file", files.Config(config)}});
    EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("run-config"), std::string::npos) << diagnostic;
  }
  for (const std::string platform : {"CPU", "UNKNOWN", ""}) {
    spec = InferletSpec(
        ExecutionProtocol::kTextGeneration, files.Model(),
        {{"run_config_file", files.Config(R"({"model":{"gpu_layers":1}})")}});
    spec.execution_target = {std::nullopt, platform};
    EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("gpu_layers"), std::string::npos) << diagnostic;
  }
}

TEST(InferletBackendTest, VisionRequiresSafeExistingProjector) {
  InferletBackend provider;
  InferletFiles files;
  auto spec =
      InferletSpec(ExecutionProtocol::kImageTextGeneration, files.Model());
  std::string diagnostic;
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("vision.mmproj"), std::string::npos);
  for (const nlohmann::json& projector :
       {nlohmann::json(42), nlohmann::json("../projector.gguf"),
        nlohmann::json("missing-projector.gguf")}) {
    const nlohmann::json config = {{"vision", {{"mmproj", projector}}}};
    spec = InferletSpec(ExecutionProtocol::kImageTextGeneration, files.Model(),
                        {{"run_config_file", files.Config(config.dump())}});
    EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
    EXPECT_TRUE(diagnostic.find("vision.mmproj") != std::string::npos ||
                diagnostic.find("projector file") != std::string::npos)
        << diagnostic;
  }
}

TEST(InferletBackendTest, InvalidWeightsFailSynchronouslyAcrossRepeatedLoads) {
  InferletBackend provider;
  InferletFiles files;
  auto spec = InferletSpec(ExecutionProtocol::kTextGeneration, files.Model());
  for (const auto& target : std::vector<ExecutionTarget>{
           {std::nullopt, ""}, {-1, "UNKNOWN"}, {0, "cpu"}}) {
    spec.execution_target = target;
    std::string diagnostic;
    EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("Inferlet"), std::string::npos);
    EXPECT_NE(diagnostic.find("load"), std::string::npos) << diagnostic;
  }
}

TEST(InferletBackendTest, SuppliedTextModelOwnsResourcesAndSerializesCalls) {
  const char* file = std::getenv("EDGEFLOW_INFERLET_TEST_MODEL");
  if (!file || !*file) {
    RecordProperty("optional_runtime_asset", "EDGEFLOW_INFERLET_TEST_MODEL");
    GTEST_SKIP() << "EDGEFLOW_INFERLET_TEST_MODEL is not supplied";
  }
  auto provider = BackendRegistry::Instance().Create("inferlet");
  ASSERT_NE(provider, nullptr);
  std::string diagnostic;
  auto session =
      std::dynamic_pointer_cast<ITextGenerationSession>(provider->Load(
          InferletSpec(ExecutionProtocol::kTextGeneration, file), &diagnostic));
  ASSERT_NE(session, nullptr) << diagnostic;
  ExpectSessionContract(*session, ExecutionProtocol::kTextGeneration);
  EXPECT_FALSE(session->SupportsRandomSeed());
  provider.reset();  // Session 独立于提供者持有运行时资源。
  GenerateOptions options;
  options.max_tokens = 8;
  options.temperature = 0.0f;
  options.top_p = 1.0f;
  std::string first = "previous", second;
  ASSERT_EQ(session->Generate("Hello", true, options, std::nullopt, &first,
                              &diagnostic),
            0)
      << diagnostic;
  ASSERT_FALSE(first.empty());
  ASSERT_EQ(session->Generate("Hello", true, options, std::nullopt, &second,
                              &diagnostic),
            0)
      << diagnostic;
  EXPECT_EQ(first, second);
  EXPECT_NE(session->Generate("Hello", true, options, 7, &second, &diagnostic),
            0);
  EXPECT_TRUE(second.empty());
  EXPECT_NE(diagnostic.find("seed"), std::string::npos);
  EXPECT_EQ(session->Generate("Hello", true, options, std::nullopt, nullptr,
                              &diagnostic),
            error::kInvalidArgument);
  options.stop_words = {first};
  ASSERT_EQ(session->Generate("Hello", true, options, std::nullopt, &second,
                              &diagnostic),
            0)
      << diagnostic;
  EXPECT_TRUE(second.empty());
  options.stop_words.clear();
  const auto generate = [&] {
    std::string output, detail;
    EXPECT_EQ(session->Generate("Hello", true, options, std::nullopt, &output,
                                &detail),
              0)
        << detail;
    return output;
  };
  auto a = std::async(std::launch::async, generate);
  auto b = std::async(std::launch::async, generate);
  EXPECT_EQ(a.get(), first);
  EXPECT_EQ(b.get(), first);
  const auto retained = first;
  session.reset();
  EXPECT_EQ(first, retained);
}

TEST(InferletBackendTest, SuppliedModelProducesOwnedFiniteTokenEmbeddings) {
  const char* file = std::getenv("EDGEFLOW_INFERLET_TEST_MODEL");
  if (!file || !*file) {
    RecordProperty("optional_runtime_asset", "EDGEFLOW_INFERLET_TEST_MODEL");
    GTEST_SKIP() << "EDGEFLOW_INFERLET_TEST_MODEL is not supplied";
  }
  InferletBackend provider;
  std::string diagnostic;
  auto session =
      std::dynamic_pointer_cast<IGeneratedTokenEmbeddingSession>(provider.Load(
          InferletSpec(ExecutionProtocol::kGeneratedTokenEmbedding, file),
          &diagnostic));
  ASSERT_NE(session, nullptr) << diagnostic;
  ExpectSessionContract(*session, ExecutionProtocol::kGeneratedTokenEmbedding);
  GeneratedTokenEmbeddings output;
  ASSERT_EQ(session->GenerateEmbeddings("Hello", true, 3, &output, &diagnostic),
            0)
      << diagnostic;
  ASSERT_FALSE(output.values.empty());
  EXPECT_LE(output.values.size(), 3u);
  ASSERT_EQ(output.values.size(), output.token_ids.size());
  ASSERT_FALSE(output.values[0].empty());
  for (const auto& row : output.values) {
    EXPECT_EQ(row.size(), output.values[0].size());
    for (float value : row) EXPECT_TRUE(std::isfinite(value));
  }
  const auto retained = output;
  GeneratedTokenEmbeddings next;
  ASSERT_EQ(session->GenerateEmbeddings("Goodbye", true, 2, &next, &diagnostic),
            0)
      << diagnostic;
  EXPECT_NE(session->GenerateEmbeddings("Hello", true, 0, &next, &diagnostic),
            0);
  EXPECT_TRUE(next.token_ids.empty());
  EXPECT_TRUE(next.values.empty());
  session.reset();
  EXPECT_EQ(output.token_ids, retained.token_ids);
  EXPECT_EQ(output.values, retained.values);
}

TEST(InferletBackendTest, SuppliedVisionModelValidatesPixelsAndGeneratesText) {
  const char* file = std::getenv("EDGEFLOW_INFERLET_TEST_VISION_MODEL");
  const char* config = std::getenv("EDGEFLOW_INFERLET_TEST_VISION_CONFIG");
  if (!file || !*file || !config || !*config) {
    RecordProperty("optional_runtime_asset",
                   "EDGEFLOW_INFERLET_TEST_VISION_MODEL/CONFIG");
    GTEST_SKIP() << "Inferlet vision model/config are not supplied";
  }
  InferletBackend provider;
  std::string diagnostic;
  auto session = std::dynamic_pointer_cast<IImageTextGenerationSession>(
      provider.Load(InferletSpec(ExecutionProtocol::kImageTextGeneration, file,
                                 {{"run_config_file", config}}),
                    &diagnostic));
  ASSERT_NE(session, nullptr) << diagnostic;
  ExpectSessionContract(*session, ExecutionProtocol::kImageTextGeneration);
  ImageTextInput input;
  input.prompt = "Describe this image in English.";
  input.width = input.height = 64;
  input.patch_size = 16;
  GenerateOptions options;
  options.max_tokens = 8;
  options.temperature = 0.0f;
  std::string output = "previous";
  EXPECT_EQ(session->Generate(input, options, &output, &diagnostic),
            error::kInvalidInput);
  EXPECT_TRUE(output.empty());
  input.rgb_chw.assign(64 * 64 * 3, 255);
  ASSERT_EQ(session->Generate(input, options, &output, &diagnostic), 0)
      << diagnostic;
  EXPECT_FALSE(output.empty());
}
#else
TEST(InferletBackendTest, DisabledBuildDoesNotRegisterAnyProtocol) {
  EXPECT_FALSE(BackendRegistry::Instance().Find("inferlet"));
  EXPECT_EQ(BackendRegistry::Instance().Create("inferlet"), nullptr);
  InferletBackend provider;
  for (const auto protocol : {ExecutionProtocol::kTextGeneration,
                              ExecutionProtocol::kImageTextGeneration,
                              ExecutionProtocol::kGeneratedTokenEmbedding}) {
    BackendLoadSpec spec(protocol);
    std::string diagnostic;
    EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("Inferlet"), std::string::npos);
    EXPECT_NE(diagnostic.find("not compiled"), std::string::npos);
  }
}
#endif

}  // namespace
}  // namespace llm_edgeflow
