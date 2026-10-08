#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "adapter/deployment_preparation.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "tests/support/node_process_pause.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/pipeline_test_utils.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {

class TextTemplateNodeTest : public ::testing::Test {
 protected:
  void SetUp() override { session_ctx_ = std::make_unique<SessionContext>(); }
  std::unique_ptr<SessionContext> session_ctx_;
};

std::string ResolveConfigPath(const std::string& relative) {
  FILE* file = fopen(relative.c_str(), "r");
  if (file) {
    fclose(file);
    return relative;
  }
  return "../" + relative;
}

TEST_F(TextTemplateNodeTest, ProcessMultiInputAggregation) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {
      {"template",
       "Query: {{primary}}\nContext: {{context}}\nCategory: {{matches}}"},
      {"separator", " | "}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch primary;
  primary.emplace_back(1, 0, "How do I upgrade?");

  RankedTextBatch context;
  context.emplace_back(1, 0, RankedCandidate("Step 1: Go to settings", 0.9f));
  context.emplace_back(1, 1, RankedCandidate("Step 2: Click upgrade", 0.8f));

  RuleMatchBatch matches;
  matches.emplace_back(1, 0, RuleMatchItem(1, "ACCOUNT_UPGRADE", "upgrade"));

  ctx.Publish("primary", primary);
  ctx.Publish("context", context);
  ctx.Publish("matches", matches);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_NE((*out)[0].data.find("Go to settings | Step 2: Click upgrade"),
            std::string::npos);
  EXPECT_NE((*out)[0].data.find("ACCOUNT_UPGRADE"), std::string::npos);
}

TEST_F(TextTemplateNodeTest, UnknownVariableFailsInit) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  // 模板变量只能是输入端口名；不再有静态变量或动态属性。
  for (const std::string tmpl : {"Hello {{user_name}}, welcome!",
                                 "{{context_text}}", "{{attributes}}"}) {
    std::string diagnostic;
    EXPECT_FALSE(InitNodeForTest(*node, {{"template", tmpl}},
                                 session_ctx_.get(), &diagnostic))
        << tmpl;
  }
}

TEST_F(TextTemplateNodeTest, DocumentAndContextJoinedBySeparator) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(
      *node, {{"template", "{{document}}#{{context}}"}, {"separator", "+"}},
      session_ctx_.get()));

  AlgContext ctx;
  OcrDocumentBatch documents;
  documents.emplace_back(1, 0, OcrDocumentItem{});
  documents.back().data.combined_text = "page1";
  documents.emplace_back(1, 1, OcrDocumentItem{});
  documents.back().data.combined_text = "page2";
  ctx.Publish("document", documents);
  ctx.Publish("context", RankedTextBatch{});

  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_EQ((*out)[0].data, "page1+page2#");
}

TEST_F(TextTemplateNodeTest, SingleBraceTreatedAsLiteral) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  // {user_name} 这样的单花括号和 JSON 对象 {"k": 1} 必须保持为字面文本。
  // 只有双花括号 {{var}} 才是模板变量。
  nlohmann::json cfg = {
      {"template",
       "Literal: {user_name}, JSON: {\"key\": 1}, Var: {{primary}}"}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  ctx.Publish("primary", TextBatch{{1, 0, "Alice"}});

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  EXPECT_EQ((*out)[0].data,
            "Literal: {user_name}, JSON: {\"key\": 1}, Var: Alice");
}

TEST_F(TextTemplateNodeTest, MalformedPlaceholderFailsInit) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  for (const std::string bad_tmpl :
       {"Hello {{unclosed", "Hello {{}}", "Hello {{invalid name}}"}) {
    nlohmann::json cfg = {{"template", bad_tmpl}};
    EXPECT_FALSE(InitNodeForTest(*node, cfg, session_ctx_.get()));
  }
}

TEST_F(TextTemplateNodeTest, TruncatePreservesUtf8CodePointBoundaries) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node,
                              {{"template", "{{primary}}"},
                               {"max_length", 4},
                               {"overflow_policy", "truncate"}},
                              session_ctx_.get()));

  AlgContext ctx;
  TextBatch primary;
  primary.emplace_back(1, 0, u8"中文");
  primary.emplace_back(2, 0, u8"A🙂B");
  ctx.Publish("primary", std::move(primary));

  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ((*out)[0].data, u8"中");
  EXPECT_EQ((*out)[1].data, "A");
}

TEST_F(TextTemplateNodeTest, TruncateRejectsInvalidUtf8) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node,
                              {{"template", "{{primary}}"},
                               {"max_length", 2},
                               {"overflow_policy", "truncate"}},
                              session_ctx_.get()));

  AlgContext ctx;
  TextBatch primary;
  primary.emplace_back(1, 0, std::string("A") + "\xF0\x9F");
  ctx.Publish("primary", std::move(primary));

  EXPECT_EQ(node->Process(&ctx), -6203);
  EXPECT_EQ(ctx.Read<TextBatch>("text"), nullptr);
}

TEST_F(TextTemplateNodeTest, ControlCommandHotSwapAndBogusRejection) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"template", "{{primary}}"}},
                              session_ctx_.get()));

  // 合法更新
  nlohmann::json valid_update = {
      {"template", "Updated: {{primary}} [{{context}}]"}};
  NodeControlResult res =
      node->Control(kControlCmdUpdatePrompt, valid_update.dump());
  EXPECT_EQ(res.status, NodeControlStatus::kHandled);

  // prompt_id 不是节点参数；接入层不会转发它。
  NodeControlResult prompt_id_res = node->Control(
      kControlCmdUpdatePrompt, nlohmann::json{{"prompt_id", "qa-v2"}}.dump());
  EXPECT_EQ(prompt_id_res.status, NodeControlStatus::kFailed);

  NodeControlResult removed_field_res =
      node->Control(kControlCmdUpdatePrompt,
                    nlohmann::json{{"prompt_template", "removed"}}.dump());
  EXPECT_EQ(removed_field_res.status, NodeControlStatus::kFailed);

  // 没有任何合法字段的无效更新 -> 拒绝
  nlohmann::json bogus_update = {{"bogus_field", 123}};
  NodeControlResult bogus_res =
      node->Control(kControlCmdUpdatePrompt, bogus_update.dump());
  EXPECT_EQ(bogus_res.status, NodeControlStatus::kFailed);

  EXPECT_EQ(node->Control(kControlCmdUpdatePrompt,
                          nlohmann::json{{"template", "{{unclosed"}}.dump())
                .status,
            NodeControlStatus::kFailed);
  AlgContext after_failure;
  after_failure.Publish("primary", TextBatch{{17, 4, "{{context}}"}});
  after_failure.Publish("context", RankedTextBatch{});
  ASSERT_EQ(node->Process(&after_failure), 0);
  ASSERT_NE(after_failure.Read<TextBatch>("text"), nullptr);
  EXPECT_EQ(after_failure.Read<TextBatch>("text")->front().data,
            "Updated: {{context}} []");
}

TEST_F(TextTemplateNodeTest, PipelineEnforcesPublishedControlSchema) {
  Pipeline pipeline;
  PipelineDiagnostic diagnostic;
  std::ifstream cfg_in(
      ResolveConfigPath("demo/fixtures/mock/pipeline_doc_qa.json"));
  ASSERT_TRUE(cfg_in.is_open());
  nlohmann::json pipe_json;
  cfg_in >> pipe_json;
  PreparedDeployment prepared;
  DeploymentDiagnostic prepare_diag;
  ASSERT_TRUE(
      PrepareDeploymentDocument(pipe_json, {}, &prepared, &prepare_diag))
      << prepare_diag.message;
  ASSERT_TRUE(BuildTestPipeline(pipeline, prepared.neutral_pipeline_json,
                                prepared.io_boundary, &diagnostic))
      << diagnostic.message;

  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt, "{}"), 0);
  EXPECT_NE(pipeline.Control(
                kControlCmdUpdatePrompt,
                nlohmann::json{{"template", "{{primary}}"}, {"unknown", true}}
                    .dump()),
            0);
  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt,
                             nlohmann::json{{"prompt_id", "doc-qa-v2"}}.dump()),
            0);
}

namespace {
nlohmann::json TemplatePipeline(const nlohmann::json& config) {
  auto root = nlohmann::json::parse(R"({
  "models": [],
  "pipeline": [
    {
      "id": "template",
      "node_type": "TextTemplateNode",
      "config": {},
      "inputs": {
        "primary": "input_sentences"
      },
      "outputs": {
        "text": "rendered_text"
      }
    },
    {
      "id": "rules",
      "node_type": "TextRuleMatchNode",
      "config": {},
      "inputs": {
        "text": "rendered_text"
      },
      "outputs": {
        "matches": "rule_matches"
      }
    }
  ]
})");
  root["pipeline"][0]["config"] = config;
  return root;
}
}  // namespace

TEST_F(TextTemplateNodeTest, UnconnectedOrUnknownVariableIsRejected) {
  for (const std::string variable : {"context", "matches", "document"}) {
    SCOPED_TRACE(variable);
    const std::string pattern = "Q={{primary}}|V={{" + variable + "}}";
    auto root = TemplatePipeline({{"template", pattern}});
    const auto invalid =
        PipelineValidator::ValidateAndPlan(root, KeywordMatchTestBoundary());
    ASSERT_FALSE(invalid.report.ok);
    EXPECT_TRUE(
        std::any_of(invalid.report.diagnostics.begin(),
                    invalid.report.diagnostics.end(), [&](const auto& d) {
                      return d.path == "/pipeline/0/config" &&
                             d.message.find(variable) != std::string::npos;
                    }));
  }
  for (const std::string variable :
       {"context_text", "document_text", "other"}) {
    SCOPED_TRACE(variable);
    auto root = TemplatePipeline({{"template", "{{" + variable + "}}"}});
    const auto invalid =
        PipelineValidator::ValidateAndPlan(root, KeywordMatchTestBoundary());
    ASSERT_FALSE(invalid.report.ok);
    EXPECT_TRUE(
        std::any_of(invalid.report.diagnostics.begin(),
                    invalid.report.diagnostics.end(), [&](const auto& d) {
                      return d.path == "/pipeline/0/config" &&
                             d.message.find(variable) != std::string::npos;
                    }));
  }
  Pipeline pipeline;
  PipelineDiagnostic diagnostic;
  ASSERT_TRUE(BuildTestPipeline(
      pipeline, TemplatePipeline({{"template", "Q={{primary}}"}}),
      KeywordMatchTestBoundary(), &diagnostic))
      << diagnostic.message;
  AlgContext ctx;
  ctx.Publish("input_sentences", TextBatch{{1, 3, "hello"}});
  ASSERT_EQ(pipeline.Execute(&ctx), 0);
  EXPECT_EQ(ctx.Read<TextBatch>("rendered_text")->front().data, "Q=hello");
}

TEST_F(TextTemplateNodeTest, ConnectedInputWithoutDataRendersEmpty) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_TRUE(InitNodeForTest(*node, {{"template", "{{primary}}|{{context}}"}},
                              session_ctx_.get()));
  AlgContext missing;
  missing.Publish("primary", TextBatch{{1, 0, "Q"}});
  ASSERT_EQ(node->Process(&missing), 0);
  EXPECT_EQ(missing.Read<TextBatch>("text")->front().data, "Q|");

  AlgContext empty;
  empty.Publish("primary", TextBatch{{1, 0, "Q"}});
  empty.Publish("context", RankedTextBatch{});
  ASSERT_EQ(node->Process(&empty), 0);
  ASSERT_NE(empty.Read<TextBatch>("text"), nullptr);
  EXPECT_EQ(empty.Read<TextBatch>("text")->front().data, "Q|");

  AlgContext other_request;
  other_request.Publish("primary", TextBatch{{1, 0, "Q"}});
  other_request.Publish(
      "context", RankedTextBatch{{2, 0, RankedCandidate("other", 1.0f)}});
  ASSERT_EQ(node->Process(&other_request), 0);
  EXPECT_EQ(other_request.Read<TextBatch>("text")->front().data, "Q|");
}

TEST_F(TextTemplateNodeTest, EmptyPrimaryRendersEmpty) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_TRUE(InitNodeForTest(*node, {{"template", "{{primary}}"}},
                              session_ctx_.get()));
  AlgContext valid_empty;
  valid_empty.Publish("primary", TextBatch{{1, 0, ""}});
  ASSERT_EQ(node->Process(&valid_empty), 0);
  EXPECT_EQ(valid_empty.Read<TextBatch>("text")->front().data, "");
}

TEST_F(TextTemplateNodeTest,
       ControlRejectsUnconnectedOrUnknownVariableAndRetainsConfiguration) {
  Pipeline pipeline;
  ASSERT_TRUE(BuildTestPipeline(pipeline,
                                TemplatePipeline({{"template", "{{primary}}"}}),
                                KeywordMatchTestBoundary()));
  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt,
                             R"({"template":"{{context}}"})"),
            0);
  AlgContext original;
  original.Publish("input_sentences", TextBatch{{1, 0, "Q"}});
  ASSERT_EQ(pipeline.Execute(&original), 0);
  EXPECT_EQ(original.Read<TextBatch>("rendered_text")->front().data, "Q");
  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt,
                             R"({"template":"{{unknown}}"})"),
            0);
  AlgContext after_failure;
  after_failure.Publish("input_sentences", TextBatch{{2, 0, "Q"}});
  ASSERT_EQ(pipeline.Execute(&after_failure), 0);
  EXPECT_EQ(after_failure.Read<TextBatch>("rendered_text")->front().data, "Q");
}

TEST_F(TextTemplateNodeTest, DirectConcurrentProcessAndControl) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"template", "OLD: {{primary}}"}},
                              session_ctx_.get()));
  const nlohmann::json update = {{"template", "NEW: {{primary}}"}};
  const TextBatch inputs{
      {101, 2, "first"}, {101, 7, "second"}, {202, 3, "third"}};
  AlgContext in_flight;
  in_flight.Publish("primary", inputs);
  test_support::NodeProcessPause pause;
  int process_result = -1;
  std::exception_ptr reader_error;
  // 在这条合法的 Process 路径上，snapshot.Read 之后的第一次堆分配是
  // 模板分组容器。在保留旧快照的状态下暂停。
  std::thread reader([&] {
    try {
      test_support::ScopedNextAllocationCallback callback(
          &test_support::NodeProcessPause::OnAllocation, &pause);
      process_result = node->Process(&in_flight);
    } catch (...) {
      reader_error = std::current_exception();
    }
  });
  const bool paused = pause.WaitUntilPaused();
  NodeControlStatus control_status = NodeControlStatus::kFailed;
  std::exception_ptr control_error;
  if (paused) {
    try {
      control_status =
          node->Control(kControlCmdUpdatePrompt, update.dump()).status;
    } catch (...) {
      control_error = std::current_exception();
    }
  }
  pause.Resume();
  reader.join();
  ASSERT_TRUE(paused) << "Process never reached the snapshot pause";
  ASSERT_EQ(reader_error, nullptr);
  ASSERT_EQ(control_error, nullptr);
  ASSERT_EQ(control_status, NodeControlStatus::kHandled);
  ASSERT_EQ(process_result, 0);

  auto expect_batch = [&](const AlgContext& ctx, const std::string& version) {
    const auto* output = ctx.Read<TextBatch>("text");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
      EXPECT_EQ((*output)[i].req_id, inputs[i].req_id);
      EXPECT_EQ((*output)[i].sub_id, inputs[i].sub_id);
      EXPECT_EQ((*output)[i].data, version + ": " + inputs[i].data);
    }
  };
  expect_batch(in_flight, "OLD");
  AlgContext subsequent;
  subsequent.Publish("primary", inputs);
  ASSERT_EQ(node->Process(&subsequent), 0);
  expect_batch(subsequent, "NEW");
}

TEST_F(TextTemplateNodeTest,
       CatalogDefinitionStatesOnlyDoubleBracesSubstitute) {
  auto def_opt = PipelineCatalog::FindNode("TextTemplateNode");
  ASSERT_TRUE(def_opt.has_value());
  const auto& def = *def_opt;
  EXPECT_NE(def.description.find("{{name}} substitutes variables"),
            std::string::npos);
  EXPECT_NE(
      def.description.find("single {name} and JSON braces remain literal"),
      std::string::npos);
  EXPECT_EQ(def.description.find("and {name} substitute"), std::string::npos);
}

}  // namespace llm_edgeflow
