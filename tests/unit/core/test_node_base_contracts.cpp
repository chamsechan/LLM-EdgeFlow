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
#include "dev_support/node_authoring/legacy_node_base.h"
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

// 2. Require / Publish 辅助函数测试
inline constexpr BlackboardKey<std::string> kTestInputKey{"test_input_key",
                                                          "string"};
inline constexpr BlackboardKey<std::string> kTestOutputKey{"test_output_key",
                                                           "string"};

class HelperTestNode : public LegacyNodeBase {
 public:
  inline static constexpr char kNodeType[] = "helper_test";
  HelperTestNode() : LegacyNodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& ctx) override {
    const auto* in_val = Require(ctx, kTestInputKey, -9901, "test semantics");
    if (!in_val) return -9901;
    Publish(ctx, kTestOutputKey, *in_val + "_processed");
    return 0;
  }
};

TEST(NodeBaseContractsTest, RequireAndPublishHelpers) {
  HelperTestNode node;
  SessionContext session_ctx;
  ValidatedNodePlan plan;
  plan.normalized_params = nlohmann::json::object();
  ASSERT_TRUE(node.Init({&plan, &session_ctx}));

  // 缺少输入键
  {
    AlgContext ctx;
    int ret = node.Process(&ctx);
    EXPECT_EQ(ret, -9901);
    EXPECT_EQ(ctx.GetErrorCode(), -9901);
    EXPECT_TRUE(ctx.GetErrorMessage().find("missing required input key") !=
                std::string::npos);
  }

  // 成功路径
  {
    AlgContext ctx;
    ctx.Publish(kTestInputKey, std::string("hello"));
    int ret = node.Process(&ctx);
    EXPECT_EQ(ret, 0);
    const auto* out_val = ctx.Read(kTestOutputKey);
    ASSERT_NE(out_val, nullptr);
    EXPECT_EQ(*out_val, "hello_processed");

    // 同一请求内第二次调用生产者，不能静默覆盖已发布的输出。
    EXPECT_EQ(node.Process(&ctx),
              static_cast<int>(NodeRuntimeCode::kUnhandledException));
    EXPECT_EQ(*out_val, "hello_processed");
    EXPECT_TRUE(ctx.GetErrorMessage().find("Duplicate output publication") !=
                std::string::npos);
  }

  // 键已存在但运行时类型不兼容。
  {
    AlgContext ctx;
    ctx.Publish(std::string(kTestInputKey.name), 42);
    int ret = node.Process(&ctx);
    EXPECT_EQ(ret, -9901);
    EXPECT_EQ(ctx.GetErrorCode(), -9901);
    EXPECT_TRUE(ctx.GetErrorMessage().find("type mismatch") !=
                std::string::npos);
    EXPECT_TRUE(ctx.GetErrorMessage().find(kTestInputKey.type_id) !=
                std::string::npos);
  }
}

class BoundPortProbeNode : public LegacyNodeBase {
 public:
  BoundPortProbeNode() : LegacyNodeBase("bound_port_probe"), input_("input") {}

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json&,
                SessionContext&) override {
    BindPort(init_ctx, input_);
    return true;
  }
  int ProcessNode(AlgContext&) override { return 0; }

 private:
  BoundInput<std::string> input_;
};

TEST(NodeBaseContractsTest, BindingRejectsDefinitionRuntimeTypeDrift) {
  ValidatedNodePlan plan;
  plan.normalized_params = nlohmann::json::object();
  plan.ports.push_back({"input", "actual_input", "integer", "1:1", "preserve",
                        "request", PortDirection::kInput});
  SessionContext session_ctx;
  NodeInitContext init_ctx;
  init_ctx.plan = &plan;
  init_ctx.session_ctx = &session_ctx;

  BoundPortProbeNode node;
  EXPECT_FALSE(node.Init(init_ctx));
}

class MultiPortProbeNode : public LegacyNodeBase {
 public:
  inline static constexpr auto kFirst = MakeBlackboardKey<TextBatch>("first");
  inline static constexpr auto kOptional =
      MakeBlackboardKey<TextBatch>("optional");
  inline static constexpr auto kCombined =
      MakeBlackboardKey<TextBatch>("combined");
  inline static constexpr auto kEcho = MakeBlackboardKey<TextBatch>("echo");

  MultiPortProbeNode() : LegacyNodeBase("multi_port_probe") {}
  bool OptionalIsBound() const { return optional_.IsBound(); }
  bool CombinedIsBound() const { return combined_.IsBound(); }
  bool EchoIsBound() const { return echo_.IsBound(); }

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json&,
                SessionContext&) override {
    BindPorts(init_ctx, first_, optional_, combined_, echo_);
    return true;
  }
  int ProcessNode(AlgContext& ctx) override {
    const auto* first = first_.Require(ctx, -9901);
    if (!first) return -9901;
    const auto* optional = optional_.Get(ctx);
    TextBatch combined = *first;
    for (auto& item : combined) {
      item.data += optional ? optional->at(0).data : "<absent>";
    }
    combined_.Set(ctx, std::move(combined));
    echo_.Set(ctx, *first);
    return 0;
  }

 private:
  BoundInput<TextBatch> first_{kFirst};
  BoundInput<TextBatch> optional_{kOptional};
  BoundOutput<TextBatch> combined_{kCombined};
  BoundOutput<TextBatch> echo_{kEcho};
};

TEST(NodeBaseContractsTest,
     BindPortsUsesPlannedKeysAndDisconnectsOptionalInput) {
  test_support::RegistryTestAccess::ScopedNodeState state_guard;
  NodeDefinition definition;
  definition.node_type = "multi_port_probe";
  definition.inputs = {RequiredInputPort(MultiPortProbeNode::kFirst),
                       OptionalInputPort(MultiPortProbeNode::kOptional)};
  definition.outputs = {OutputPort(MultiPortProbeNode::kCombined),
                        OutputPort(MultiPortProbeNode::kEcho)};
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      definition.node_type,
      [] { return std::make_unique<MultiPortProbeNode>(); }, definition));

  for (bool omit_optional : {false, true}) {
    SCOPED_TRACE(omit_optional);
    std::string diagnostic;
    const std::unordered_set<std::string> omitted =
        omit_optional ? std::unordered_set<std::string>{"optional"}
                      : std::unordered_set<std::string>{};
    const auto plan = PrepareNodePlanForTest(
        definition.node_type, nlohmann::json::object(), omitted, "actual_in_",
        "actual_out_", &diagnostic);
    ASSERT_NE(plan, nullptr) << diagnostic;
    SessionContext session;
    MultiPortProbeNode node;
    ASSERT_TRUE(node.Init({plan.get(), &session, &diagnostic})) << diagnostic;
    EXPECT_EQ(node.OptionalIsBound(), !omit_optional);
    AlgContext ctx;
    ctx.Publish("actual_in_first", TextBatch{{7, 0, "mapped"}});
    ctx.Publish("actual_in_optional", TextBatch{{7, 0, "+optional"}});
    ctx.Publish(MultiPortProbeNode::kFirst, TextBatch{{7, 0, "wrong first"}});
    ctx.Publish(MultiPortProbeNode::kOptional,
                TextBatch{{7, 0, "stale optional"}});
    ASSERT_EQ(node.Process(&ctx), 0);
    const auto* combined = ctx.Read<TextBatch>("actual_out_combined");
    const auto* echo = ctx.Read<TextBatch>("actual_out_echo");
    ASSERT_NE(combined, nullptr);
    ASSERT_NE(echo, nullptr);
    ASSERT_EQ(combined->size(), 1u);
    ASSERT_EQ(echo->size(), 1u);
    EXPECT_EQ(combined->at(0).data,
              omit_optional ? "mapped<absent>" : "mapped+optional");
    EXPECT_EQ(echo->at(0).data, "mapped");
    EXPECT_EQ(combined->at(0).req_id, 7);
    EXPECT_EQ(combined->at(0).sub_id, 0);
    EXPECT_EQ(echo->at(0).req_id, 7);
    EXPECT_EQ(echo->at(0).sub_id, 0);
    EXPECT_FALSE(ctx.Has(MultiPortProbeNode::kCombined));
    EXPECT_FALSE(ctx.Has(MultiPortProbeNode::kEcho));
  }
}

TEST(NodeBaseContractsTest, BindPortsStopsAtFirstErrorInDeclarationOrder) {
  ValidatedNodePlan plan;
  plan.normalized_params = nlohmann::json::object();
  plan.ports = {{"first", "actual_first", "integer", "1:1", "preserve",
                 "request", PortDirection::kInput},
                {"optional", "actual_optional", "TextBatch", "1:1", "preserve",
                 "request", PortDirection::kInput},
                {"combined", "actual_combined", "integer", "1:1", "preserve",
                 "request", PortDirection::kOutput},
                {"echo", "actual_echo", "TextBatch", "1:1", "preserve",
                 "request", PortDirection::kOutput}};
  SessionContext session;
  MultiPortProbeNode node;
  std::string diagnostic;
  EXPECT_FALSE(node.Init({&plan, &session, &diagnostic}));
  EXPECT_EQ(diagnostic,
            "Input port TypeId mismatch for first (expected: TextBatch, bound: "
            "integer)");
  EXPECT_FALSE(node.OptionalIsBound());
  EXPECT_FALSE(node.CombinedIsBound());
  EXPECT_FALSE(node.EchoIsBound());
}

TEST(NodeBaseContractsTest,
     OutputBindingAllowsUnusedPortsAndRejectsBoundTypeMismatch) {
  for (int fault = 0; fault < 3; ++fault) {
    SCOPED_TRACE(fault);
    ValidatedNodePlan plan;
    plan.normalized_params = nlohmann::json::object();
    plan.ports.push_back({"optional", "", "integer", "1:1", "preserve",
                          "request", PortDirection::kInput});
    if (fault != 0) {
      plan.ports.push_back({"combined", fault == 1 ? "" : "actual_combined",
                            "integer", "1:1", "preserve", "request",
                            PortDirection::kOutput});
    }
    plan.ports.push_back({"echo", "actual_echo", "TextBatch", "1:1", "preserve",
                          "request", PortDirection::kOutput});
    SessionContext session;
    MultiPortProbeNode node;
    std::string diagnostic;
    EXPECT_EQ(node.Init({&plan, &session, &diagnostic}), fault != 2);
    EXPECT_EQ(diagnostic, fault == 2
                              ? "Output port TypeId mismatch for combined "
                                "(expected: TextBatch, bound: integer)"
                              : "");
    EXPECT_FALSE(node.OptionalIsBound());
    EXPECT_FALSE(node.CombinedIsBound());
    EXPECT_EQ(node.EchoIsBound(), fault != 2);
  }
}

// 3. 使用模型能力的统一函数式编写
class MockAsrModel : public IAsrModel {
 public:
  const std::string& ImplName() const noexcept override {
    static const std::string t = "mock_asr";
    return t;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string model_type = "asr";
    return model_type;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  bool SupportsLanguage(std::string_view) const noexcept override {
    return true;
  }
  int Transcribe(const AudioPcmBatch& inputs, const TranscribeOptions&,
                 TextBatch* outputs,
                 std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
    if (!outputs) return -1;
    ++infer_calls_;
    outputs->clear();
    if (fail_) {
      if (diagnostic) *diagnostic = "ASR device unavailable";
      return -6299;
    }
    for (const auto& item : inputs) {
      outputs->emplace_back(item.req_id, item.sub_id, "mock_transcription");
    }
    if (return_wrong_count_ && !outputs->empty()) {
      outputs->pop_back();
    }
    if (corrupt_provenance_ && outputs->size() > 1) {
      ++(*outputs)[1].sub_id;
    }
    return 0;
  }

  bool fail_ = false;
  int infer_calls_ = 0;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

inline constexpr BlackboardKey<AudioPcmBatch> kTestAudioInputs{
    "test_audio_inputs", "AudioPcmBatch"};
inline constexpr BlackboardKey<TextBatch> kTestTranscripts{"test_transcripts",
                                                           "TextBatch"};

struct MockAsrInputs {
  const AudioPcmBatch* audio = nullptr;
};
struct MockAsrModels {
  AsrCall transcriber;
};

auto MockAsrSpec() {
  return MakeNodeSpec(
      InputsOf<MockAsrInputs>{
          Required(kTestAudioInputs.name, &MockAsrInputs::audio)},
      PreservedOutput<TextBatch>(kTestTranscripts.name, kTestAudioInputs.name),
      ModelsOf<MockAsrModels>{
          Model("transcriber", "bind_model", &MockAsrModels::transcriber)},
      [](const MockAsrInputs& inputs, const MockAsrModels& models) {
        return models.transcriber.Transcribe(*inputs.audio,
                                             TranscribeOptions{});
      });
}
using MockAsrNode = AuthorNode<decltype(MockAsrSpec())>;

TEST(NodeBaseContractsTest, FunctionAsrWorkflow) {
  test_support::RegistryTestAccess::ScopedNodeState state_guard;
  auto definition = MockAsrSpec().BuildDefinition("mock_asr");
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      definition.node_type,
      []() { return std::make_unique<MockAsrNode>("mock_asr", MockAsrSpec()); },
      definition));

  SessionContext session_ctx;
  auto model = std::make_shared<MockAsrModel>();
  RegisterTestModel(session_ctx.GetModelManager(), "test_asr_model", model,
                    "test-v1");

  MockAsrNode node("mock_asr", MockAsrSpec());
  ASSERT_TRUE(
      InitNodeForTest(node, {{"bind_model", "test_asr_model"}}, &session_ctx));

  AlgContext ctx;
  AudioPcmBatch audios;
  audios.emplace_back(0, 0, AudioPcmPayload{});
  audios.emplace_back(0, 1, AudioPcmPayload{});
  ctx.Publish(kTestAudioInputs, std::move(audios));

  int ret = node.Process(&ctx);
  EXPECT_EQ(ret, 0);

  const auto* results = ctx.Read(kTestTranscripts);
  ASSERT_NE(results, nullptr);
  ASSERT_EQ(results->size(), 2u);
  EXPECT_EQ((*results)[0].data, "mock_transcription");
  EXPECT_EQ(model->infer_calls_, 1);

  AlgContext empty_ctx;
  empty_ctx.Publish(kTestAudioInputs, AudioPcmBatch{});
  EXPECT_EQ(node.Process(&empty_ctx), 0);
  const auto* empty_results = empty_ctx.Read(kTestTranscripts);
  ASSERT_NE(empty_results, nullptr);
  EXPECT_TRUE(empty_results->empty());
  EXPECT_EQ(model->infer_calls_, 1);

  model->fail_ = true;
  AlgContext failure_ctx;
  failure_ctx.Publish(kTestAudioInputs,
                      AudioPcmBatch{{7, 0, AudioPcmPayload{}}});
  EXPECT_EQ(node.Process(&failure_ctx), -6299);
  EXPECT_EQ(failure_ctx.GetErrorCode(), -6299);
  EXPECT_NE(failure_ctx.GetErrorMessage().find("ASR device unavailable"),
            std::string::npos);
  EXPECT_FALSE(failure_ctx.Has(kTestTranscripts.name));
  model->fail_ = false;

  AudioPcmBatch invalid_audios;
  invalid_audios.emplace_back(7, 0, AudioPcmPayload{});
  invalid_audios.emplace_back(7, 1, AudioPcmPayload{});

  model->return_wrong_count_ = true;
  AlgContext count_ctx;
  count_ctx.Publish(kTestAudioInputs, invalid_audios);
  EXPECT_EQ(node.Process(&count_ctx),
            node_error::author_node::kOutputCountMismatch);
  EXPECT_FALSE(count_ctx.Has(kTestTranscripts.name));

  model->return_wrong_count_ = false;
  model->corrupt_provenance_ = true;
  AlgContext provenance_ctx;
  provenance_ctx.Publish(kTestAudioInputs, invalid_audios);
  EXPECT_EQ(node.Process(&provenance_ctx),
            node_error::author_node::kOutputProvenanceMismatch);
  EXPECT_FALSE(provenance_ctx.Has(kTestTranscripts.name));
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
