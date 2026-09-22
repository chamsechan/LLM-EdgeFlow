#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "engine/model_interface.h"
#include "nodes/authoring.h"
#include "nodes/node_base.h"
#include "nodes/node_config_parser.h"
#include "nodes/node_error_codes.h"
#include "nodes/traceable_batch_validation.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {

namespace {

struct NodeParserParameters {
  int count = 99;
  std::string label = "unparsed";
};

std::vector<ConfigFieldDefinition> NodeParserFields() {
  return {{"count", ConfigValueKind::kInteger, false, 3, 1.0, 8.0, {}, ""},
          {"label",
           ConfigValueKind::kString,
           false,
           "default",
           std::nullopt,
           std::nullopt,
           {},
           ""}};
}

}  // namespace

TEST(NodeBaseContractsTest, ConfigParserUsesFieldDefaultsBeforeSemanticParser) {
  int calls = 0;
  const NodeConfigParser<NodeParserParameters> parser(
      NodeParserFields(), [&](const nlohmann::json& config,
                              NodeParserParameters* result, std::string*) {
        ++calls;
        result->count = config.at("count").get<int>();
        result->label = config.at("label").get<std::string>();
        return true;
      });
  ASSERT_EQ(parser.Fields().size(), 2u);
  EXPECT_EQ(parser.Fields()[0].name, "count");
  EXPECT_EQ(parser.Fields()[0].default_value, 3);
  const nlohmann::json raw = nlohmann::json::object();
  std::string error = "old error";
  const auto result = parser.Parse(raw, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(result->count, 3);
  EXPECT_EQ(result->label, "default");
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(raw.empty());
  EXPECT_EQ(calls, 1);

  for (const auto& invalid :
       std::vector<nlohmann::json>{nlohmann::json::array(),
                                   {{"unknown", 1}},
                                   {{"count", "3"}},
                                   {{"count", 0}},
                                   {{"count", 9}},
                                   {{"label", 7}}}) {
    SCOPED_TRACE(invalid.dump());
    EXPECT_FALSE(parser.Parse(invalid, &error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(calls, 1);
  }
}

TEST(NodeBaseContractsTest,
     ConfigParserKeepsNormalizedJsonIdentityAndOwnsResult) {
  nlohmann::json normalized = {{"count", 4}, {"label", "original"}};
  const nlohmann::json* received = nullptr;
  const NodeConfigParser<NodeParserParameters> parser(
      NodeParserFields(), [&](const nlohmann::json& config,
                              NodeParserParameters* result, std::string*) {
        received = &config;
        result->count = config.at("count").get<int>();
        result->label = config.at("label").get<std::string>();
        return true;
      });
  std::string error = "old error";
  const auto result = parser.ParseNormalized(normalized, &error);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_EQ(received, &normalized);
  EXPECT_TRUE(error.empty());
  normalized["count"] = 8;
  normalized["label"] = "changed after parsing";
  EXPECT_EQ(result->count, 4);
  EXPECT_EQ(result->label, "original");
}

TEST(NodeBaseContractsTest, ConfigParserFailuresNeverReturnPartialParameters) {
  const nlohmann::json normalized = {{"count", 4}, {"label", "value"}};
  for (bool already_normalized : {false, true}) {
    for (int failure = 0; failure < 4; ++failure) {
      SCOPED_TRACE(already_normalized);
      SCOPED_TRACE(failure);
      const NodeConfigParser<NodeParserParameters> parser(
          NodeParserFields(),
          [&](const nlohmann::json&, NodeParserParameters* partial,
              std::string* error) -> bool {
            partial->count = 7;
            partial->label = "partial result";
            if (failure == 0) {
              if (error) *error = "Label is not allowed";
              return false;
            }
            if (failure == 1) return false;
            if (failure == 2)
              throw std::runtime_error("Semantic parser failed");
            throw 42;
          });
      std::string error = "old error";
      const auto result = already_normalized
                              ? parser.ParseNormalized(normalized, &error)
                              : parser.Parse(normalized, &error);
      EXPECT_FALSE(result.has_value());
      EXPECT_FALSE(error.empty());
      EXPECT_NE(error, "old error");
      if (failure == 0) {
        EXPECT_EQ(error, "Label is not allowed");
      }
      if (failure == 2) {
        EXPECT_EQ(error, "Semantic parser failed");
      }
      EXPECT_FALSE(parser.ParseNormalized(normalized).has_value());
    }
  }
  const NodeConfigParser<NodeParserParameters> missing(NodeParserFields(), {});
  std::string error;
  EXPECT_FALSE(missing.ParseNormalized(normalized, &error).has_value());
  EXPECT_FALSE(error.empty());
}

TEST(NodeErrorCodesTest, ActiveBusinessAndAuthoringErrorsAreDistinct) {
  EXPECT_EQ(node_error::control::kInvalidRequest, -1);
  EXPECT_EQ(node_error::text_embedding::kSessionInferenceFailed, -5101);
  EXPECT_EQ(node_error::structured_json_parse::kParseFailed, -6102);
  EXPECT_EQ(node_error::text_template::kRenderedOutputTooLong, -6201);
  EXPECT_EQ(node_error::text_template::kMissingVariable, -6202);
  EXPECT_EQ(node_error::text_rerank::kMissingInput, -7001);

  const std::vector<int> codes = {
      node_error::control::kInvalidRequest,
      node_error::text_chunk::kInvalidUtf8,
      node_error::text_embedding::kSessionInferenceFailed,
      node_error::text_rule_match::kRegexExecutionFailed,
      node_error::structured_json_parse::kParseFailed,
      node_error::text_template::kRenderedOutputTooLong,
      node_error::text_template::kMissingVariable,
      node_error::text_template::kInvalidUtf8,
      node_error::text_rerank::kMissingInput,
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

// 1. Exception Test Node
class ExceptionThrowingNode : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "ExceptionThrowingNode";
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
  plan.normalized_config = nlohmann::json::object();
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

// 2. Require / Publish Helper Test
inline constexpr BlackboardKey<std::string> kTestInputKey{"test_input_key",
                                                          "string"};
inline constexpr BlackboardKey<std::string> kTestOutputKey{"test_output_key",
                                                           "string"};

class HelperTestNode : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "HelperTestNode";
  HelperTestNode() : NodeBase(kNodeType) {}

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
  plan.normalized_config = nlohmann::json::object();
  ASSERT_TRUE(node.Init({&plan, &session_ctx}));

  // Missing input key
  {
    AlgContext ctx;
    int ret = node.Process(&ctx);
    EXPECT_EQ(ret, -9901);
    EXPECT_EQ(ctx.GetErrorCode(), -9901);
    EXPECT_TRUE(ctx.GetErrorMessage().find("missing required input key") !=
                std::string::npos);
  }

  // Success path
  {
    AlgContext ctx;
    ctx.Publish(kTestInputKey, std::string("hello"));
    int ret = node.Process(&ctx);
    EXPECT_EQ(ret, 0);
    const auto* out_val = ctx.Read(kTestOutputKey);
    ASSERT_NE(out_val, nullptr);
    EXPECT_EQ(*out_val, "hello_processed");

    // A second producer invocation in the same request cannot silently
    // overwrite the already published output.
    EXPECT_EQ(node.Process(&ctx),
              static_cast<int>(NodeRuntimeCode::kUnhandledException));
    EXPECT_EQ(*out_val, "hello_processed");
    EXPECT_TRUE(ctx.GetErrorMessage().find("Duplicate output publication") !=
                std::string::npos);
  }

  // Existing key with an incompatible runtime type.
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

class BoundPortProbeNode : public NodeBase {
 public:
  BoundPortProbeNode() : NodeBase("BoundPortProbeNode"), input_("input") {}

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
  plan.normalized_config = nlohmann::json::object();
  plan.ports.push_back({"input", "actual_input", "integer", "1:1", "preserve",
                        "request", PortDirection::kInput});
  SessionContext session_ctx;
  NodeInitContext init_ctx;
  init_ctx.plan = &plan;
  init_ctx.session_ctx = &session_ctx;

  BoundPortProbeNode node;
  EXPECT_FALSE(node.Init(init_ctx));
}

class MultiPortProbeNode : public NodeBase {
 public:
  inline static constexpr auto kFirst = MakeBlackboardKey<TextBatch>("first");
  inline static constexpr auto kOptional =
      MakeBlackboardKey<TextBatch>("optional");
  inline static constexpr auto kCombined =
      MakeBlackboardKey<TextBatch>("combined");
  inline static constexpr auto kEcho = MakeBlackboardKey<TextBatch>("echo");

  MultiPortProbeNode() : NodeBase("MultiPortProbeNode") {}
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
  definition.node_type = "MultiPortProbeNode";
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
  plan.normalized_config = nlohmann::json::object();
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

TEST(NodeBaseContractsTest, OutputBindingRejectsMissingKeysBeforeTypeMismatch) {
  for (int fault = 0; fault < 3; ++fault) {
    SCOPED_TRACE(fault);
    ValidatedNodePlan plan;
    plan.normalized_config = nlohmann::json::object();
    // The runtime base permits unbound inputs; the author contract validates
    // required inputs.
    plan.ports.push_back({"optional", "", "integer", "1:1", "preserve",
                          "request", PortDirection::kInput});
    if (fault != 0) {
      plan.ports.push_back({"combined", fault == 1 ? "" : "actual_combined",
                            "integer", "1:1", "preserve", "request",
                            PortDirection::kOutput});
    }
    plan.ports.push_back({"echo", "actual_echo", "integer", "1:1", "preserve",
                          "request", PortDirection::kOutput});
    SessionContext session;
    MultiPortProbeNode node;
    std::string diagnostic;
    EXPECT_FALSE(node.Init({&plan, &session, &diagnostic}));
    EXPECT_EQ(diagnostic, fault == 2
                              ? "Output port TypeId mismatch for combined "
                                "(expected: TextBatch, bound: integer)"
                              : "Output port is unbound in plan: combined");
    EXPECT_FALSE(node.OptionalIsBound());
    EXPECT_FALSE(node.CombinedIsBound());
    EXPECT_FALSE(node.EchoIsBound());
  }
}

// 3. Unified function authoring with a model capability
class MockAsrModel : public IAsrModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string t = "mock_asr";
    return t;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "asr";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  int Transcribe(const AudioPcmBatch& inputs, TextBatch* outputs,
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
  return MakeBatchSpec(
      InputsOf<MockAsrInputs>{
          Required(kTestAudioInputs.name, &MockAsrInputs::audio)},
      PreservedOutput<TextBatch>(kTestTranscripts.name, kTestAudioInputs.name),
      ModelsOf<MockAsrModels>{Model("transcriber", "bind_model",
                                    &MockAsrModels::transcriber,
                                    "test_asr_model")},
      [](const MockAsrInputs& inputs, const NoParameters&,
         const MockAsrModels& models) {
        return models.transcriber.Transcribe(*inputs.audio);
      });
}
using MockAsrNode = AuthorNode<decltype(MockAsrSpec())>;

TEST(NodeBaseContractsTest, FunctionAsrWorkflow) {
  test_support::RegistryTestAccess::ScopedNodeState state_guard;
  auto definition = MockAsrSpec().BuildDefinition("MockAsrNode");
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      definition.node_type,
      []() {
        return std::make_unique<MockAsrNode>("MockAsrNode", MockAsrSpec());
      },
      definition));

  SessionContext session_ctx;
  auto model = std::make_shared<MockAsrModel>();
  session_ctx.GetModelManager().RegisterModel("test_asr_model", model,
                                              "test-v1");

  MockAsrNode node("MockAsrNode", MockAsrSpec());
  ASSERT_TRUE(InitNodeForTest(node, nlohmann::json::object(), &session_ctx));

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
