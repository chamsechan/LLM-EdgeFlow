#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/pipeline_config.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

Json ParsingExample() {
  return {{"comment", "parser fixture"},
          {"models",
           {{{"type", "language"},
             {"name", "generator"},
             {"file", "models/nonexistent.bin"},
             {"backend", {{"type", "runtime"}}}}}},
          {"pipeline",
           {{{"type", "transform"},
             {"name", "first"},
             {"inputs", {{"text", "input.text"}}}},
            {{"type", "transform"},
             {"name", "second"},
             {"params", {{"limit", 7}}},
             {"inputs", {{"text", "first.text"}}},
             {"depends_on", {"first"}}}}}};
}

TEST(PipelineParsingTest, PreservesExplicitValuesAndDefaultsWithSourceIndices) {
  ParsedPipelineConfig parsed;
  PipelineDiagnostic diagnostic;
  const auto root = ParsingExample();
  ASSERT_TRUE(ParsePipelineConfig(root, &parsed, &diagnostic));
  EXPECT_EQ(diagnostic.code, error::kOk);
  EXPECT_EQ(parsed.max_parallel_workers, 1u);
  ASSERT_EQ(parsed.models.size(), 1u);
  const auto& model = parsed.models.front();
  EXPECT_EQ(model.model_name, "generator");
  EXPECT_EQ(model.model_type, "language");
  EXPECT_EQ(model.backend_type, "runtime");
  EXPECT_EQ(model.model_file, "models/nonexistent.bin");
  EXPECT_EQ(model.model_params, Json::object());
  EXPECT_EQ(model.backend_params, Json::object());
  EXPECT_EQ(model.source_index, 0u);
  ASSERT_EQ(parsed.nodes.size(), 2u);
  EXPECT_EQ(parsed.nodes[0].name, "first");
  EXPECT_EQ(parsed.nodes[0].node_type, "transform");
  EXPECT_EQ(parsed.nodes[0].source_index, 0u);
  EXPECT_TRUE(parsed.nodes[0].depends_on.empty());
  EXPECT_EQ(parsed.nodes[0].params, Json::object());
  EXPECT_EQ(parsed.nodes[0].ports.inputs.at("text"), "input.text");
  EXPECT_EQ(parsed.nodes[1].source_index, 1u);
  EXPECT_EQ(parsed.nodes[1].params, Json({{"limit", 7}}));
  EXPECT_EQ(parsed.nodes[1].ports.inputs.at("text"), "first.text");
  EXPECT_EQ(parsed.nodes[1].depends_on, std::vector<std::string>{"first"});
  EXPECT_EQ(root, ParsingExample());
}

TEST(PipelineParsingTest, SuccessfulParseReplacesTheWholePreviousResult) {
  ParsedPipelineConfig parsed;
  PipelineDiagnostic diagnostic;
  ASSERT_TRUE(ParsePipelineConfig(ParsingExample(), &parsed, &diagnostic));
  diagnostic = {error::kFieldRange, "/stale", "stale"};
  ASSERT_TRUE(ParsePipelineConfig(
      {{"max_parallel_workers", 64},
       {"pipeline", {{{"type", "source"}, {"name", "replacement"}}}}},
      &parsed, &diagnostic));
  EXPECT_EQ(parsed.max_parallel_workers, 64u);
  EXPECT_TRUE(parsed.models.empty());
  ASSERT_EQ(parsed.nodes.size(), 1u);
  EXPECT_EQ(parsed.nodes[0].name, "replacement");
  EXPECT_TRUE(parsed.nodes[0].ports.inputs.empty());
  EXPECT_EQ(diagnostic.code, error::kOk);
  EXPECT_TRUE(diagnostic.path.empty());
  EXPECT_TRUE(diagnostic.message.empty());
}

struct InvalidStructure {
  const char* name;
  const char* operation;
  const char* changed_path;
  Json value;
  int code;
  const char* path;
};

TEST(PipelineParsingTest, NamedStructuralChangesFailAtTheirOwnedField) {
  const std::vector<InvalidStructure> cases = {
      {"root is array", "replace", "", Json::array(), error::kRootType, "/"},
      {"unknown root field", "add", "/legacy", true, error::kUnknownField,
       "/legacy"},
      {"comment type", "replace", "/comment", 1, error::kFieldType, "/comment"},
      {"worker type", "add", "/max_parallel_workers", 1.5, error::kFieldType,
       "/max_parallel_workers"},
      {"worker below minimum", "add", "/max_parallel_workers", 0,
       error::kFieldRange, "/max_parallel_workers"},
      {"worker above maximum", "add", "/max_parallel_workers", 65,
       error::kFieldRange, "/max_parallel_workers"},
      {"models container", "replace", "/models", Json::object(),
       error::kFieldType, "/models"},
      {"model item", "replace", "/models/0", 1, error::kFieldType, "/models/0"},
      {"unknown model field", "add", "/models/0/legacy", true,
       error::kUnknownField, "/models/0/legacy"},
      {"missing model category", "remove", "/models/0/type", nullptr,
       error::kMissingField, "/models/0/type"},
      {"missing model name", "remove", "/models/0/name", nullptr,
       error::kMissingField, "/models/0/name"},
      {"missing model file", "remove", "/models/0/file", nullptr,
       error::kMissingField, "/models/0/file"},
      {"empty model file", "replace", "/models/0/file", "", error::kFieldRange,
       "/models/0/file"},
      {"model file type", "replace", "/models/0/file", false, error::kFieldType,
       "/models/0/file"},
      {"empty model name", "replace", "/models/0/name", "", error::kFieldRange,
       "/models/0/name"},
      {"missing backend", "remove", "/models/0/backend", nullptr,
       error::kMissingField, "/models/0/backend"},
      {"backend container", "replace", "/models/0/backend", Json::array(),
       error::kFieldType, "/models/0/backend"},
      {"missing backend type", "remove", "/models/0/backend/type", nullptr,
       error::kMissingField, "/models/0/backend/type"},
      {"empty backend type", "replace", "/models/0/backend/type", "",
       error::kFieldRange, "/models/0/backend/type"},
      {"unknown backend field", "add", "/models/0/backend/legacy", 1,
       error::kUnknownField, "/models/0/backend/legacy"},
      {"model params container", "add", "/models/0/params", Json::array(),
       error::kFieldType, "/models/0/params"},
      {"backend params container", "add", "/models/0/backend/params", 2,
       error::kFieldType, "/models/0/backend/params"},
      {"missing pipeline", "remove", "/pipeline", nullptr, error::kMissingField,
       "/pipeline"},
      {"pipeline container", "replace", "/pipeline", Json::object(),
       error::kFieldType, "/pipeline"},
      {"empty pipeline", "replace", "/pipeline", Json::array(),
       error::kFieldRange, "/pipeline"},
      {"node item", "replace", "/pipeline/1", "node", error::kFieldType,
       "/pipeline/1"},
      {"unknown node field", "add", "/pipeline/1/outputs", Json::object(),
       error::kUnknownField, "/pipeline/1/outputs"},
      {"missing node name", "remove", "/pipeline/1/name", nullptr,
       error::kMissingField, "/pipeline/1/name"},
      {"missing node type", "remove", "/pipeline/1/type", nullptr,
       error::kMissingField, "/pipeline/1/type"},
      {"empty node type", "replace", "/pipeline/1/type", "", error::kFieldRange,
       "/pipeline/1/type"},
      {"node name type", "replace", "/pipeline/1/name", 3, error::kFieldType,
       "/pipeline/1/name"},
      {"empty node name", "replace", "/pipeline/1/name", "", error::kFieldRange,
       "/pipeline/1/name"},
      {"reserved input name", "replace", "/pipeline/1/name", "input",
       error::kInvalidNodeName, "/pipeline/1/name"},
      {"reserved output name", "replace", "/pipeline/1/name", "output",
       error::kInvalidNodeName, "/pipeline/1/name"},
      {"dotted node name", "replace", "/pipeline/1/name", "a.b",
       error::kInvalidNodeName, "/pipeline/1/name"},
      {"duplicate node name", "replace", "/pipeline/1/name", "first",
       error::kDuplicateNodeName, "/pipeline/1/name"},
      {"node params container", "replace", "/pipeline/1/params", false,
       error::kFieldType, "/pipeline/1/params"},
      {"inputs container", "replace", "/pipeline/1/inputs", Json::array(),
       error::kFieldType, "/pipeline/1/inputs"},
      {"input reference type", "replace", "/pipeline/1/inputs/text", 4,
       error::kFieldType, "/pipeline/1/inputs/text"},
      {"dependencies container", "replace", "/pipeline/1/depends_on", "first",
       error::kFieldType, "/pipeline/1/depends_on"},
      {"dependency item type", "replace", "/pipeline/1/depends_on/0", 5,
       error::kFieldType, "/pipeline/1/depends_on/0"},
      {"empty dependency", "replace", "/pipeline/1/depends_on/0", "",
       error::kFieldRange, "/pipeline/1/depends_on/0"},
  };
  for (const auto& example : cases) {
    SCOPED_TRACE(example.name);
    ParsedPipelineConfig parsed;
    ASSERT_TRUE(ParsePipelineConfig(ParsingExample(), &parsed));
    Json operation = {{"op", example.operation},
                      {"path", example.changed_path}};
    if (std::string(example.operation) != "remove")
      operation["value"] = example.value;
    const auto invalid = ParsingExample().patch(Json::array({operation}));
    PipelineDiagnostic diagnostic;
    EXPECT_FALSE(ParsePipelineConfig(invalid, &parsed, &diagnostic));
    EXPECT_EQ(diagnostic.code, example.code) << diagnostic.message;
    EXPECT_EQ(diagnostic.path, example.path);
    // 前面的条目已解析后再失败时，不得发布部分状态。
    EXPECT_EQ(parsed.max_parallel_workers, 1u);
    ASSERT_EQ(parsed.models.size(), 1u);
    EXPECT_EQ(parsed.models[0].model_name, "generator");
    ASSERT_EQ(parsed.nodes.size(), 2u);
    EXPECT_EQ(parsed.nodes[1].name, "second");
    EXPECT_EQ(parsed.nodes[1].params, Json({{"limit", 7}}));
  }
}

TEST(PipelineParsingTest, AcceptsMaximumArraySizesAndRejectsTheNextEntry) {
  auto root = ParsingExample();
  root["models"] = Json::array();
  for (size_t i = 0; i < 64; ++i) {
    auto model = ParsingExample()["models"][0];
    model["name"] = "model_" + std::to_string(i);
    root["models"].push_back(model);
  }
  root["pipeline"] = Json::array();
  for (size_t i = 0; i < 256; ++i) {
    root["pipeline"].push_back(
        {{"type", "source"}, {"name", "node_" + std::to_string(i)}});
  }
  root["pipeline"][0]["depends_on"] = Json::array();
  for (size_t i = 0; i < 256; ++i)
    root["pipeline"][0]["depends_on"].push_back("dep_" + std::to_string(i));
  ParsedPipelineConfig parsed;
  ASSERT_TRUE(ParsePipelineConfig(root, &parsed));
  EXPECT_EQ(parsed.models.size(), 64u);
  EXPECT_EQ(parsed.nodes.size(), 256u);
  EXPECT_EQ(parsed.nodes[0].depends_on.size(), 256u);
  for (const auto* array_path :
       {"/models", "/pipeline", "/pipeline/0/depends_on"}) {
    SCOPED_TRACE(array_path);
    auto oversized = root;
    auto& array = oversized[Json::json_pointer(array_path)];
    array.push_back(array.back());
    PipelineDiagnostic diagnostic;
    EXPECT_FALSE(ParsePipelineConfig(oversized, &parsed, &diagnostic));
    EXPECT_EQ(diagnostic.code, error::kFieldRange);
    EXPECT_EQ(diagnostic.path, array_path);
    EXPECT_EQ(parsed.models.size(), 64u);
    EXPECT_EQ(parsed.nodes.size(), 256u);
  }
}

TEST(PipelineParsingTest,
     LateFailureDoesNotPublishAnySuccessfullyParsedPrefix) {
  ParsedPipelineConfig parsed;
  ASSERT_TRUE(ParsePipelineConfig(ParsingExample(), &parsed));
  auto root = ParsingExample();
  root["max_parallel_workers"] = 12;
  root["models"][0]["name"] = "new_model";
  root["models"][0]["params"] = {{"changed", true}};
  root["pipeline"][0]["name"] = "new_first";
  root["pipeline"][1]["depends_on"] = {false};
  PipelineDiagnostic diagnostic;
  ASSERT_FALSE(ParsePipelineConfig(root, &parsed, &diagnostic));
  EXPECT_EQ(diagnostic.code, error::kFieldType);
  EXPECT_EQ(diagnostic.path, "/pipeline/1/depends_on/0");
  EXPECT_EQ(parsed.max_parallel_workers, 1u);
  ASSERT_EQ(parsed.models.size(), 1u);
  EXPECT_EQ(parsed.models[0].model_name, "generator");
  EXPECT_EQ(parsed.models[0].model_params, Json::object());
  ASSERT_EQ(parsed.nodes.size(), 2u);
  EXPECT_EQ(parsed.nodes[0].name, "first");
  EXPECT_EQ(parsed.nodes[1].depends_on, std::vector<std::string>{"first"});
}

TEST(PipelineParsingTest, RejectsDuplicateModelInstanceNames) {
  auto root = ParsingExample();
  root["models"].push_back(root["models"][0]);
  ParsedPipelineConfig parsed;
  PipelineDiagnostic diagnostic;
  EXPECT_FALSE(ParsePipelineConfig(root, &parsed, &diagnostic));
  EXPECT_EQ(diagnostic.code, error::kDuplicateModelName);
  EXPECT_EQ(diagnostic.path, "/models/1/name");
}

TEST(PipelineParsingTest, PreservesOpaqueParametersAndUnresolvedFiles) {
  auto root = ParsingExample();
  root["models"][0]["file"] = "../unresolved/model.bin";
  root["models"][0]["params"] = {{"options", {{"nested", {1, 2}}}}};
  root["models"][0]["backend"]["params"] = {{"device", "example"}};
  ParsedPipelineConfig parsed;
  ASSERT_TRUE(ParsePipelineConfig(root, &parsed, nullptr));
  EXPECT_EQ(parsed.models[0].model_file, "../unresolved/model.bin");
  EXPECT_EQ(parsed.models[0].model_params, root["models"][0]["params"]);
  EXPECT_EQ(parsed.models[0].backend_params,
            root["models"][0]["backend"]["params"]);
}

TEST(PipelineParsingTest, ReportsNullDestinationAndAllowsNoDiagnostic) {
  PipelineDiagnostic diagnostic;
  EXPECT_FALSE(ParsePipelineConfig(ParsingExample(), nullptr, &diagnostic));
  EXPECT_EQ(diagnostic.code, error::kFieldType);
  EXPECT_EQ(diagnostic.path, "/");
  ParsedPipelineConfig parsed;
  EXPECT_FALSE(ParsePipelineConfig(Json::array(), &parsed));
}
}  // namespace
}  // namespace llm_edgeflow
