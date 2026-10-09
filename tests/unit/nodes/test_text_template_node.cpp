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

TEST_F(TextTemplateNodeTest, UnknownVariableFailsPreparation) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  std::string diagnostic;
  EXPECT_FALSE(InitNodeForTest(
      *node, {{"template", "Hello {{unknown_variable}}, welcome!"}},
      session_ctx_.get(), &diagnostic));
  EXPECT_NE(diagnostic.find("unknown_variable"), std::string::npos)
      << diagnostic;
}

TEST_F(TextTemplateNodeTest, SingleBraceTreatedAsLiteral) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);

  // 单花括号和 JSON 对象必须保持为字面文本。
  // 只有双花括号 {{var}} 才是模板变量。
  nlohmann::json cfg = {{"template",
                         "Literal: {primary}, JSON: {\"key\": 1}, Var: "
                         "{{primary}}"}};
  ASSERT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  ctx.Publish("primary", TextBatch{{1, 7, "Alice"}});

  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("text");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  EXPECT_EQ(out->front().req_id, 1u);
  EXPECT_EQ(out->front().sub_id, 7u);
  EXPECT_EQ((*out)[0].data,
            "Literal: {primary}, JSON: {\"key\": 1}, Var: Alice");
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

  // 没有任何合法字段的无效更新 -> 拒绝
  nlohmann::json bogus_update = {{"bogus_field", 123}};
  NodeControlResult bogus_res =
      node->Control(kControlCmdUpdatePrompt, bogus_update.dump());
  EXPECT_EQ(bogus_res.status, NodeControlStatus::kFailed);

  EXPECT_EQ(node->Control(kControlCmdUpdatePrompt,
                          nlohmann::json{{"template", "{{unclosed"}}.dump())
                .status,
            NodeControlStatus::kFailed);
  EXPECT_EQ(
      node->Control(kControlCmdUpdatePrompt,
                    nlohmann::json{{"template", "{{unknown_variable}}"}}.dump())
          .status,
      NodeControlStatus::kFailed);
  AlgContext after_failure;
  after_failure.Publish("primary", TextBatch{{17, 4, "{{context}}"}});
  after_failure.Publish("context", RankedTextBatch{});
  ASSERT_EQ(node->Process(&after_failure), 0);
  ASSERT_NE(after_failure.Read<TextBatch>("text"), nullptr);
  ASSERT_EQ(after_failure.Read<TextBatch>("text")->size(), 1u);
  EXPECT_EQ(after_failure.Read<TextBatch>("text")->front().req_id, 17u);
  EXPECT_EQ(after_failure.Read<TextBatch>("text")->front().sub_id, 4u);
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
  pipe_json.erase("io");

  const auto boundary = MakeTestBoundary(
      {{"raw_docs", "TextBatch"}, {"raw_queries", "TextBatch"}},
      {{"llm_answers", "TextBatch"},
       {"intent_matches", "RuleMatchBatch"},
       {"doc_chunk_counts", "Int32Batch"}});
  ASSERT_TRUE(BuildTestPipeline(pipeline, pipe_json, boundary, &diagnostic))
      << diagnostic.message;

  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt, "{}"), 0);
  EXPECT_NE(pipeline.Control(
                kControlCmdUpdatePrompt,
                nlohmann::json{{"template", "{{primary}}"}, {"unknown", true}}
                    .dump()),
            0);
  EXPECT_EQ(
      pipeline.Control(kControlCmdUpdatePrompt,
                       nlohmann::json{{"template", "{{primary}}"}}.dump()),
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

TEST_F(TextTemplateNodeTest, UnconnectedInputVariableFailsPreparation) {
  for (const std::string variable : {"context", "matches", "document"}) {
    SCOPED_TRACE(variable);
    const std::string pattern = "Q={{primary}}|V={{" + variable + "}}";
    auto root = TemplatePipeline({{"template", pattern}});
    const auto invalid = PipelineValidator::ValidateAndPlan(
        root, MakeTestBoundary({{"input_sentences", "TextBatch"}},
                               {{"rule_matches", "RuleMatchBatch"}}));
    ASSERT_FALSE(invalid.report.ok);
    EXPECT_TRUE(
        std::any_of(invalid.report.diagnostics.begin(),
                    invalid.report.diagnostics.end(), [&](const auto& d) {
                      return d.code == DiagnosticCode::kInvalidCombination &&
                             d.path == "/pipeline/0/config" &&
                             d.message.find(variable) != std::string::npos;
                    }));
    auto node = NodeRegistry::Instance().Create("TextTemplateNode");
    ASSERT_NE(node, nullptr);
    std::string diagnostic;
    EXPECT_FALSE(InitNodeForTest(*node, {{"template", pattern}},
                                 session_ctx_.get(), &diagnostic, {variable}));
    EXPECT_NE(diagnostic.find(variable), std::string::npos) << diagnostic;
  }
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  std::string diagnostic;
  EXPECT_FALSE(InitNodeForTest(*node, {{"template", "{{primary}}"}},
                               session_ctx_.get(), &diagnostic, {"primary"}));
  EXPECT_NE(diagnostic.find("primary"), std::string::npos) << diagnostic;
}

TEST_F(TextTemplateNodeTest, ConnectedInputsWithoutRequestDataRenderEmpty) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(
      *node, {{"template", "{{primary}}|{{context}}|{{matches}}|{{document}}"}},
      session_ctx_.get()));
  AlgContext missing;
  missing.Publish("primary", TextBatch{{1, 5, "Q"}});
  ASSERT_EQ(node->Process(&missing), 0);
  ASSERT_NE(missing.Read<TextBatch>("text"), nullptr);
  ASSERT_EQ(missing.Read<TextBatch>("text")->size(), 1u);
  EXPECT_EQ(missing.Read<TextBatch>("text")->front().req_id, 1u);
  EXPECT_EQ(missing.Read<TextBatch>("text")->front().sub_id, 5u);
  EXPECT_EQ(missing.Read<TextBatch>("text")->front().data, "Q|||");

  AlgContext empty;
  empty.Publish("primary", TextBatch{{1, 5, "Q"}});
  empty.Publish("context", RankedTextBatch{});
  empty.Publish("matches", RuleMatchBatch{});
  empty.Publish("document", OcrDocumentBatch{});
  ASSERT_EQ(node->Process(&empty), 0);
  ASSERT_NE(empty.Read<TextBatch>("text"), nullptr);
  EXPECT_EQ(empty.Read<TextBatch>("text")->front().data, "Q|||");

  AlgContext other_request;
  other_request.Publish("primary", TextBatch{{1, 5, "Q"}});
  other_request.Publish(
      "context", RankedTextBatch{{2, 0, RankedCandidate("other", 1.0f)}});
  other_request.Publish(
      "matches", RuleMatchBatch{{2, 3, RuleMatchItem(1, "OTHER", "other")}});
  other_request.Publish("document",
                        OcrDocumentBatch{{2, 7, OcrDocumentItem{{}, "other"}}});
  ASSERT_EQ(node->Process(&other_request), 0);
  ASSERT_NE(other_request.Read<TextBatch>("text"), nullptr);
  ASSERT_EQ(other_request.Read<TextBatch>("text")->size(), 1u);
  EXPECT_EQ(other_request.Read<TextBatch>("text")->front().data, "Q|||");
}

TEST_F(TextTemplateNodeTest, AggregateOnlyInputsRenderMissingPrimaryAsEmpty) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(
      *node, {{"template", "{{primary}}|{{context}}|{{matches}}|{{document}}"}},
      session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("context",
              RankedTextBatch{{2, 7, RankedCandidate("second", 1.0f)},
                              {1, 4, RankedCandidate("first", 1.0f)}});
  ctx.Publish("matches",
              RuleMatchBatch{{4, 6, RuleMatchItem(1, "FOURTH", "match")}});
  ctx.Publish("document",
              OcrDocumentBatch{{3, 9, OcrDocumentItem{{}, "third"}}});
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<TextBatch>("text");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 4u);
  const std::vector<std::string> expected = {"|first||", "|second||",
                                             "|||third", "||FOURTH (match)|"};
  for (size_t i = 0; i < output->size(); ++i) {
    EXPECT_EQ((*output)[i].req_id, i + 1);
    EXPECT_EQ((*output)[i].sub_id, 0u);
    EXPECT_EQ((*output)[i].data, expected[i]);
  }
}

TEST_F(TextTemplateNodeTest, OverflowDoesNotPublishPartialOutput) {
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node,
                              {{"template", "{{primary}}"},
                               {"max_length", 5},
                               {"overflow_policy", "fail"}},
                              session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("primary", TextBatch{{1, 4, "short"}, {2, 7, "too long"}});
  EXPECT_EQ(node->Process(&ctx), -6201);
  EXPECT_FALSE(ctx.Has("text"));

  AlgContext valid_empty;
  valid_empty.Publish("primary", TextBatch{{1, 4, ""}});
  ASSERT_EQ(node->Process(&valid_empty), 0);
  ASSERT_NE(valid_empty.Read<TextBatch>("text"), nullptr);
  ASSERT_EQ(valid_empty.Read<TextBatch>("text")->size(), 1u);
  EXPECT_EQ(valid_empty.Read<TextBatch>("text")->front().req_id, 1u);
  EXPECT_EQ(valid_empty.Read<TextBatch>("text")->front().sub_id, 4u);
  EXPECT_EQ(valid_empty.Read<TextBatch>("text")->front().data, "");
}

TEST_F(TextTemplateNodeTest,
       ControlRejectsUnconnectedBuiltinAndRetainsConfiguration) {
  Pipeline pipeline;
  ASSERT_TRUE(BuildTestPipeline(
      pipeline, TemplatePipeline({{"template", "{{primary}}"}}),
      MakeTestBoundary({{"input_sentences", "TextBatch"}},
                       {{"rule_matches", "RuleMatchBatch"}})));
  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt,
                             R"({"template":"{{context}}"})"),
            0);
  AlgContext original;
  original.Publish("input_sentences", TextBatch{{1, 3, "Q"}});
  ASSERT_EQ(pipeline.Execute(&original), 0);
  ASSERT_NE(original.Read<TextBatch>("rendered_text"), nullptr);
  EXPECT_EQ(original.Read<TextBatch>("rendered_text")->front().data, "Q");
  EXPECT_EQ(pipeline.Control(kControlCmdUpdatePrompt,
                             R"({"template":"NEW: {{primary}}"})"),
            0);
  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt,
                             R"({"template":"{{unknown_variable}}"})"),
            0);
  EXPECT_NE(pipeline.Control(kControlCmdUpdatePrompt,
                             R"({"template":"{{context}}"})"),
            0);
  AlgContext after_failure;
  after_failure.Publish("input_sentences", TextBatch{{2, 7, "Q"}});
  ASSERT_EQ(pipeline.Execute(&after_failure), 0);
  const auto* output = after_failure.Read<TextBatch>("rendered_text");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 1u);
  EXPECT_EQ(output->front().req_id, 2u);
  EXPECT_EQ(output->front().sub_id, 7u);
  EXPECT_EQ(output->front().data, "NEW: Q");
}

TEST_F(TextTemplateNodeTest, BoundTypedInputRemainsAvailableAcrossControl) {
  std::string diagnostic;
  auto plan = PrepareNodePlanForTest(
      "TextTemplateNode", {{"template", "{{context}}"}},
      {"primary", "matches", "document"}, "bound_", "", &diagnostic);
  ASSERT_NE(plan, nullptr) << diagnostic;
  auto node = NodeRegistry::Instance().Create("TextTemplateNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init({plan.get(), session_ctx_.get(), &diagnostic}))
      << diagnostic;
  EXPECT_EQ(node->Control(kControlCmdUpdatePrompt,
                          R"({"template":"Context: {{context}}"})")
                .status,
            NodeControlStatus::kHandled);
  EXPECT_EQ(
      node->Control(kControlCmdUpdatePrompt, R"({"template":"{{primary}}"})")
          .status,
      NodeControlStatus::kFailed);
  AlgContext ctx;
  ctx.Publish("bound_context",
              RankedTextBatch{{1, 8, RankedCandidate("Alice", 1.0f)}});
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* output = ctx.Read<TextBatch>("text");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 1u);
  EXPECT_EQ(output->front().req_id, 1u);
  EXPECT_EQ(output->front().sub_id, 0u);
  EXPECT_EQ(output->front().data, "Context: Alice");
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
