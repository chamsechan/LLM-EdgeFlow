#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/backend_registry.h"
#include "engine/backends/kite_llm/kite_llm_backend.h"

namespace llm_edgeflow {
namespace {

TEST(KiteBackendTest, UnsupportedProtocolAndNonCpuTargetFailBeforeResources) {
  KiteLlmBackend provider;
  EXPECT_EQ(provider.BackendType(), "kite_llm");
  BackendLoadSpec spec(ExecutionProtocol::kTensorGraph);
  std::string diagnostic;
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(
      diagnostic.find("does not support requested protocol: tensor_graph"),
      std::string::npos);
  spec.requested_protocol = ExecutionProtocol::kTextGeneration;
  spec.execution_target = {0, "CUDA"};
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("supports CPU execution only"), std::string::npos);
  spec.execution_target = {-2, "CPU"};
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("device_id must be >= -1"), std::string::npos);
  spec.execution_target = {1, "cpu"};
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("only accepts device_id 0"), std::string::npos);
}

#ifdef HAVE_KITELLM
class KiteFiles {
 public:
  KiteFiles() {
    char name[] = "/tmp/edgeflow-kite-config-XXXXXX";
    if (!mkdtemp(name))
      throw std::runtime_error("Cannot create Kite resource fixtures");
    directory_ = name;
    std::ofstream(directory_ / "model.gguf", std::ios::binary)
        << "not vendor weights";
  }
  ~KiteFiles() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  std::string Model() const { return (directory_ / "model.gguf").string(); }
  std::string Config(const nlohmann::json& value) {
    const auto path = directory_ / "run.json";
    std::ofstream(path) << value.dump();
    return path.string();
  }

 private:
  std::filesystem::path directory_;
};

BackendLoadSpec KiteSpec(
    ExecutionProtocol protocol, const std::string& file,
    const nlohmann::json& params = nlohmann::json::object()) {
  BackendLoadSpec spec(protocol);
  const auto definition = BackendRegistry::Instance().Find("kite_llm");
  EXPECT_TRUE(definition);
  std::string diagnostic;
  if (definition)
    EXPECT_TRUE(definition->params.Parse(params, &spec.params, &diagnostic))
        << diagnostic;
  spec.model_file = file;
  spec.execution_target = {0, "CPU"};
  return spec;
}

TEST(KiteBackendTest, EnabledDefinitionDeclaresOnlyTheThreeNeutralProtocols) {
  const auto definition = BackendRegistry::Instance().Find("kite_llm");
  ASSERT_TRUE(definition);
  EXPECT_EQ(definition->supported_protocols,
            (std::vector<ExecutionProtocol>{
                ExecutionProtocol::kTextGeneration,
                ExecutionProtocol::kImageTextGeneration,
                ExecutionProtocol::kGeneratedTokenEmbedding}));
  EXPECT_EQ(definition->concurrency, InferenceConcurrency::kSerialized);
  EXPECT_NE(BackendRegistry::Instance().Create("kite_llm"), nullptr);
  std::shared_ptr<const ParameterValues> params;
  std::string diagnostic;
  ASSERT_TRUE(
      definition->params.Parse(nlohmann::json::object(), &params, &diagnostic));
  EXPECT_EQ(params->Effective(), nlohmann::json::object());
  ASSERT_EQ(definition->params.Fields().size(), 1u);
  EXPECT_EQ(definition->params.Fields()[0].name, "run_config_file");
}

TEST(KiteBackendTest,
     LoadRejectsResourcesAndVisionConfigurationBeforeCallingPrivateSdk) {
  KiteLlmBackend provider;
  KiteFiles files;
  std::string diagnostic;
  auto spec = KiteSpec(ExecutionProtocol::kTextGeneration, "");
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_EQ(diagnostic, "kiteLLM model path is empty");
  spec.model_file = files.Model() + ".missing";
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("not a regular file"), std::string::npos);
  spec = KiteSpec(ExecutionProtocol::kTextGeneration, files.Model(),
                  {{"run_config_file", files.Model() + ".missing"}});
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("run-config file does not exist"),
            std::string::npos);
  spec = KiteSpec(ExecutionProtocol::kImageTextGeneration, files.Model());
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_EQ(diagnostic,
            "image_text_generation requires run-config vision.mmproj");
  const auto reject_config = [&](nlohmann::json config,
                                 const std::string& expected) {
    auto candidate =
        KiteSpec(ExecutionProtocol::kImageTextGeneration, files.Model(),
                 {{"run_config_file", files.Config(config)}});
    diagnostic.clear();
    EXPECT_EQ(provider.Load(candidate, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find(expected), std::string::npos) << diagnostic;
  };
  reject_config({{"model", {{"gpu_layers", 1}}},
                 {"vision", {{"mmproj", "projector.gguf"}}}},
                "conflicts with run-config");
  reject_config({{"vision", {{"mmproj", 42}}}},
                "Invalid run-config vision.mmproj");
  reject_config({{"vision", {{"mmproj", "../projector.gguf"}}}},
                "Invalid run-config vision.mmproj");
  reject_config({{"vision", {{"mmproj", "missing-projector.gguf"}}}},
                "projector file does not exist");
  // No private SDK layout or success inference is simulated by these pre-load
  // checks.
}
#else
TEST(KiteBackendTest,
     DisabledBackendHasNoRegistrationForAnyAdvertisedProtocol) {
  EXPECT_FALSE(BackendRegistry::Instance().Find("kite_llm"));
  EXPECT_EQ(BackendRegistry::Instance().Create("kite_llm"), nullptr);
  KiteLlmBackend provider;
  for (const auto protocol : {ExecutionProtocol::kTextGeneration,
                              ExecutionProtocol::kImageTextGeneration,
                              ExecutionProtocol::kGeneratedTokenEmbedding}) {
    BackendLoadSpec spec(protocol);
    spec.execution_target = {-1, "CPU"};
    std::string diagnostic;
    EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
    EXPECT_EQ(diagnostic, "kiteLLM SDK was not compiled into this build");
  }
}
#endif

}  // namespace
}  // namespace llm_edgeflow
