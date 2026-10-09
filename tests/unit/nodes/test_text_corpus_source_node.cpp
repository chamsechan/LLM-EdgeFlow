#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {

class TextCorpusSourceNodeTest : public ::testing::Test {
 protected:
  void SetUp() override { session_ctx_ = std::make_unique<SessionContext>(); }
  std::unique_ptr<SessionContext> session_ctx_;
};

TEST_F(TextCorpusSourceNodeTest, ProcessStaticCorpusEmission) {
  auto node = NodeRegistry::Instance().Create("TextCorpusSourceNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {
      {"corpus", {"Clause 1: Compliance", "Clause 2: Security"}}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("corpus");
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ((*out)[0].data, "Clause 1: Compliance");
  EXPECT_EQ((*out)[1].data, "Clause 2: Security");
  for (size_t index = 0; index < out->size(); ++index) {
    EXPECT_EQ(out->at(index).req_id, 0u);
    EXPECT_EQ(out->at(index).sub_id, index);
  }
  const auto definition = PipelineCatalog::FindNode("TextCorpusSourceNode");
  ASSERT_TRUE(definition.has_value());
  EXPECT_TRUE(definition->inputs.empty());
  ASSERT_EQ(definition->outputs.size(), 1u);
  EXPECT_EQ(definition->outputs.front().lifetime, "session");
}

TEST_F(TextCorpusSourceNodeTest, EmptyCorpusConfig) {
  auto node = NodeRegistry::Instance().Create("TextCorpusSourceNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"corpus", nlohmann::json::array()}},
                              session_ctx_.get()));

  AlgContext ctx;
  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out = ctx.Read<TextBatch>("corpus");
  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(out->empty());
}

TEST_F(TextCorpusSourceNodeTest,
       MissingAndInvalidCorpusRejectedDuringConfiguration) {
  struct InvalidCase {
    nlohmann::json config;
    DiagnosticCode code;
    std::string path;
  };
  const std::vector<InvalidCase> invalid = {
      {nlohmann::json::object(), DiagnosticCode::kMissingConfigField,
       "/corpus"},
      {{{"corpus", "not an array"}},
       DiagnosticCode::kConfigFieldType,
       "/corpus"},
      {{{"corpus", {"valid", 7}}},
       DiagnosticCode::kConfigFieldType,
       "/corpus/1"},
      {{{"corpus", {"valid", false}}},
       DiagnosticCode::kConfigFieldType,
       "/corpus/1"},
      {{{"corpus", {"valid", nullptr}}},
       DiagnosticCode::kConfigFieldType,
       "/corpus/1"}};
  for (const auto& item : invalid) {
    SCOPED_TRACE(item.config.dump());
    auto node = NodeRegistry::Instance().Create("TextCorpusSourceNode");
    ASSERT_NE(node, nullptr);
    std::string diagnostic;
    EXPECT_FALSE(
        InitNodeForTest(*node, item.config, session_ctx_.get(), &diagnostic));
    EXPECT_NE(diagnostic.find(item.path), std::string::npos) << diagnostic;
    const nlohmann::json pipeline = {
        {"biz_name", "keyword_match"},
        {"models", nlohmann::json::array()},
        {"pipeline",
         nlohmann::json::array({{{"id", "source"},
                                 {"node_type", "TextCorpusSourceNode"},
                                 {"config", item.config}}})}};
    const auto report = PipelineValidator::Validate(pipeline);
    EXPECT_FALSE(report.ok);
    EXPECT_TRUE(
        std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                    [&](const auto& error) {
                      return error.path == "/pipeline/0/config" + item.path &&
                             error.code == item.code;
                    }))
        << report.ToJson().dump(2);
  }
}

}  // namespace llm_edgeflow
