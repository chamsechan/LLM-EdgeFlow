#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#include "adapter/model_file_resolver.h"
#include "contracts/config_schema.h"
#include "contracts/config_schema_validation.h"
#include "contracts/parameters.h"
#include "contracts/path_utils.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {
namespace fs = std::filesystem;

struct FileTestDirectory {
  fs::path root =
      fs::temp_directory_path() /
      ("edgeflow-file-params-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  FileTestDirectory() { fs::create_directories(root / "pipeline"); }
  ~FileTestDirectory() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

struct ModelFileParams {
  std::string tokenizer_file;
  std::string note;
};
struct BackendFileParams {
  std::optional<std::string> run_config_file;
};

bool RegisterFileFixtures() {
  auto& models = ModelRegistry::Instance();
  auto& backends = BackendRegistry::Instance();
  if (!backends.Has("file_parameter_backend")) {
    BackendDefinition backend;
    backend.backend_type = "file_parameter_backend";
    backend.supported_protocols = {ExecutionProtocol::kFixture};
    backend.params = Parameters<BackendFileParams>{
        Field("run_config_file", &BackendFileParams::run_config_file).File()};
    if (!backends.Register(backend, [] { return nullptr; })) return false;
  }
  if (!models.Has("file_parameter_model")) {
    ModelDefinition model;
    model.impl_name = "file_parameter_model";
    model.model_type = "file_parameter_type";
    model.required_protocol = ExecutionProtocol::kFixture;
    model.fixture_backends = {"file_parameter_backend"};
    model.params = Parameters<ModelFileParams>{
        Field("tokenizer_file", &ModelFileParams::tokenizer_file)
            .Required()
            .File(),
        Field("note", &ModelFileParams::note).Default("")};
    if (!models.Register(model, [](const auto&, auto*) { return nullptr; }))
      return false;
  }
  return true;
}

nlohmann::json FileDocument() {
  return {{"models",
           {{{"type", "file_parameter_type"},
             {"name", "encoder"},
             {"file", "weights/missing.bin"},
             {"params",
              {{"tokenizer_file", "vocab/missing.txt"},
               {"note", "../literal-text"}}},
             {"backend",
              {{"type", "file_parameter_backend"},
               {"params", {{"run_config_file", "run/config.json"}}}}}}}}};
}
}  // namespace

TEST(ConverterContractsTest,
     RelativeFilesRejectPortableEscapeAndAllowMissingFiles) {
  FileTestDirectory directory;
  const auto base = directory.root / "pipeline";
  fs::path resolved;
  std::string error;
  for (const std::string& invalid :
       {std::string(), std::string("/absolute.bin"),
        std::string("C:\\absolute.bin"), std::string("C:relative.bin"),
        std::string("\\\\server\\share\\model.bin"),
        std::string("../model.bin"), std::string("nested/../model.bin"),
        std::string("nested\\..\\model.bin"), std::string("nul\0file", 8)}) {
    SCOPED_TRACE(invalid);
    EXPECT_FALSE(ResolveFileUnderDirectory(base, invalid, &resolved, &error));
    EXPECT_TRUE(resolved.empty());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(ResolveFileUnderDirectory({}, invalid, &resolved, &error));
  }
  ASSERT_TRUE(
      ResolveFileUnderDirectory(base, "nested/missing.bin", &resolved, &error))
      << error;
  EXPECT_EQ(resolved, fs::weakly_canonical(base / "nested/missing.bin"));
  EXPECT_FALSE(fs::exists(resolved));
  ASSERT_TRUE(
      ResolveFileUnderDirectory(base, "..name/missing.bin", &resolved, &error))
      << error;
  EXPECT_EQ(resolved, fs::weakly_canonical(base / "..name/missing.bin"));
  ASSERT_TRUE(
      ResolveFileUnderDirectory({}, "nested\\missing.bin", &resolved, &error));
  EXPECT_EQ(resolved, fs::path("nested/missing.bin"));
  fs::create_directories(directory.root / "outside");
  fs::create_directory_symlink(directory.root / "outside", base / "escape");
  EXPECT_FALSE(
      ResolveFileUnderDirectory(base, "escape/missing.bin", &resolved, &error));
  EXPECT_TRUE(resolved.empty());
}

TEST(ConverterContractsTest,
     FileMetadataResolvesOnlySelectedFileFieldsAtomically) {
  ASSERT_TRUE(RegisterFileFixtures());
  FileTestDirectory directory;
  const auto base = directory.root / "pipeline";
  const auto document = FileDocument();
  const auto original = document;
  nlohmann::json resolved;
  std::string error;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(ResolveModelFiles(document, base.string(), &resolved, &error,
                                &diagnostic))
      << error;
  const auto& model = resolved["models"][0];
  EXPECT_EQ(model["file"], (base / "weights/missing.bin").string());
  EXPECT_EQ(model["params"]["tokenizer_file"],
            (base / "vocab/missing.txt").string());
  EXPECT_EQ(model["backend"]["params"]["run_config_file"],
            (base / "run/config.json").string());
  EXPECT_EQ(model["params"]["note"], "../literal-text");
  EXPECT_EQ(document, original);
  ASSERT_TRUE(ResolveModelFiles(document, "", &resolved, &error, &diagnostic));
  EXPECT_EQ(resolved, original);
  for (const auto& path : {"/models/0/file", "/models/0/params/tokenizer_file",
                           "/models/0/backend/params/run_config_file"}) {
    auto invalid = document;
    invalid[nlohmann::json::json_pointer(path)] = "../escape";
    EXPECT_FALSE(ResolveModelFiles(invalid, base.string(), &resolved, &error,
                                   &diagnostic));
    EXPECT_EQ(diagnostic.code, "INVALID_FILE_PATH");
    EXPECT_EQ(diagnostic.path, path);
    EXPECT_TRUE(resolved.is_null());
    EXPECT_EQ(invalid[nlohmann::json::json_pointer(path)], "../escape");
  }
  auto unknown = document;
  unknown["models"][0]["type"] = "not_registered";
  unknown["models"][0]["file"] = "../defer-to-core";
  ASSERT_TRUE(ResolveModelFiles(unknown, base.string(), &resolved, &error,
                                &diagnostic));
  EXPECT_EQ(resolved, unknown);
  auto malformed = document;
  malformed["models"][0].erase("name");
  malformed["models"][0]["file"] = "../defer-to-core";
  ASSERT_TRUE(ResolveModelFiles(malformed, base.string(), &resolved, &error,
                                &diagnostic));
  EXPECT_EQ(resolved, malformed);
  for (const char* path : {"/models/0/params", "/models/0/backend/params"}) {
    malformed = document;
    malformed[nlohmann::json::json_pointer(path)] = false;
    malformed["models"][0]["file"] = "../defer-to-core";
    ASSERT_TRUE(ResolveModelFiles(malformed, base.string(), &resolved, &error,
                                  &diagnostic));
    EXPECT_EQ(resolved, malformed);
  }
}

TEST(ConverterContractsTest, FileMetadataIsExportedAndStringOnly) {
  ASSERT_TRUE(RegisterFileFixtures());
  const auto model = ModelRegistry::Instance().Find("file_parameter_model");
  const auto backend =
      BackendRegistry::Instance().Find("file_parameter_backend");
  ASSERT_TRUE(model);
  ASSERT_TRUE(backend);
  const auto model_fields = model->params.Fields();
  const auto backend_fields = backend->params.Fields();
  ASSERT_EQ(model_fields.size(), 2U);
  EXPECT_TRUE(ConfigFieldToJson(model_fields[0])["file"].get<bool>());
  EXPECT_TRUE(ConfigFieldJsonSchema(model_fields[0])["file"].get<bool>());
  EXPECT_FALSE(ConfigFieldToJson(model_fields[1]).contains("file"));
  ASSERT_EQ(backend_fields.size(), 1U);
  EXPECT_FALSE(backend_fields[0].required);
  EXPECT_TRUE(backend_fields[0].default_value.is_null());
  auto invalid = model_fields[0];
  invalid.kind = ConfigValueKind::kInteger;
  std::string error;
  EXPECT_FALSE(ValidateConfigFieldDefinitions({invalid}, &error));
}
}  // namespace llm_edgeflow
