#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cli/pipeline_authoring.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

Json Document() {
  return {{"models", Json::array()},
          {"io",
           {{"input", {{{"type", "keyword_in"}, {"name", "keyword_match"}}}},
            {"output",
             {{{"type", "json_out"},
               {"name", "keyword_match"},
               {"inputs", {{"matches", "match.matches"}}}}}}}},
          {"pipeline",
           {{{"type", "text_template"},
             {"name", "render"},
             {"params", {{"template", "render.text {{primary}}"}}},
             {"inputs", {{"primary", "input.sentence_text"}}}},
            {{"type", "text_rule_match"},
             {"name", "match"},
             {"inputs", {{"text", "render.text"}}},
             {"depends_on", {"render"}}},
            {{"type", "text_template"},
             {"name", "render_more"},
             {"inputs", {{"primary", "render.text"}}},
             {"depends_on", {"match", "render"}}}}}};
}

AuthoringResult Edit(const Json& document, const Json& operations,
                     bool valid = false) {
  return PipelineAuthoring::ApplyRequest({{"pipeline", document},
                                          {"operations", operations},
                                          {"require_valid", valid}});
}
Json Edge(const std::string& kind, const std::string& source,
          const std::string& source_port, const std::string& target,
          const std::string& target_port) {
  return {{"kind", kind},
          {"source", {{"node", source}, {"port", source_port}}},
          {"target", {{"node", target}, {"port", target_port}}}};
}

TEST(PipelineEditingTest,
     AddUsesCatalogTypesAndUniqueNamesWithoutMaterializingDefaults) {
  const auto document = Document();
  const auto result = Edit(
      document, Json::array({{{"kind", "add_node"}, {"type", "text_template"}},
                             {{"kind", "add_node"},
                              {"type", "text_template"},
                              {"params", {{"template", "literal"}}}}}));
  ASSERT_TRUE(result.ok) << result.ToJson();
  ASSERT_TRUE(result.pipeline.has_value());
  const auto& nodes = (*result.pipeline)["pipeline"];
  ASSERT_EQ(nodes.size(), 5u);
  EXPECT_EQ(nodes[3]["name"], "text_template");
  EXPECT_FALSE(nodes[3].contains("params"));
  EXPECT_EQ(nodes[4]["name"], "text_template_2");
  EXPECT_EQ(nodes[4]["params"], Json({{"template", "literal"}}));
  EXPECT_EQ(document["pipeline"].size(), 3u);
  ASSERT_EQ(result.changes.size(), 2u);
  EXPECT_EQ(result.changes[0].action, "add_node");
  EXPECT_FALSE(result.validation["ok"].get<bool>());
  EXPECT_FALSE(result.failed_operation_index.has_value());
}

TEST(PipelineEditingTest,
     RenameRewritesDataOrderAndBoundaryReferencesButKeepsLiteralParams) {
  const auto document = Document();
  const auto result = Edit(document,
                           Json::array({{{"kind", "rename_node"},
                                         {"node", "render"},
                                         {"new_name", "rendered"}},
                                        {{"kind", "rename_node"},
                                         {"node", "match"},
                                         {"new_name", "matched"}}}),
                           true);
  ASSERT_TRUE(result.ok) << result.ToJson();
  ASSERT_TRUE(result.pipeline.has_value());
  const auto& candidate = *result.pipeline;
  EXPECT_EQ(candidate["pipeline"][1]["inputs"]["text"], "rendered.text");
  EXPECT_EQ(candidate["pipeline"][2]["inputs"]["primary"], "rendered.text");
  EXPECT_EQ(candidate["pipeline"][2]["depends_on"],
            Json::array({"matched", "rendered"}));
  EXPECT_EQ(candidate["io"]["output"][0]["inputs"]["matches"],
            "matched.matches");
  EXPECT_EQ(candidate["pipeline"][0]["params"]["template"],
            "render.text {{primary}}");
  EXPECT_EQ(candidate["pipeline"][2]["name"], "render_more");
  EXPECT_EQ(document["pipeline"][0]["name"], "render");
  EXPECT_TRUE(result.validation["ok"].get<bool>());
  EXPECT_EQ(result.changes[0].affected_nodes,
            (std::vector<std::string>{"rendered", "match", "render_more"}));
  EXPECT_EQ(result.changes[1].affected_nodes,
            (std::vector<std::string>{"matched", "render_more", "output"}));
}

TEST(PipelineEditingTest,
     RemoveClearsOnlyReferencesToItsNodeAndCanPublishAnInvalidDraft) {
  const auto result = Edit(
      Document(), Json::array({{{"kind", "remove_node"}, {"node", "match"}}}));
  ASSERT_TRUE(result.ok) << result.ToJson();
  ASSERT_TRUE(result.pipeline.has_value());
  const auto& candidate = *result.pipeline;
  ASSERT_EQ(candidate["pipeline"].size(), 2u);
  EXPECT_EQ(candidate["pipeline"][1]["inputs"]["primary"], "render.text");
  EXPECT_EQ(candidate["pipeline"][1]["depends_on"], Json::array({"render"}));
  EXPECT_TRUE(candidate["io"]["output"][0]["inputs"].empty());
  EXPECT_FALSE(result.validation["ok"].get<bool>());
  EXPECT_EQ(result.changes[0].affected_nodes,
            (std::vector<std::string>{"match", "render_more", "output"}));
  const auto strict =
      Edit(Document(),
           Json::array({{{"kind", "remove_node"}, {"node", "match"}}}), true);
  EXPECT_FALSE(strict.ok);
  EXPECT_FALSE(strict.pipeline.has_value());
  EXPECT_FALSE(strict.failed_operation_index.has_value());
  EXPECT_FALSE(strict.validation["ok"].get<bool>());
}

TEST(PipelineEditingTest,
     ConnectAndDisconnectResolveRealNodeIngressAndEgressPorts) {
  const auto result = Edit(
      Document(),
      Json::array({Edge("disconnect", "render", "text", "match", "text"),
                   Edge("connect", "input", "sentence_text", "match", "text"),
                   Edge("disconnect", "match", "matches", "output", "matches"),
                   Edge("connect", "match", "matches", "output", "matches")}),
      true);
  ASSERT_TRUE(result.ok) << result.ToJson();
  ASSERT_TRUE(result.pipeline.has_value());
  EXPECT_EQ((*result.pipeline)["pipeline"][1]["inputs"]["text"],
            "input.sentence_text");
  EXPECT_EQ((*result.pipeline)["io"]["output"][0]["inputs"]["matches"],
            "match.matches");
  ASSERT_EQ(result.changes.size(), 4u);
  const auto serialized = result.ToJson();
  EXPECT_EQ(serialized["changes"][1]["port"], "text");
  EXPECT_EQ(serialized["changes"][1]["affected_nodes"],
            Json::array({"input", "match"}));
  EXPECT_TRUE(serialized.contains("validation"));
}

TEST(
    PipelineEditingTest,
    OrderingIsDeduplicatedRemovedAndCycleFailureLeavesOperationStateUntouched) {
  const auto result = Edit(Document(),
                           Json::array({{{"kind", "add_dependency"},
                                         {"node", "render_more"},
                                         {"depends_on", "render"}},
                                        {{"kind", "remove_dependency"},
                                         {"node", "render_more"},
                                         {"depends_on", "match"}}}),
                           true);
  ASSERT_TRUE(result.ok) << result.ToJson();
  EXPECT_EQ((*result.pipeline)["pipeline"][2]["depends_on"],
            Json::array({"render"}));
  auto candidate = Document();
  const auto original = candidate;
  std::vector<AuthoringChange> changes;
  std::string error;
  EXPECT_FALSE(PipelineAuthoring::ApplyOperation(
      &candidate,
      {{"kind", "add_dependency"}, {"node", "render"}, {"depends_on", "match"}},
      &changes, &error));
  EXPECT_EQ(candidate, original);
  EXPECT_TRUE(changes.empty());
  EXPECT_NE(error.find("DAG_CYCLE"), std::string::npos);
}

TEST(PipelineEditingTest,
     BatchFailureExposesIndexAndErrorWithoutPublishingPartialCandidate) {
  const auto document = Document();
  const auto result =
      Edit(document, Json::array({{{"kind", "rename_node"},
                                   {"node", "render"},
                                   {"new_name", "renamed"}},
                                  Edge("disconnect", "input", "sentence_text",
                                       "match", "text")}));
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.pipeline.has_value());
  ASSERT_TRUE(result.failed_operation_index.has_value());
  EXPECT_EQ(*result.failed_operation_index, 1u);
  const auto serialized = result.ToJson();
  EXPECT_FALSE(serialized.contains("pipeline"));
  ASSERT_EQ(serialized["diagnostics"].size(), 1u);
  EXPECT_EQ(serialized["diagnostics"][0]["code"], "AUTHORING_ERROR");
  EXPECT_EQ(serialized["diagnostics"][0]["path"], "/");
  EXPECT_EQ(document, Document());
}

TEST(PipelineEditingTest,
     EditingProtocolRejectsUnknownFieldsTypesEndpointsAndAmbiguousNames) {
  const Json operations = Json::array(
      {{{"kind", "add_node"}, {"type", "missing_type"}},
       {{"kind", "rename_node"}, {"node", "render"}, {"new_name", "input"}},
       {{"kind", "add_node"}, {"type", "text_template"}, {"params", 42}},
       Edge("connect", "output", "matches", "match", "text"),
       Edge("connect", "render", "missing_port", "match", "text"),
       Edge("connect", "match", "matches", "render_more", "primary"),
       {{"kind", "remove_node"}, {"node", "render"}, {"extra", true}}});
  for (const auto& operation : operations) {
    SCOPED_TRACE(operation.dump());
    const auto result = PipelineAuthoring::ApplyRequest(
        {{"pipeline", Document()}, {"operation", operation}});
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.pipeline.has_value());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_TRUE(result.changes.empty());
  }
  auto ambiguous = Document();
  ambiguous["pipeline"].push_back(ambiguous["pipeline"][0]);
  const auto result = PipelineAuthoring::ApplyRequest(
      {{"pipeline", ambiguous},
       {"operation", {{"kind", "remove_node"}, {"node", "render"}}}});
  ASSERT_FALSE(result.diagnostics.empty());
  EXPECT_NE(result.diagnostics[0].find("AMBIGUOUS_NODE_NAME"),
            std::string::npos);
}
}  // namespace
}  // namespace llm_edgeflow
