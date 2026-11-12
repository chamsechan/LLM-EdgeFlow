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

TEST(PipelineParsingTest,
     SuccessfulParsesPreserveFieldsAndReplacePreviousState) {
  ParsedPipelineConfig parsed;
  PipelineDiagnostic diagnostic;
  auto root = ParsingExample();
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
  root["models"][0]["file"] = "../unresolved/model.bin";
  root["models"][0]["params"] = {{"options", {{"nested", {1, 2}}}}};
  root["models"][0]["backend"]["params"] = {{"device", "example"}};
  ASSERT_TRUE(ParsePipelineConfig(root, &parsed, nullptr));
  EXPECT_EQ(parsed.models[0].model_file, "../unresolved/model.bin");
  EXPECT_EQ(parsed.models[0].model_params, root["models"][0]["params"]);
  EXPECT_EQ(parsed.models[0].backend_params,
            root["models"][0]["backend"]["params"]);
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

TEST(PipelineParsingTest, StructuralErrorsIdentifyTheFirstInvalidField) {
  auto reject = [](const Json& input, int code, const std::string& path) {
    SCOPED_TRACE(input.dump());
    ParsedPipelineConfig parsed;
    ASSERT_TRUE(ParsePipelineConfig(ParsingExample(), &parsed));
    ASSERT_EQ(parsed.models.size(), 1u);
    ASSERT_EQ(parsed.nodes.size(), 2u);
    parsed.max_parallel_workers = 12;
    parsed.models[0].model_name = "previous_model";
    parsed.models[0].model_params = {{"retained", true}};
    parsed.nodes[0].name = "previous_node";
    PipelineDiagnostic diagnostic;
    EXPECT_FALSE(ParsePipelineConfig(input, &parsed, &diagnostic));
    EXPECT_EQ(diagnostic.code, code) << diagnostic.message;
    EXPECT_EQ(diagnostic.path, path);
    EXPECT_EQ(parsed.max_parallel_workers, 12u);
    ASSERT_EQ(parsed.models.size(), 1u);
    EXPECT_EQ(parsed.models[0].model_name, "previous_model");
    EXPECT_EQ(parsed.models[0].model_params, (Json{{"retained", true}}));
    ASSERT_EQ(parsed.nodes.size(), 2u);
    EXPECT_EQ(parsed.nodes[0].name, "previous_node");
    EXPECT_EQ(parsed.nodes[1].name, "second");
    EXPECT_EQ(parsed.nodes[1].params, (Json{{"limit", 7}}));
    EXPECT_EQ(parsed.nodes[1].depends_on, std::vector<std::string>{"first"});
  };
  struct InvalidField {
    const char* path;
    Json value;
    int code;
  };
  const InvalidField cases[] = {
      {"/a~1b~0c", true, error::kUnknownField},
      {"/", true, error::kUnknownField},
      {"/comment", 1, error::kFieldType},
      {"/max_parallel_workers", 1.5, error::kFieldType},
      {"/max_parallel_workers", 0, error::kFieldRange},
      {"/max_parallel_workers", 65, error::kFieldRange},
      {"/models", Json::object(), error::kFieldType},
      {"/models/0", 1, error::kFieldType},
      {"/models/0/a~1b~0c", true, error::kUnknownField},
      {"/models/0/", true, error::kUnknownField},
      {"/models/0/file", "", error::kFieldRange},
      {"/models/0/file", false, error::kFieldType},
      {"/models/0/name", "", error::kFieldRange},
      {"/models/0/backend", Json::array(), error::kFieldType},
      {"/models/0/backend/type", "", error::kFieldRange},
      {"/models/0/backend/a~1b~0c", true, error::kUnknownField},
      {"/models/0/backend/", true, error::kUnknownField},
      {"/models/0/params", Json::array(), error::kFieldType},
      {"/models/0/backend/params", 2, error::kFieldType},
      {"/pipeline", Json::object(), error::kFieldType},
      {"/pipeline", Json::array(), error::kFieldRange},
      {"/pipeline/1", "node", error::kFieldType},
      {"/pipeline/1/a~1b~0c", true, error::kUnknownField},
      {"/pipeline/1/", true, error::kUnknownField},
      {"/pipeline/1/type", "", error::kFieldRange},
      {"/pipeline/1/name", 3, error::kFieldType},
      {"/pipeline/1/name", "", error::kFieldRange},
      {"/pipeline/1/name", "input", error::kInvalidNodeName},
      {"/pipeline/1/name", "output", error::kInvalidNodeName},
      {"/pipeline/1/name", "a.b", error::kInvalidNodeName},
      {"/pipeline/1/name", "first", error::kDuplicateNodeName},
      {"/pipeline/1/params", false, error::kFieldType},
      {"/pipeline/1/inputs", Json::array(), error::kFieldType},
      {"/pipeline/1/inputs/text", 4, error::kFieldType},
      {"/pipeline/1/depends_on", "first", error::kFieldType},
      {"/pipeline/1/depends_on/0", 5, error::kFieldType},
      {"/pipeline/1/depends_on/0", "", error::kFieldRange},
  };
  for (const auto& example : cases) {
    auto input = ParsingExample();
    input[Json::json_pointer(example.path)] = example.value;
    reject(input, example.code, example.path);
  }
  for (const auto* field :
       {"/models/0/type", "/models/0/name", "/models/0/file",
        "/models/0/backend", "/models/0/backend/type", "/pipeline",
        "/pipeline/1/name", "/pipeline/1/type"}) {
    auto input = ParsingExample();
    const Json::json_pointer path(field);
    input[path.parent_pointer()].erase(path.back());
    reject(input, error::kMissingField, field);
  }
  reject(Json::array(), error::kRootType, "/");
  reject({{"comment", 42}}, error::kFieldType, "/comment");
  reject({{"pipeline", {{{"type", 7}}}}}, error::kFieldType,
         "/pipeline/0/type");
  auto duplicate = ParsingExample();
  duplicate["models"].push_back(duplicate["models"][0]);
  reject(duplicate, error::kDuplicateModelName, "/models/1/name");
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
