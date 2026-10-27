#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

#include "cli/pipeline_json_schema.h"
#include "contracts/config_schema_validation.h"
#include "core/node_definition_validation.h"
#include "core/pipeline_catalog.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

TEST(CatalogPublicationTest,
     NodeDefinitionsPublishTheSamePortsAndParameterSchemas) {
  const auto definitions = PipelineCatalog::Nodes();
  ASSERT_FALSE(definitions.empty());
  const auto schema = BuildPipelineJsonSchema();
  const auto& node_schema = schema["properties"]["pipeline"]["items"];
  const auto registered =
      node_schema["properties"]["type"]["enum"].get<std::set<std::string>>();
  std::set<std::string> expected;
  for (const auto& definition : definitions) {
    SCOPED_TRACE(definition.node_type);
    std::string detail;
    ASSERT_TRUE(ValidateNodeDefinitionStructure(definition, &detail)) << detail;
    expected.insert(definition.node_type);
    const auto published = PipelineCatalog::NodeToJson(definition);
    EXPECT_EQ(published["node_type"], definition.node_type);
    ASSERT_EQ(published["inputs"].size(), definition.inputs.size());
    for (size_t index = 0; index < definition.inputs.size(); ++index) {
      EXPECT_EQ(published["inputs"][index]["key"],
                definition.inputs[index].logical_name);
      EXPECT_EQ(published["inputs"][index]["type_id"],
                definition.inputs[index].type_id);
    }
    const auto branch =
        std::find_if(node_schema["allOf"].begin(), node_schema["allOf"].end(),
                     [&](const auto& item) {
                       return item["if"]["properties"]["type"]["const"] ==
                              definition.node_type;
                     });
    ASSERT_NE(branch, node_schema["allOf"].end());
    const auto& params = (*branch)["then"]["properties"]["params"];
    EXPECT_EQ(params["properties"].size(), definition.config_fields.size());
    for (const auto& field : definition.config_fields) {
      EXPECT_EQ(params["properties"][field.name], ConfigFieldJsonSchema(field));
      const auto& required = params["required"];
      EXPECT_EQ(std::find(required.begin(), required.end(), field.name) !=
                    required.end(),
                field.required);
    }
  }
  EXPECT_EQ(registered, expected);
}

TEST(CatalogPublicationTest,
     BackendSetMatchesTheSelectedBuildAndModelBindingsUseItsProtocols) {
  std::set<std::string> expected;
#if EDGEFLOW_EXPECT_ONNXRUNTIME
  expected.insert("onnxruntime");
#endif
#if EDGEFLOW_EXPECT_LLAMACPP
  expected.insert("llama_cpp");
#endif
#if EDGEFLOW_EXPECT_WHISPERCPP
  expected.insert("whisper_cpp");
#endif
#if EDGEFLOW_EXPECT_KITELLM
  expected.insert("kite_llm");
#endif
#if EDGEFLOW_EXPECT_INFERLET
  expected.insert("inferlet");
#endif
  const auto backends = PipelineCatalog::Backends();
  std::set<std::string> actual;
  for (const auto& backend : backends) {
    actual.insert(backend.backend_type);
    EXPECT_EQ(std::count(backend.supported_protocols.begin(),
                         backend.supported_protocols.end(),
                         ExecutionProtocol::kFixture),
              0);
    EXPECT_EQ(PipelineCatalog::BackendToJson(backend)["backend_type"],
              backend.backend_type);
  }
  EXPECT_EQ(actual, expected);
  for (const auto& model : PipelineCatalog::Models()) {
    SCOPED_TRACE(model.impl_name);
    const auto published = PipelineCatalog::ModelToJson(model);
    EXPECT_EQ(published["required_protocol"],
              ExecutionProtocolName(model.required_protocol));
    for (const auto& name : published["backends"]) {
      const auto backend =
          PipelineCatalog::FindBackend(name.get<std::string>());
      ASSERT_TRUE(backend.has_value());
      const auto compatible = std::find(backend->supported_protocols.begin(),
                                        backend->supported_protocols.end(),
                                        model.required_protocol);
      EXPECT_NE(compatible, backend->supported_protocols.end());
      const auto implementations = ModelRegistry::Instance().FindImplementation(
          model.model_type, backend->backend_type);
      ASSERT_EQ(implementations.size(), 1u);
      EXPECT_EQ(implementations.front().impl_name, model.impl_name);
    }
  }
}
}  // namespace
}  // namespace llm_edgeflow
