#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "contracts/parameters.h"
#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "engine/model_interface.h"
#include "nodes/authoring.h"
#include "nodes/node_base.h"
#include "nodes/node_error_codes.h"
#include "nodes/traceable_batch_validation.h"
#include "tests/support/model_registration.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {

TEST(NodeErrorCodesTest, ActiveBusinessAndAuthoringErrorsAreDistinct) {
  EXPECT_EQ(node_error::control::kInvalidRequest, -1);
  EXPECT_EQ(node_error::text_embedding::kSessionInferenceFailed, -5101);
  EXPECT_EQ(node_error::structured_json_parse::kParseFailed, -6102);
  EXPECT_EQ(node_error::text_template::kRenderedOutputTooLong, -6201);

  const std::vector<int> codes = {
      node_error::control::kInvalidRequest,
      node_error::text_chunk::kInvalidUtf8,
      node_error::text_embedding::kSessionInferenceFailed,
      node_error::text_rule_match::kRegexExecutionFailed,
      node_error::structured_json_parse::kParseFailed,
      node_error::text_template::kRenderedOutputTooLong,
      node_error::text_template::kInvalidUtf8,
      node_error::author_node::kMissingInput,
      node_error::author_node::kBusinessError,
      node_error::author_node::kModelCallFailed,
      node_error::author_node::kOutputCountMismatch,
      node_error::author_node::kOutputProvenanceMismatch,
      node_error::author_node::kInternalError,
  };
  EXPECT_EQ(std::unordered_set<int>(codes.begin(), codes.end()).size(),
            codes.size());
}

// 1. 抛异常的测试 Node
class ExceptionThrowingNode : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "exception_throwing";
  explicit ExceptionThrowingNode(bool throw_in_init = false)
      : NodeBase(kNodeType), throw_in_init_(throw_in_init) {}

 protected:
  bool InitNode(const NodeInitContext&, const nlohmann::json&,
                SessionContext&) override {
    if (throw_in_init_) {
      throw std::runtime_error("Simulated Init failure");
    }
    return true;
  }

  int ProcessNode(AlgContext&) override {
    throw std::runtime_error("Simulated Process failure");
  }

 private:
  bool throw_in_init_ = false;
};

TEST(NodeBaseContractsTest, NullContextSafety) {
  ExceptionThrowingNode node;
  EXPECT_EQ(node.Process(nullptr),
            static_cast<int>(NodeRuntimeCode::kInvalidContext));
  EXPECT_FALSE(node.Init({}));
}

TEST(NodeBaseContractsTest, InitAndProcessExceptionSafety) {
  ExceptionThrowingNode fail_init_node(true);
  SessionContext session_ctx;

  std::string diagnostic = "stale";
  ValidatedNodePlan plan;
  plan.normalized_params = nlohmann::json::object();
  NodeInitContext init_ctx;
  init_ctx.plan = &plan;
  init_ctx.session_ctx = &session_ctx;
  init_ctx.diagnostic = &diagnostic;
  EXPECT_FALSE(fail_init_node.Init(init_ctx));
  EXPECT_EQ(diagnostic, "Simulated Init failure");
  init_ctx.session_ctx = nullptr;
  EXPECT_FALSE(fail_init_node.Init(init_ctx));
  EXPECT_NE(diagnostic.find("SessionContext"), std::string::npos);

  ExceptionThrowingNode fail_proc_node(false);
  init_ctx.session_ctx = &session_ctx;
  EXPECT_TRUE(fail_proc_node.Init(init_ctx));

  AlgContext ctx;
  int ret = fail_proc_node.Process(&ctx);
  EXPECT_EQ(ret, static_cast<int>(NodeRuntimeCode::kUnhandledException));
  EXPECT_EQ(ctx.GetErrorCode(),
            static_cast<int>(NodeRuntimeCode::kUnhandledException));
  EXPECT_TRUE(ctx.GetErrorMessage().find("Simulated Process failure") !=
              std::string::npos);
}

TEST(NodeBaseContractsTest, TraceableAlignmentReportsFirstMismatch) {
  TextBatch inputs = {{1, 0, "first"}, {1, 1, "second"}};
  EmbeddingBatch outputs = {
      {1, 0, std::vector<float>{1.0F}},
      {1, 1, std::vector<float>{2.0F}},
  };

  auto result = ValidatePreservedTraceableAlignment(inputs, outputs);
  EXPECT_TRUE(result.IsAligned());
  EXPECT_EQ(result.mismatch_index, 2U);

  outputs.pop_back();
  result = ValidatePreservedTraceableAlignment(inputs, outputs);
  EXPECT_EQ(result.error, TraceableAlignmentError::kCountMismatch);
  EXPECT_EQ(result.mismatch_index, 1U);

  outputs.emplace_back(1, 9, std::vector<float>{2.0F});
  result = ValidatePreservedTraceableAlignment(inputs, outputs);
  EXPECT_EQ(result.error, TraceableAlignmentError::kProvenanceMismatch);
  EXPECT_EQ(result.mismatch_index, 1U);
}

}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST(NodeBaseContractsTest, UnconnectedPlannedInputCannotReadDefaultKey) {
  BoundInput<std::string> input("attributes");
  AlgContext ctx;
  ctx.Publish("attributes", std::string("unrelated"));
  input.Unbind();
  EXPECT_FALSE(input.IsBound());
  EXPECT_FALSE(input.Has(ctx));
  EXPECT_EQ(input.Get(ctx), nullptr);
  input.Resolve("attributes");
  EXPECT_EQ(*input.Get(ctx), "unrelated");
}
}  // namespace llm_edgeflow
