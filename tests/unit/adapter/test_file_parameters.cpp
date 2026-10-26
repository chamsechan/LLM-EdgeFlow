#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "adapter/model_file_resolver.h"
#include "contracts/error_codes.h"
#include "contracts/parameters.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;
struct ModelFiles {
  std::optional<std::string> tokens;
  std::string note;
};
struct BackendFiles {
  std::optional<std::string> run_config;
};

bool RegisterFileDeclarations() {
  static const bool registered = [] {
    BackendDefinition backend;
    backend.backend_type = "file_resolution_fixture";
    backend.supported_protocols = {ExecutionProtocol::kFixture};
    backend.params = Parameters<BackendFiles>{
        Field("run_config", &BackendFiles::run_config).File()};
    if (!BackendRegistry::Instance().Register(
            backend, [] { return std::unique_ptr<IInferenceBackend>{}; }))
      return false;
    ModelDefinition model;
    model.impl_name = "file_resolution_model";
    model.model_type = "llm";
    model.required_protocol = ExecutionProtocol::kFixture;
    model.fixture_backends = {backend.backend_type};
    model.params =
        Parameters<ModelFiles>{Field("token/file~", &ModelFiles::tokens).File(),
                               Field("note", &ModelFiles::note).Default("")};
    return ModelRegistry::Instance().Register(
        model, [](const ModelCreateContext&, std::string*) {
          return std::shared_ptr<IModel>{};
        });
  }();
  return registered;
}

Json FileDocument() {
  return {
      {"models",
       Json::array({{{"type", "llm"},
                     {"name", "model"},
                     {"file", "weights/model.bin"},
                     {"params",
                      {{"token/file~", "tokens/vocab.txt"},
                       {"note", "../opaque prompt"}}},
                     {"backend",
                      {{"type", "file_resolution_fixture"},
                       {"params", {{"run_config", "runtime/run.json"}}}}}}})},
      {"other", "unchanged"}};
}

class FileParametersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(RegisterFileDeclarations());
    char directory[] = "/tmp/edgeflow-file-resolution-XXXXXX";
    ASSERT_NE(mkdtemp(directory), nullptr);
    directory_ = std::filesystem::canonical(directory);
    root_ = directory_ / "pipeline";
    std::filesystem::create_directory(root_);
  }
  void TearDown() override {
    std::error_code error;
    if (!directory_.empty()) std::filesystem::remove_all(directory_, error);
    EXPECT_FALSE(error) << error.message();
  }
  std::filesystem::path directory_;
  std::filesystem::path root_;
};

TEST_F(
    FileParametersTest,
    KnownModelAndDeclaredFilesResolveAgainstJsonDirectoryWithoutExistenceChecks) {
  const auto source = FileDocument();
  Json resolved;
  std::string error;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(
      ResolveModelFiles(source, root_.string(), &resolved, &error, &diagnostic))
      << error;
  auto expected = source;
  expected["models"][0]["file"] = (root_ / "weights/model.bin").string();
  expected["models"][0]["params"]["token/file~"] =
      (root_ / "tokens/vocab.txt").string();
  expected["models"][0]["backend"]["params"]["run_config"] =
      (root_ / "runtime/run.json").string();
  EXPECT_EQ(resolved, expected);
  EXPECT_EQ(source, FileDocument());
  EXPECT_EQ(resolved["models"][0]["params"]["note"], "../opaque prompt");
  EXPECT_FALSE(std::filesystem::exists(root_ / "weights/model.bin"));
  EXPECT_EQ(diagnostic.code, error::kOk);
}

TEST_F(FileParametersTest, NoDirectoryStillEnforcesPortableRelativeSpelling) {
  auto source = FileDocument();
  source["models"][0]["file"] = "weights\\.\\model.bin";
  Json resolved;
  std::string error;
  ASSERT_TRUE(ResolveModelFiles(source, "", &resolved, &error));
  auto expected = source;
  expected["models"][0]["file"] = "weights/model.bin";
  EXPECT_EQ(resolved, expected);
  source["models"][0]["params"].erase("token/file~");
  source["models"][0]["backend"]["params"].erase("run_config");
  ASSERT_TRUE(ResolveModelFiles(source, root_.string(), &resolved, &error));
  EXPECT_FALSE(resolved["models"][0]["params"].contains("token/file~"));
  EXPECT_TRUE(resolved["models"][0]["backend"]["params"].empty());
}

TEST_F(
    FileParametersTest,
    UnsafeModelFilenamesFailAtomicallyIncludingLexicallyLocalParentComponents) {
  const std::vector<std::string> filenames{"",
                                           "../outside.bin",
                                           "weights/../model.bin",
                                           "/tmp/model.bin",
                                           "C:model.bin",
                                           "C:\\model.bin",
                                           "\\\\server\\share\\model.bin",
                                           std::string("model\0.bin", 10)};
  for (const auto& filename : filenames) {
    SCOPED_TRACE(filename);
    auto source = FileDocument();
    source["models"][0]["file"] = filename;
    const auto original = source;
    Json resolved = {{"previous", true}};
    std::string error;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(ResolveModelFiles(source, root_.string(), &resolved, &error,
                                   &diagnostic));
    EXPECT_TRUE(resolved.is_null());
    EXPECT_EQ(source, original);
    EXPECT_EQ(diagnostic.code, error::kInvalidFilePath);
    EXPECT_EQ(diagnostic.path, "/models/0/file");
    EXPECT_FALSE(error.empty());
  }
}

TEST_F(FileParametersTest,
       DeclaredFileErrorsUseEscapedFieldPathsAndDiscardEarlierResolutions) {
  struct InvalidField {
    const char* pointer;
    const char* expected_path;
  };
  const std::array<InvalidField, 2> fields{
      {{"/models/0/params/token~1file~0", "/models/0/params/token~1file~0"},
       {"/models/0/backend/params/run_config",
        "/models/0/backend/params/run_config"}}};
  for (const auto& field : fields) {
    SCOPED_TRACE(field.pointer);
    auto source = FileDocument();
    source[Json::json_pointer(field.pointer)] = "local/../forbidden.bin";
    Json resolved = {{"previous", true}};
    std::string error;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(ResolveModelFiles(source, root_.string(), &resolved, &error,
                                   &diagnostic));
    EXPECT_TRUE(resolved.is_null());
    EXPECT_EQ(diagnostic.code, error::kInvalidFilePath);
    EXPECT_EQ(diagnostic.path, field.expected_path);
    EXPECT_EQ(source["models"][0]["file"], "weights/model.bin");
  }
}

TEST_F(FileParametersTest,
       ExistingAndMissingTargetsUnderEscapingSymlinkAreRejected) {
  const auto outside = directory_ / "outside";
  std::filesystem::create_directory(outside);
  std::ofstream(outside / "model.bin") << "neutral resource";
  std::filesystem::create_directory_symlink(outside, root_ / "linked");
  for (const char* file : {"linked/model.bin", "linked/missing.bin"}) {
    auto source = FileDocument();
    source["models"][0]["file"] = file;
    Json resolved;
    std::string error;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(ResolveModelFiles(source, root_.string(), &resolved, &error,
                                   &diagnostic));
    EXPECT_TRUE(resolved.is_null());
    EXPECT_EQ(diagnostic.path, "/models/0/file");
    EXPECT_NE(error.find("escapes"), std::string::npos);
  }
}

TEST_F(FileParametersTest,
       UnknownSelectionsAndStructuralErrorsRemainForCoreToDiagnose) {
  auto source = FileDocument();
  source["models"][0]["backend"]["type"] =
      "unknown_backend_for_file_resolution";
  source["models"][0]["file"] = "../not-resolved.bin";
  Json resolved;
  std::string error;
  ASSERT_TRUE(ResolveModelFiles(source, root_.string(), &resolved, &error));
  EXPECT_EQ(resolved, source);
  source = FileDocument();
  source["models"][0]["file"] = 123;
  ASSERT_TRUE(ResolveModelFiles(source, root_.string(), &resolved, &error));
  EXPECT_EQ(resolved, source);
  source["models"][0]["file"] = "weights/model.bin";
  source["models"][0]["params"]["token/file~"] = 123;
  ASSERT_TRUE(ResolveModelFiles(source, root_.string(), &resolved, &error));
  EXPECT_EQ(resolved["models"][0]["params"]["token/file~"], 123);
  EXPECT_FALSE(ResolveModelFiles(source, root_.string(), nullptr, &error));
}

}  // namespace
}  // namespace llm_edgeflow
