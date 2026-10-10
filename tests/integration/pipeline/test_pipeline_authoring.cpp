#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "cli/pipeline_authoring.h"
#include "cli/pipeline_document_validation.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

class ScopedIoState {
 public:
  ScopedIoState()
      : inputs_(IoConverterRegistry::Instance().AllInputConverters()),
        outputs_(IoConverterRegistry::Instance().AllOutputConverters()) {}
  ~ScopedIoState() {
    auto& registry = IoConverterRegistry::Instance();
    registry.ClearForTesting();
    for (const auto& definition : inputs_)
      EXPECT_TRUE(registry.RegisterInputConverter(definition));
    for (const auto& definition : outputs_)
      EXPECT_TRUE(registry.RegisterOutputConverter(definition));
  }
  ScopedIoState(const ScopedIoState&) = delete;
  ScopedIoState& operator=(const ScopedIoState&) = delete;

 private:
  std::vector<InputConverterDefinition> inputs_;
  std::vector<OutputConverterDefinition> outputs_;
};

Json Document(const std::vector<std::string>& output_names = {"doc_qa"}) {
  Json outputs = Json::array();
  for (const auto& name : output_names)
    outputs.push_back({{"type", "doc_out"},
                       {"name", name},
                       {"inputs",
                        {{"answer_text", "answer.text"},
                         {"intent", "intent.matches"},
                         {"chunk_count", "chunks.chunk_counts"}}}});
  return {{"io",
           {{"input", {{{"type", "doc_in"}, {"name", "doc_qa"}}}},
            {"output", std::move(outputs)}}},
          {"models", Json::array()},
          {"pipeline",
           {{{"type", "text_chunk"},
             {"name", "chunks"},
             {"inputs", {{"text", "input.doc_text"}}}},
            {{"type", "text_rule_match"},
             {"name", "intent"},
             {"inputs", {{"text", "input.query_text"}}}},
            {{"type", "text_template"},
             {"name", "answer"},
             {"inputs", {{"primary", "input.query_text"}}}},
            {{"type", "text_template"},
             {"name", "decorate"},
             {"inputs", {{"primary", "answer.text"}}},
             {"depends_on", {"answer"}}}}}};
}

Json Node(const Json& document, const std::string& name) {
  for (const auto& node : document.at("pipeline"))
    if (node.at("name") == name) return node;
  throw std::out_of_range("Missing node: " + name);
}

void RegisterDocOutputs() {
  auto& registry = IoConverterRegistry::Instance();
  const auto* original = registry.FindOutputConverter("doc_out", "doc_qa");
  ASSERT_NE(original, nullptr);
  const auto prototype = *original;
  for (const auto& [name, service] : std::vector<std::pair<std::string, int>>{
           {"keyword_match", kMockServiceKeywordMatch},
           {"entity_extract", kMockServiceEntityExtract}}) {
    auto definition = prototype;
    definition.name = name;
    definition.service_type = service;
    ASSERT_TRUE(registry.RegisterOutputConverter(definition));
  }
}

bool HasDiagnostic(const Json& validation, const std::string& code) {
  const auto& diagnostics = validation.at("diagnostics");
  return std::any_of(
      diagnostics.begin(), diagnostics.end(),
      [&](const auto& diagnostic) { return diagnostic.at("code") == code; });
}
}  // namespace

TEST(PipelineAuthoringTest, RenameRewritesNodesOutputsAndOrderingReferences) {
  ScopedIoState scope;
  RegisterDocOutputs();
  const auto original = Document({"keyword_match", "entity_extract"});
  ASSERT_TRUE(
      ValidatePipelineDocument(original, DocumentValidationMode::kValidate).ok);
  const Json request = {{"pipeline", original},
                        {"require_valid", true},
                        {"operation",
                         {{"kind", "rename_node"},
                          {"node", "answer"},
                          {"new_name", "prepare_answer"}}}};
  const auto before = request.dump();
  const auto result = PipelineAuthoring::ApplyRequest(request);
  ASSERT_TRUE(result.ok) << result.ToJson().dump(2);
  ASSERT_TRUE(result.pipeline.has_value());
  EXPECT_EQ(request.dump(), before);
  EXPECT_EQ(Node(*result.pipeline, "prepare_answer").at("inputs"),
            Node(original, "answer").at("inputs"));
  const auto& consumer = Node(*result.pipeline, "decorate");
  EXPECT_EQ(consumer.at("inputs").at("primary"), "prepare_answer.text");
  EXPECT_EQ(consumer.at("depends_on"), Json::array({"prepare_answer"}));
  for (const auto& output : result.pipeline->at("io").at("output")) {
    EXPECT_EQ(output.at("inputs").at("answer_text"), "prepare_answer.text");
    EXPECT_EQ(output.at("inputs").at("intent"), "intent.matches");
    EXPECT_EQ(output.at("inputs").at("chunk_count"), "chunks.chunk_counts");
  }
  EXPECT_TRUE(result.validation.at("ok").get<bool>());
}

TEST(PipelineAuthoringTest,
     RemoveClearsAllReferencesAndKeepsUnrelatedBindings) {
  ScopedIoState scope;
  RegisterDocOutputs();
  const auto original = Document({"keyword_match", "entity_extract"});
  ASSERT_TRUE(
      ValidatePipelineDocument(original, DocumentValidationMode::kValidate).ok);
  const Json request = {
      {"pipeline", original},
      {"require_valid", false},
      {"operation", {{"kind", "remove_node"}, {"node", "answer"}}}};
  const auto before = request.dump();
  const auto result = PipelineAuthoring::ApplyRequest(request);
  ASSERT_TRUE(result.ok) << result.ToJson().dump(2);
  ASSERT_TRUE(result.pipeline.has_value());
  EXPECT_EQ(request.dump(), before);
  EXPECT_EQ(result.pipeline->at("pipeline").size(),
            original.at("pipeline").size() - 1);
  const auto& consumer = Node(*result.pipeline, "decorate");
  EXPECT_FALSE(consumer.at("inputs").contains("primary"));
  EXPECT_TRUE(consumer.at("depends_on").empty());
  EXPECT_EQ(Node(*result.pipeline, "chunks"), Node(original, "chunks"));
  EXPECT_EQ(Node(*result.pipeline, "intent"), Node(original, "intent"));
  for (const auto& output : result.pipeline->at("io").at("output")) {
    EXPECT_FALSE(output.at("inputs").contains("answer_text"));
    EXPECT_EQ(output.at("inputs").at("intent"), "intent.matches");
    EXPECT_EQ(output.at("inputs").at("chunk_count"), "chunks.chunk_counts");
  }
  EXPECT_FALSE(result.validation.at("ok").get<bool>());
  EXPECT_TRUE(HasDiagnostic(result.validation, "MISSING_OUTPUT_PRODUCER"));
}

TEST(PipelineAuthoringTest,
     SharedOutputPortIsAmbiguousForConnectAndDisconnect) {
  ScopedIoState scope;
  RegisterDocOutputs();
  const auto original = Document({"keyword_match", "entity_extract"});
  ASSERT_TRUE(
      ValidatePipelineDocument(original, DocumentValidationMode::kValidate).ok);
  for (const char* kind : {"connect", "disconnect"}) {
    SCOPED_TRACE(kind);
    const Json request = {
        {"pipeline", original},
        {"require_valid", true},
        {"operation",
         {{"kind", kind},
          {"source", {{"node", "answer"}, {"port", "text"}}},
          {"target", {{"node", "output"}, {"port", "answer_text"}}}}}};
    const auto before = request.dump();
    const auto result = PipelineAuthoring::ApplyRequest(request);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.pipeline.has_value());
    EXPECT_FALSE(result.ToJson().contains("pipeline"));
    EXPECT_EQ(request.dump(), before);
    ASSERT_EQ(result.failed_operation_index, 0U);
    ASSERT_FALSE(result.diagnostics.empty());
    for (const char* fragment : {"AMBIGUOUS_OUTPUT_PORT", "output.answer_text",
                                 "doc_out", "keyword_match", "entity_extract"})
      EXPECT_NE(result.diagnostics.front().find(fragment), std::string::npos);
  }
}

TEST(PipelineAuthoringTest,
     FailedLaterOperationReturnsNoPartiallyEditedDocument) {
  const Json request = {
      {"pipeline", Document()},
      {"operations",
       {{{"kind", "rename_node"},
         {"node", "answer"},
         {"new_name", "prepare_answer"}},
        {{"kind", "connect"},
         {"source", {{"node", "missing_source"}, {"port", "text"}}},
         {"target", {{"node", "decorate"}, {"port", "primary"}}}}}}};
  const auto before = request.dump();
  const auto result = PipelineAuthoring::ApplyRequest(request);
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.pipeline.has_value());
  EXPECT_FALSE(result.ToJson().contains("pipeline"));
  ASSERT_EQ(result.failed_operation_index, 1U);
  ASSERT_FALSE(result.diagnostics.empty());
  EXPECT_NE(result.diagnostics.front().find("NODE_NOT_FOUND"),
            std::string::npos);
  EXPECT_EQ(request.dump(), before);
  EXPECT_EQ(Node(request.at("pipeline"), "decorate").at("inputs").at("primary"),
            "answer.text");
}

TEST(PipelineAuthoringTest,
     RequireValidRejectsMissingOutputWithoutReturningDraft) {
  const Json request = {
      {"pipeline", Document()},
      {"require_valid", true},
      {"operation", {{"kind", "remove_node"}, {"node", "answer"}}}};
  const auto before = request.dump();
  const auto result = PipelineAuthoring::ApplyRequest(request);
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.pipeline.has_value());
  EXPECT_FALSE(result.validation.at("ok").get<bool>());
  EXPECT_TRUE(HasDiagnostic(result.validation, "MISSING_OUTPUT_PRODUCER"));
  EXPECT_EQ(request.dump(), before);
}

TEST(PipelineAuthoringTest, NativeValidationFindsCycleIntroducedByReferences) {
  const auto original = Document();
  ASSERT_TRUE(
      ValidatePipelineDocument(original, DocumentValidationMode::kValidate).ok);
  auto cyclic = original;
  cyclic["pipeline"][2]["inputs"]["primary"] = "decorate.text";
  const auto native =
      ValidatePipelineDocument(cyclic, DocumentValidationMode::kValidate);
  EXPECT_FALSE(native.ok);
  EXPECT_TRUE(HasDiagnostic(native.response, "DAG_CYCLE"));
  const std::vector<Json> operations = {
      {{"kind", "connect"},
       {"source", {{"node", "decorate"}, {"port", "text"}}},
       {"target", {{"node", "answer"}, {"port", "primary"}}}},
      {{"kind", "add_dependency"},
       {"node", "answer"},
       {"depends_on", "decorate"}}};
  for (const auto& operation : operations) {
    SCOPED_TRACE(operation.dump());
    for (const bool require_valid : {true, false}) {
      SCOPED_TRACE(require_valid);
      const Json request = {{"pipeline", original},
                            {"require_valid", require_valid},
                            {"operation", operation}};
      const auto before = request.dump();
      const auto rejected = PipelineAuthoring::ApplyRequest(request);
      EXPECT_FALSE(rejected.ok);
      EXPECT_FALSE(rejected.pipeline.has_value());
      EXPECT_FALSE(rejected.ToJson().contains("pipeline"));
      EXPECT_EQ(rejected.failed_operation_index, 0U);
      ASSERT_FALSE(rejected.diagnostics.empty());
      EXPECT_NE(rejected.diagnostics.front().find("DAG_CYCLE"),
                std::string::npos);
      EXPECT_EQ(request.dump(), before);
    }
  }
}

TEST(PipelineAuthoringTest,
     AutomaticNamesAvoidCollisionsWithoutWritingDefaults) {
  const Json request = {{"pipeline", Document()},
                        {"operations",
                         {{{"kind", "add_node"}, {"type", "text_template"}},
                          {{"kind", "add_node"}, {"type", "text_template"}}}}};
  const auto before = request.dump();
  const auto result = PipelineAuthoring::ApplyRequest(request);
  ASSERT_TRUE(result.ok) << result.ToJson().dump(2);
  ASSERT_TRUE(result.pipeline.has_value());
  for (const char* name : {"text_template", "text_template_2"}) {
    const auto& added = Node(*result.pipeline, name);
    EXPECT_EQ(added.at("type"), "text_template");
    EXPECT_FALSE(added.contains("params"));
    EXPECT_FALSE(added.contains("inputs"));
  }
  EXPECT_EQ(request.dump(), before);
}
}  // namespace llm_edgeflow
