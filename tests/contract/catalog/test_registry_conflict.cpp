#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "core/node_registry.h"
#include "edgeflow/operator/interface.h"

namespace llm_edgeflow {
TEST(RegistrationStartupTest,
     DuplicateNodePreservesFirstEntryAndPermanentlyRejectsStartup) {
  auto& registry = NodeRegistry::Instance();
  ASSERT_FALSE(registry.HasConflict());
  const auto original = registry.Find("text_rule_match");
  ASSERT_TRUE(original.has_value());
  const auto table = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(table.Init(), 0) << operator_api::GetOperatorLastError();
  ASSERT_EQ(table.DeInit(), 0);
  EXPECT_FALSE(registry.Register(
      original->node_type, [] { return std::unique_ptr<INode>{}; }, *original));
  ASSERT_TRUE(registry.HasConflict());
  ASSERT_NE(registry.Create(original->node_type), nullptr);
  const auto errors = registry.GetConflictErrors();
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("text_rule_match"), std::string::npos);
  EXPECT_NE(table.Init(), 0);
  const std::string first = operator_api::GetOperatorLastError();
  EXPECT_NE(first.find("text_rule_match"), std::string::npos);
  EXPECT_NE(table.Init(), 0);
  EXPECT_TRUE(registry.HasConflict());
  EXPECT_EQ(registry.Find("text_rule_match")->outputs.size(),
            original->outputs.size());
}
}  // namespace llm_edgeflow
