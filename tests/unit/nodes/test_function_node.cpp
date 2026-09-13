#include <gtest/gtest.h>

#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "engine/model_interface.h"
#include "nodes/authoring.h"
#include "nodes/node_config_parser.h"
#include "nodes/node_error_codes.h"
#include "tests/support/node_harness.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {
namespace {

struct ComplexParams {
  std::string prefix;
  int limit = 0;
};

auto ComplexConfig() {
  NodeConfigParser<ComplexParams> parser(
      {ConfigFieldDefinition{"nested", ConfigValueKind::kObject, true}},
      [](const nlohmann::json& config, ComplexParams* params,
         std::string* error) {
        const auto& nested = config.at("nested");
        if (!nested.contains("prefix") || !nested["prefix"].is_string()) {
          if (error) *error = "nested.prefix must be text";
          return false;
        }
        params->prefix = nested["prefix"].get<std::string>();
        params->limit = -1;  // The basic member binding must override this.
        return true;
      });
  auto config = Parameters<ComplexParams>({
      Field("limit", &ComplexParams::limit).Default(8).Range(1, 20),
  });
  config.WithParser(std::move(parser));
  config.Validate([](const ComplexParams& params, std::string* error) {
    if (params.prefix.size() > static_cast<size_t>(params.limit)) {
      if (error) *error = "prefix exceeds limit";
      return false;
    }
    return true;
  });
  config.ValidateBindings([](const ComplexParams& params,
                             const std::unordered_set<std::string>& ports,
                             std::string* error) {
    if (!params.prefix.empty() && !ports.count("context")) {
      if (error) *error = "prefix requires context";
      return false;
    }
    return true;
  });
  return config;
}

struct ComplexInputs {
  const TextBatch* input = nullptr;
  const TextBatch* context = nullptr;
};

auto ComplexSpec() {
  return MakeBatchSpec(
      InputsOf<ComplexInputs>({Required("input", &ComplexInputs::input),
                               Optional("context", &ComplexInputs::context)}),
      PreservedOutput<TextBatch>("output", "input"), ComplexConfig(),
      ModelsOf<NoModels>{},
      [](const ComplexInputs& input, const ComplexParams& params,
         const NoModels&) -> NodeResult<TextBatch> {
        return MapPayloads(*input.input, [&](const std::string& text) {
          return params.prefix + text;
        });
      });
}
REGISTER_FUNCTION_NODE(ComplexParserNode, ComplexSpec());

int local_logic_constructions = 0;
class RequestLocalLogic {
 public:
  RequestLocalLogic() { ++local_logic_constructions; }
  NodeResult<TextBatch> Run(const ComplexInputs& inputs, const NoParameters&,
                            const NoModels&) {
    return MapPayloads(*inputs.input, [&](const std::string& text) {
      accumulated_ += text;
      return accumulated_;
    });
  }

 private:
  std::string accumulated_;
};

auto RequestLocalSpec() {
  return MakeBatchSpec(
      InputsOf<ComplexInputs>({Required("input", &ComplexInputs::input)}),
      PreservedOutput<TextBatch>("output", "input"), Parameters<NoParameters>{},
      ModelsOf<NoModels>{}, &RequestLocalLogic::Run);
}
REGISTER_FUNCTION_NODE(RequestLocalNode, RequestLocalSpec());

struct CleanParams {
  std::string prefix;
};

auto CleanConfig() {
  return Parameters<CleanParams>({
      Field("prefix", &CleanParams::prefix)
          .Default(std::string{})
          .Description("Prefix for text"),
  });
}

std::string CleanTextFn(const std::string& in, const CleanParams& params) {
  std::string out = in;
  while (!out.empty() && out.back() == '\n') {
    out.pop_back();
  }
  return params.prefix + out;
}

auto CleanSpec() {
  return MakeMapSpec(Input<TextBatch>("input"), Output<TextBatch>("output"),
                     CleanConfig(), &CleanTextFn)
      .Description("Clean text map node");
}

REGISTER_FUNCTION_NODE(CleanTextMapNode, CleanSpec());

// Node returning NodeResult with failure on specific keyword
NodeResult<std::string> FailableCleanFn(const std::string& in) {
  if (in == "FAIL") {
    return NodeResult<std::string>::Failure(
        NodeErrorKind::kBusinessError, "Forced failure on keyword FAIL", -9999);
  }
  return NodeResult<std::string>::Success("processed:" + in);
}

auto FailableSpec() {
  return MakeMapSpec(Input<TextBatch>("input"), Output<TextBatch>("output"),
                     &FailableCleanFn)
      .Description("Failable map node");
}

REGISTER_FUNCTION_NODE(FailableMapNode, FailableSpec());

// Node without parameters returning plain string
std::string UpperFn(const std::string& in) {
  std::string out = in;
  for (auto& c : out) c = static_cast<char>(toupper(c));
  return out;
}

auto UpperSpec() {
  return MakeMapSpec(Input<TextBatch>("input"), Output<TextBatch>("output"),
                     &UpperFn)
      .Description("Uppercase map node");
}

REGISTER_FUNCTION_NODE(UpperMapNode, UpperSpec());

// ---------------------------------------------------------------------------
// Batch Fixtures
// ---------------------------------------------------------------------------

class CountingMockLlmModel final : public ILlmModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string t = "counting_mock_llm";
    return t;
  }
  const std::string& Capability() const noexcept override {
    static const std::string cap = "llm";
    return cap;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 8; }

  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs) noexcept override {
    ++call_count;
    last_options = options;
    last_prompts = prompts;
    if (fail_first_n > 0 && call_count <= fail_first_n) {
      if (outputs) outputs->clear();
      return -8901;
    }
    if (always_fail) {
      if (outputs) outputs->clear();
      return -8902;
    }
    if (outputs) {
      outputs->clear();
      for (const auto& item : prompts) {
        outputs->emplace_back(item.req_id, item.sub_id, "ans:" + item.data);
      }
      if (return_wrong_count && !outputs->empty()) outputs->pop_back();
      if (corrupt_provenance && !outputs->empty()) ++(*outputs)[0].sub_id;
    }
    return 0;
  }

  mutable int call_count = 0;
  mutable GenerateOptions last_options;
  mutable TextBatch last_prompts;
  int fail_first_n = 0;
  bool always_fail = false;
  bool return_wrong_count = false;
  bool corrupt_provenance = false;
};

struct AnswerInputs {
  const TextBatch* questions = nullptr;
  const TextBatch* context = nullptr;
};

struct AnswerModels {
  LlmCall llm;
};

struct AnswerParams {
  int max_revisions = 1;
};

auto AnswerConfig() {
  return Parameters<AnswerParams>({
      Field("max_revisions", &AnswerParams::max_revisions).Default(1),
  });
}

NodeResult<TextBatch> AnswerBatchFn(const AnswerInputs& inputs,
                                    const AnswerParams& params,
                                    const AnswerModels& models) {
  if (!inputs.questions || inputs.questions->empty()) {
    return NodeResult<TextBatch>::Success(TextBatch{});
  }

  std::string ctx_str;
  if (inputs.context && !inputs.context->empty()) {
    ctx_str = "[CTX:" + (*inputs.context)[0].data + "] ";
  }

  auto prompts = MapPayloads(*inputs.questions, [&](const std::string& text) {
    return ctx_str + text;
  });

  GenerateOptions opts;
  opts.temperature = 0.5f;
  auto result = models.llm.Generate(prompts, opts);
  for (int round = 0; round < params.max_revisions && !result.ok(); ++round) {
    result = models.llm.Generate(prompts, opts);
  }
  if (!result.ok()) return result;

  return MapPayloads(result.value(),
                     [](const std::string& ans) { return "final:" + ans; });
}

auto AnswerBatchSpec() {
  return MakeBatchSpec(InputsOf<AnswerInputs>({
                           Required("questions", &AnswerInputs::questions),
                           Optional("context", &AnswerInputs::context,
                                    InputFlow::AggregateByRequest),
                       }),
                       PreservedOutput<TextBatch>("output", "questions"),
                       AnswerConfig(),
                       ModelsOf<AnswerModels>({
                           Llm("generator", "bind_model", &AnswerModels::llm),
                       }),
                       &AnswerBatchFn)
      .Description("Batch answering node with retry");
}

REGISTER_FUNCTION_NODE(AnswerBatchNode, AnswerBatchSpec());

struct TwoStageModels {
  LlmCall draft;
  LlmCall revise;
};

auto TwoStageSpec() {
  return MakeBatchSpec(
      InputsOf<AnswerInputs>({Required("questions", &AnswerInputs::questions)}),
      PreservedOutput<TextBatch>("output", "questions"),
      ModelsOf<TwoStageModels>({
          Llm("draft", "draft_model", &TwoStageModels::draft),
          Llm("revise", "revise_model", &TwoStageModels::revise),
      }),
      [](const AnswerInputs& inputs, const NoParameters&,
         const TwoStageModels& models) -> NodeResult<TextBatch> {
        auto draft = models.draft.Generate(*inputs.questions);
        if (!draft.ok()) return draft;
        return models.revise.Generate(draft.value());
      });
}

class TwoStageHarnessNode : public AuthorNode<decltype(TwoStageSpec())> {
 public:
  static constexpr const char* kNodeType = "TwoStageHarnessNode";
  TwoStageHarnessNode() : AuthorNode(kNodeType, TwoStageSpec()) {}
};

NodeDefinition TwoStageDefinition() {
  auto definition =
      TwoStageSpec().BuildDefinition(TwoStageHarnessNode::kNodeType);
  for (auto& field : definition.config_fields) {
    if (field.name == "draft_model" || field.name == "revise_model") {
      field.required = false;
      field.default_value = "default_" + field.name;
    }
  }
  return definition;
}

REGISTER_NODE_WITH_DEFINITION(TwoStageHarnessNode, TwoStageDefinition());

// Object logic batch node
struct LogicAnswerParams {
  std::string tag = "logic";
};

auto LogicAnswerConfig() {
  return Parameters<LogicAnswerParams>({
      Field("tag", &LogicAnswerParams::tag).Default("logic"),
  });
}

class AnswerLogic {
 public:
  NodeResult<TextBatch> Run(const AnswerInputs& inputs,
                            const LogicAnswerParams& params,
                            const AnswerModels& models) const {
    if (!inputs.questions || inputs.questions->empty()) {
      return NodeResult<TextBatch>::Success(TextBatch{});
    }
    auto prompts = MapPayloads(*inputs.questions, [&](const std::string& text) {
      return params.tag + ":" + text;
    });
    return models.llm.Generate(prompts);
  }
};

auto LogicBatchSpec() {
  return MakeBatchSpec(InputsOf<AnswerInputs>({
                           Required("questions", &AnswerInputs::questions),
                       }),
                       PreservedOutput<TextBatch>("output", "questions"),
                       LogicAnswerConfig(),
                       ModelsOf<AnswerModels>({
                           Llm("generator", "bind_model", &AnswerModels::llm),
                       }),
                       &AnswerLogic::Run)
      .Description("Logic class batch node");
}

REGISTER_FUNCTION_NODE(LogicBatchNode, LogicBatchSpec());

// LLM Text shortcut node
struct ShortcutParams {
  std::string suffix = "!";
};

auto ShortcutConfig() {
  return Parameters<ShortcutParams>({
      Field("suffix", &ShortcutParams::suffix).Default("!"),
  });
}

auto ShortcutSpec() {
  return MakeLlmTextSpec(
      Input<TextBatch>("prompt"), Output<TextBatch>("text"), ShortcutConfig(),
      [](const std::string& in, const ShortcutParams&) {
        return "prompt:" + in;
      },
      [](const std::string& out, const ShortcutParams& p) {
        return out + p.suffix;
      });
}

REGISTER_FUNCTION_NODE(LlmShortcutNode, ShortcutSpec());

int formatting_calls = 0;
NodeResult<std::string> FormatUntilSecond(const std::string& text) {
  ++formatting_calls;
  if (text == "ans:second") {
    return NodeResult<std::string>::Failure(NodeErrorKind::kBusinessError,
                                            "second answer rejected", -9876);
  }
  return "formatted:" + text;
}

struct ConstOnlyFormatter {
  NodeResult<std::string> operator()(const std::string& text) const {
    return FormatUntilSecond(text);
  }
  NodeResult<std::string> operator()(std::string&) const = delete;
};

struct ConstOnlyParameterFormatter {
  NodeResult<std::string> operator()(const std::string& text,
                                     const ShortcutParams&) const {
    return FormatUntilSecond(text);
  }
  NodeResult<std::string> operator()(std::string&,
                                     const ShortcutParams&) const = delete;
};

auto FailingFormatSpec() {
  return MakeLlmTextSpec(
      Input<TextBatch>("input"), Output<TextBatch>("output"),
      [](const std::string& text) { return text; }, ConstOnlyFormatter{});
}
REGISTER_FUNCTION_NODE(FailingFormatNode, FailingFormatSpec());

auto FailingParameterFormatSpec() {
  return MakeLlmTextSpec(
      Input<TextBatch>("input"), Output<TextBatch>("output"), ShortcutConfig(),
      [](const std::string& text) { return text; },
      ConstOnlyParameterFormatter{});
}
REGISTER_FUNCTION_NODE(FailingParameterFormatNode,
                       FailingParameterFormatSpec());

static std::string StarterBuildPrompt(const std::string& text) {
  return "prompt:" + text;
}
static std::string StarterFormatAnswer(const std::string& text) {
  return "formatted:" + text;
}

auto StarterLlmTestSpec() {
  return MakeLlmTextSpec(Input<TextBatch>("input"), Output<TextBatch>("output"),
                         &StarterBuildPrompt, &StarterFormatAnswer);
}
REGISTER_FUNCTION_NODE(StarterLlmTestNode, StarterLlmTestSpec());

struct TextToScoreInputs {
  const TextBatch* texts = nullptr;
};

auto TextToScoreSpec() {
  return MakeBatchSpec(
      InputsOf<TextToScoreInputs>({
          Required("texts", &TextToScoreInputs::texts),
      }),
      PreservedOutput<ScoreBatch>("scores", "texts"),
      Parameters<NoParameters>{}, ModelsOf<NoModels>({}),
      [](const TextToScoreInputs& in, const NoParameters&,
         const NoModels&) -> NodeResult<ScoreBatch> {
        ScoreBatch scores;
        if (!in.texts) return NodeResult<ScoreBatch>::Success(scores);
        scores.reserve(in.texts->size());
        for (const auto& item : *in.texts) {
          scores.emplace_back(item.req_id, item.sub_id, 1.0f);
        }
        return NodeResult<ScoreBatch>::Success(std::move(scores));
      });
}
REGISTER_FUNCTION_NODE(TextToScoreNode, TextToScoreSpec());

struct ContextPollutionInputs {
  const TextBatch* input = nullptr;
};

auto FailingAfterPublishSpec() {
  return MakeBatchSpec(InputsOf<ContextPollutionInputs>({
                           Required("input", &ContextPollutionInputs::input),
                       }),
                       PreservedOutput<TextBatch>("output", "input"),
                       Parameters<NoParameters>{}, ModelsOf<NoModels>({}),
                       [](const ContextPollutionInputs&, const NoParameters&,
                          const NoModels&) -> NodeResult<TextBatch> {
                         return NodeResult<TextBatch>::Failure(
                             NodeErrorKind::kBusinessError, "forced error");
                       });
}
REGISTER_FUNCTION_NODE(FailingAfterPublishNode, FailingAfterPublishSpec());

struct BindingTestParams {
  bool check_mask = false;
};

struct BindingTestInputs {
  const TextBatch* texts = nullptr;
  const TextBatch* mask = nullptr;
};

inline auto BindingTestSpec() {
  return MakeBatchSpec(
      InputsOf<BindingTestInputs>({
          Required("texts", &BindingTestInputs::texts),
          Optional("mask", &BindingTestInputs::mask),
      }),
      PreservedOutput<TextBatch>("output", "texts"),
      Parameters<BindingTestParams>(
          {
              Field("check_mask", &BindingTestParams::check_mask)
                  .Default(false),
          })
          .ValidateBindings([](const BindingTestParams& p,
                               const std::unordered_set<std::string>& conn,
                               std::string* err) {
            if (p.check_mask && conn.count("mask") == 0) {
              if (err) *err = "check_mask requires mask port to be connected";
              return false;
            }
            return true;
          }),
      ModelsOf<NoModels>{},
      [](const BindingTestInputs& in, const BindingTestParams&,
         const NoModels&) -> NodeResult<TextBatch> {
        return NodeResult<TextBatch>::Success(*in.texts);
      });
}
REGISTER_FUNCTION_NODE(BindingTestNode, BindingTestSpec());

struct BindingMapParams {
  bool require_extra = false;
};

inline auto BindingMapSpec() {
  return MakeMapSpec(
      Input<TextBatch>("input"), Output<TextBatch>("output"),
      Parameters<BindingMapParams>(
          {
              Field("require_extra", &BindingMapParams::require_extra)
                  .Default(false),
          })
          .ValidateBindings([](const BindingMapParams& p,
                               const std::unordered_set<std::string>& conn,
                               std::string* err) {
            if (p.require_extra && conn.count("extra") == 0) {
              if (err)
                *err = "require_extra requires extra port to be connected";
              return false;
            }
            return true;
          }),
      [](const std::string& in, const BindingMapParams&) { return in; });
}
REGISTER_FUNCTION_NODE(BindingMapNode, BindingMapSpec());

}  // namespace

// ===========================================================================
// Tests
// ===========================================================================

// A1: Map 多请求/非零 sub_id 保序、输入未修改、空批次不调用；中间项失败无输出
TEST(FunctionNodeTest, MapPreservesOrderingAndProvenanceAcrossRequests) {
  NodeHarness harness("CleanTextMapNode");
  harness.Config({{"prefix", "PRE:"}});

  TextBatch input_batch;
  input_batch.emplace_back(1001, 0, "first\n");
  input_batch.emplace_back(1001, 1, "second\n\n");
  input_batch.emplace_back(1002, 5, "third");

  harness.TextInputWithBatch("input", input_batch);

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();

  const auto* output_batch = result.Output<TextBatch>("output");
  ASSERT_NE(output_batch, nullptr);
  ASSERT_EQ(output_batch->size(), 3u);

  EXPECT_EQ((*output_batch)[0].req_id, 1001u);
  EXPECT_EQ((*output_batch)[0].sub_id, 0u);
  EXPECT_EQ((*output_batch)[0].data, "PRE:first");

  EXPECT_EQ((*output_batch)[1].req_id, 1001u);
  EXPECT_EQ((*output_batch)[1].sub_id, 1u);
  EXPECT_EQ((*output_batch)[1].data, "PRE:second");

  EXPECT_EQ((*output_batch)[2].req_id, 1002u);
  EXPECT_EQ((*output_batch)[2].sub_id, 5u);
  EXPECT_EQ((*output_batch)[2].data, "PRE:third");

  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"PRE:first", "PRE:second", "PRE:third"}));
}

TEST(FunctionNodeTest, EmptyBatchReturnsEmptyWithoutCallingFunction) {
  NodeHarness harness("CleanTextMapNode");
  harness.TextInput("input", {});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_TRUE(result.TextValues("output").empty());
}

TEST(FunctionNodeTest, IntermediateFailureProducesNoOutput) {
  NodeHarness harness("FailableMapNode");
  harness.TextInput("input", {"ok1", "FAIL", "ok3"});

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.process_code(), -9999);

  const auto* out = result.Output<TextBatch>("output");
  EXPECT_EQ(out, nullptr);
}

// A5: 重复 output key 保留旧值且失败；最终业务失败不发布新输出
TEST(FunctionNodeTest, DuplicateOutputKeyFailsAndKeepsExistingValue) {
  auto node = NodeRegistry::Instance().Create("UpperMapNode");
  ASSERT_NE(node, nullptr);

  NodeInitContext init_ctx;
  SessionContext session_ctx;
  init_ctx.session_ctx = &session_ctx;
  ASSERT_TRUE(node->Init(init_ctx));

  AlgContext ctx;
  TextBatch existing;
  existing.emplace_back(99, 99, "ORIGINAL_PRE_EXISTING");
  ASSERT_TRUE(ctx.Publish("output", existing));

  TextBatch input;
  input.emplace_back(1, 0, "hello");
  ASSERT_TRUE(ctx.Publish("input", input));

  int ret = node->Process(&ctx);
  EXPECT_NE(ret, 0);

  const auto* current = ctx.Read<TextBatch>("output");
  ASSERT_NE(current, nullptr);
  ASSERT_EQ(current->size(), 1u);
  EXPECT_EQ((*current)[0].data, "ORIGINAL_PRE_EXISTING");
}

// A6: Spec/Definition 求值抛异常、重复成员、非法默认值不 abort，注册失败有诊断
TEST(FunctionNodeTest,
     RegisterWithDefinitionFactoryHandlesExceptionsGracefully) {
  bool registered = NodeRegistry::Instance().RegisterWithDefinitionFactory(
      "ThrowingNodeTest", []() { return nullptr; },
      []() -> NodeDefinition {
        throw std::runtime_error("simulated spec definition exception");
      });

  EXPECT_FALSE(registered);
  EXPECT_TRUE(NodeRegistry::Instance().HasConflict());

  bool found_message = false;
  for (const auto& err : NodeRegistry::Instance().GetConflictErrors()) {
    if (err.find("simulated spec definition exception") != std::string::npos) {
      found_message = true;
      break;
    }
  }
  EXPECT_TRUE(found_message);
  NodeRegistry::Instance().ClearConflictForTesting();
}

TEST(FunctionNodeTest, NodeWithoutParametersOperatesCorrectly) {
  NodeHarness harness("UpperMapNode");
  harness.TextInput("input", {"hello", "world"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"HELLO", "WORLD"}));
}

TEST(FunctionNodeTest, HarnessAllowsModelSlotsToShareOneModel) {
  auto model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("TwoStageHarnessNode");
  harness.Config({{"draft_model", "shared"}, {"revise_model", "shared"}});
  harness.BindModel("shared", model);
  harness.TextInput("questions", {"hello"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"ans:ans:hello"}));
  EXPECT_EQ(model->call_count, 2);
}

TEST(FunctionNodeTest, HarnessUsesDefinitionDefaultModelReferences) {
  auto draft = std::make_shared<CountingMockLlmModel>();
  auto revise = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("TwoStageHarnessNode");
  harness.BindModel("default_draft_model", draft);
  harness.BindModel("default_revise_model", revise);
  harness.TextInput("questions", {"hello"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"ans:ans:hello"}));
  EXPECT_EQ(draft->call_count, 1);
  EXPECT_EQ(revise->call_count, 1);
}

// A2: Batch 空输入调用 Run；optional 未连/已连空/已连缺值区分；anchor 错误失败
TEST(FunctionNodeTest, BatchEmptyInputCallsRunAndSucceeds) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("AnswerBatchNode");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("questions", {});
  harness.TextInput("context", {});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_TRUE(result.TextValues("output").empty());
  EXPECT_EQ(mock_model->call_count, 0);
}

TEST(FunctionNodeTest, BatchOptionalPortUnconnectedGivesNullptr) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("AnswerBatchNode");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_model);
  harness.OmitPortFromPlan("context");
  harness.TextInput("questions", {"What is AI?"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  // Without context, output won't have [CTX:...]
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"final:ans:What is AI?"}));
  EXPECT_EQ(mock_model->call_count, 1);
}

TEST(FunctionNodeTest, BatchOptionalPortConnectedProvidesContext) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("AnswerBatchNode");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("questions", {"What is AI?"});
  harness.TextInput("context", {"Document knowledge"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{
                "final:ans:[CTX:Document knowledge] What is AI?"}));
  EXPECT_EQ(mock_model->call_count, 1);
}

TEST(FunctionNodeTest, BatchOptionalPortConnectedButMissingFailsClosed) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("AnswerBatchNode");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("questions", {"What is AI?"});
  // context is in plan, but NOT supplied to AlgContext

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.diagnostic().find("context"), std::string::npos);
}

TEST(FunctionNodeTest, BatchUnplannedInitWithOptionalPortFails) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("AnswerBatchNode");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_model);
  harness.DisablePlan();

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.init_failed());
  EXPECT_NE(result.diagnostic().find("ValidatedNodePlan"), std::string::npos);
}

// M1: 一次非空批次一次 Model 调用；空输入零调用；options 完整传递
TEST(FunctionNodeTest, BatchSingleModelCallAndOptionsPassing) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("AnswerBatchNode");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("questions", {"Q1", "Q2", "Q3"});
  harness.OmitPortFromPlan("context");

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(mock_model->call_count, 1);
  EXPECT_FLOAT_EQ(mock_model->last_options.temperature, 0.5f);
  EXPECT_EQ(mock_model->last_prompts.size(), 3u);
}

// M2: Model 返回失败、少项、乱序、错 req/sub 全部失败无输出
TEST(FunctionNodeTest, BatchModelFailuresFailClosedWithoutOutput) {
  // Case 1: Model returns inference failure
  {
    auto mock_model = std::make_shared<CountingMockLlmModel>();
    mock_model->always_fail = true;
    NodeHarness harness("AnswerBatchNode");
    harness.Config({{"bind_model", "test_llm"}, {"max_revisions", 0}});
    harness.BindModel("test_llm", mock_model);
    harness.TextInput("questions", {"Q1"});
    harness.OmitPortFromPlan("context");

    auto result = harness.Run();
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
  }

  // Case 2: Model returns wrong count (fewer items)
  {
    auto mock_model = std::make_shared<CountingMockLlmModel>();
    mock_model->return_wrong_count = true;
    NodeHarness harness("AnswerBatchNode");
    harness.Config({{"bind_model", "test_llm"}, {"max_revisions", 0}});
    harness.BindModel("test_llm", mock_model);
    harness.TextInput("questions", {"Q1", "Q2"});
    harness.OmitPortFromPlan("context");

    auto result = harness.Run();
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
  }

  // Case 3: Model returns corrupt provenance
  {
    auto mock_model = std::make_shared<CountingMockLlmModel>();
    mock_model->corrupt_provenance = true;
    NodeHarness harness("AnswerBatchNode");
    harness.Config({{"bind_model", "test_llm"}, {"max_revisions", 0}});
    harness.BindModel("test_llm", mock_model);
    harness.TextInput("questions", {"Q1"});
    harness.OmitPortFromPlan("context");

    auto result = harness.Run();
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
  }
}

// M3: 条件二次调用、失败后显式重试成功、耗尽后失败；Context 最终错误一致
TEST(FunctionNodeTest, BatchModelCallRetrySucceedsWithoutPollutingContext) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  mock_model->fail_first_n = 1;  // Fails on attempt 1, succeeds on attempt 2

  NodeHarness harness("AnswerBatchNode");
  harness.Config({{"bind_model", "test_llm"}, {"max_revisions", 2}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("questions", {"Q1"});
  harness.OmitPortFromPlan("context");

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(mock_model->call_count, 2);
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"final:ans:Q1"}));

  // Ensure AlgContext is completely clean!
  EXPECT_TRUE(result.Context()->IsOk());
}

// Ordinary logic object Run
TEST(FunctionNodeTest, BatchLogicClassExecutesPerRequest) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("LogicBatchNode");
  harness.Config({{"bind_model", "test_llm"}, {"tag", "my_tag"}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("questions", {"hello"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"ans:my_tag:hello"}));
}

// MakeLlmTextSpec shortcut
TEST(FunctionNodeTest,
     LlmFormattingFailureDoesNotPublishPartiallyFormattedBatch) {
  for (const char* name : {"FailingFormatNode", "FailingParameterFormatNode"}) {
    SCOPED_TRACE(name);
    formatting_calls = 0;
    auto model = std::make_shared<CountingMockLlmModel>();
    NodeHarness harness(name);
    harness.Config({{"bind_model", "formatter_llm"}});
    harness.BindModel("formatter_llm", model);
    harness.TextInput("input", {"first", "second", "third"});
    auto result = harness.Run();
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.init_failed()) << result.diagnostic();
    EXPECT_EQ(result.process_code(), -9876);
    EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
    EXPECT_EQ(formatting_calls,
              2);  // First succeeded; second stopped the batch.
    EXPECT_EQ(model->call_count, 1);
  }
}

TEST(FunctionNodeTest, LlmShortcutNodeExecutesPipeline) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("LlmShortcutNode");
  harness.Config({{"bind_model", "test_llm"}, {"suffix", "!!!"}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("prompt", {"world"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  // BuildPrompt: prompt:world -> Llm: ans:prompt:world -> FormatAnswer:
  // ans:prompt:world!!!
  EXPECT_EQ(result.TextValues("text"),
            (std::vector<std::string>{"ans:prompt:world!!!"}));
}

TEST(FunctionNodeTest, FourArgMakeLlmTextSpecExecutesProperly) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("StarterLlmTestNode");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("input", {"hello"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"formatted:ans:prompt:hello"}));
}

TEST(FunctionNodeTest, BatchCrossTypeOutputAlignmentSucceeds) {
  NodeHarness harness("TextToScoreNode");
  harness.TextInput("texts", {"query1", "query2"});
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* scores = result.Output<ScoreBatch>("scores");
  ASSERT_NE(scores, nullptr);
  ASSERT_EQ(scores->size(), 2U);
  EXPECT_EQ((*scores)[0].req_id, 101U);
  EXPECT_EQ((*scores)[0].data, 1.0f);
  EXPECT_EQ((*scores)[1].data, 1.0f);
}

TEST(FunctionNodeTest, NodeHarnessFailsInitOnInvalidConfig) {
  NodeHarness harness("LlmShortcutNode");
  harness.Config({{"bind_model", "test_llm"}, {"unknown_field", 123}});
  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.init_failed());
  EXPECT_NE(result.diagnostic().find("Configuration validation failed"),
            std::string::npos);
}

TEST(FunctionNodeTest, ProcessFailedPreservesAlgContextForOutputInspection) {
  NodeHarness harness("FailingAfterPublishNode");
  harness.TextInput("input", {"item1"});
  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(result.init_failed());
  EXPECT_EQ(result.process_code(), node_error::author_node::kBusinessError);
  ASSERT_NE(result.Context(), nullptr);
  EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
}

TEST(FunctionNodeTest, BindingValidationEnforcedInInitAndHarness) {
  // Map node test
  const auto map_def = PipelineCatalog::FindNode("BindingMapNode");
  ASSERT_TRUE(map_def.has_value());
  ASSERT_TRUE(map_def->validate_config);

  // 1. Map Definition preflight rejects configuration when extra port missing
  std::string map_diag;
  EXPECT_FALSE(map_def->validate_config({{"require_extra", true}}, {"input"},
                                        &map_diag));
  EXPECT_NE(map_diag.find("require_extra requires extra port"),
            std::string::npos);

  // 2. Map Unplanned Init fails when extra port missing
  auto map_unplanned = NodeRegistry::Instance().Create("BindingMapNode");
  ASSERT_NE(map_unplanned, nullptr);
  nlohmann::json invalid_map_cfg = {{"require_extra", true}};
  SessionContext session_ctx;
  std::string map_init_diag;
  NodeInitContext init_ctx_map_unplanned;
  init_ctx_map_unplanned.config = &invalid_map_cfg;
  init_ctx_map_unplanned.session_ctx = &session_ctx;
  init_ctx_map_unplanned.diagnostic = &map_init_diag;
  init_ctx_map_unplanned.plan = nullptr;
  EXPECT_FALSE(map_unplanned->Init(init_ctx_map_unplanned));
  EXPECT_NE(map_init_diag.find("require_extra requires extra port"),
            std::string::npos);

  // 3. Map Manual Plan Init fails when extra port missing
  auto map_manual = NodeRegistry::Instance().Create("BindingMapNode");
  ASSERT_NE(map_manual, nullptr);
  ValidatedNodePlan manual_map_plan;
  manual_map_plan.normalized_config = invalid_map_cfg;
  manual_map_plan.ports.push_back(
      {"input", "bk_input", BlackboardTypeTraits<TextBatch>::TypeName(), "1:1",
       "preserve", "request", PortDirection::kInput});
  manual_map_plan.ports.push_back(
      {"output", "bk_output", BlackboardTypeTraits<TextBatch>::TypeName(),
       "1:1", "preserve", "request", PortDirection::kOutput});
  std::string manual_map_diag;
  NodeInitContext init_ctx_map_manual;
  init_ctx_map_manual.config = &invalid_map_cfg;
  init_ctx_map_manual.session_ctx = &session_ctx;
  init_ctx_map_manual.diagnostic = &manual_map_diag;
  init_ctx_map_manual.plan = &manual_map_plan;
  EXPECT_FALSE(map_manual->Init(init_ctx_map_manual));
  EXPECT_NE(manual_map_diag.find("require_extra requires extra port"),
            std::string::npos);

  // 4. Batch node tests
  const auto def = PipelineCatalog::FindNode("BindingTestNode");
  ASSERT_TRUE(def.has_value());
  ASSERT_TRUE(def->validate_config);

  // Batch Definition preflight rejects configuration when mask is missing
  std::string diag;
  EXPECT_FALSE(def->validate_config({{"check_mask", true}}, {"texts"}, &diag));
  EXPECT_NE(diag.find("check_mask requires mask port"), std::string::npos);

  // Batch Manual Plan Init fails when mask is not connected
  auto node_manual = NodeRegistry::Instance().Create("BindingTestNode");
  ASSERT_NE(node_manual, nullptr);
  ValidatedNodePlan manual_plan;
  nlohmann::json invalid_cfg = {{"check_mask", true}};
  manual_plan.normalized_config = invalid_cfg;
  manual_plan.ports.push_back(
      {"texts", "bk_texts", BlackboardTypeTraits<TextBatch>::TypeName(), "1:1",
       "preserve", "request", PortDirection::kInput});
  manual_plan.ports.push_back(
      {"output", "bk_output", BlackboardTypeTraits<TextBatch>::TypeName(),
       "1:1", "preserve", "request", PortDirection::kOutput});
  std::string manual_diag;
  NodeInitContext init_ctx_manual;
  init_ctx_manual.config = &invalid_cfg;
  init_ctx_manual.session_ctx = &session_ctx;
  init_ctx_manual.diagnostic = &manual_diag;
  init_ctx_manual.plan = &manual_plan;
  EXPECT_FALSE(node_manual->Init(init_ctx_manual));
  EXPECT_NE(manual_diag.find("check_mask requires mask port"),
            std::string::npos);

  // 5. NodeHarness fails Init when optional port is omitted
  NodeHarness harness_fail("BindingTestNode");
  harness_fail.Config({{"check_mask", true}});
  harness_fail.OmitPortFromPlan("mask");
  harness_fail.TextInput("texts", {"hello"});
  auto result_fail = harness_fail.Run();
  EXPECT_FALSE(result_fail.ok());
  EXPECT_TRUE(result_fail.init_failed());
  EXPECT_NE(result_fail.diagnostic().find("check_mask requires mask port"),
            std::string::npos);

  // 6. NodeHarness succeeds when mask is connected
  NodeHarness harness_ok("BindingTestNode");
  harness_ok.Config({{"check_mask", true}});
  harness_ok.TextInput("texts", {"hello"});
  harness_ok.TextInput("mask", {"m1"});
  auto result_ok = harness_ok.Run();
  EXPECT_TRUE(result_ok.ok()) << result_ok.diagnostic();
  EXPECT_EQ(result_ok.TextValues("output"),
            (std::vector<std::string>{"hello"}));
}

TEST(FunctionNodeTest, ComplexParserMatchesPreflightInitAndOwnsConfiguration) {
  const auto definition = PipelineCatalog::FindNode("ComplexParserNode");
  ASSERT_TRUE(definition.has_value());
  ASSERT_TRUE(definition->validate_config);
  const nlohmann::json good = {{"nested", {{"prefix", "tag:"}}}, {"limit", 8}};
  for (int fault = 0; fault < 4; ++fault) {
    SCOPED_TRACE(fault);
    auto config = good;
    if (fault == 1) config["nested"]["prefix"] = 123;
    if (fault == 2) config["limit"] = 2;
    const std::unordered_set<std::string> ports =
        fault == 3 ? std::unordered_set<std::string>{"input"}
                   : std::unordered_set<std::string>{"input", "context"};
    std::string preflight_error;
    EXPECT_EQ(definition->validate_config(config, ports, &preflight_error),
              fault == 0);
    ValidatedNodePlan plan;
    plan.normalized_config = config;
    for (const auto& port : ports) {
      plan.ports.push_back({port, port, "TextBatch", "1:1", "preserve",
                            "request", PortDirection::kInput});
    }
    plan.ports.push_back({"output", "output", "TextBatch", "1:1", "preserve",
                          "request", PortDirection::kOutput});
    auto node = NodeRegistry::Instance().Create("ComplexParserNode");
    ASSERT_NE(node, nullptr);
    SessionContext session;
    std::string init_error;
    NodeInitContext init{&plan, &config, &session};
    init.diagnostic = &init_error;
    EXPECT_EQ(node->Init(init), fault == 0);
    if (fault != 0) {
      EXPECT_FALSE(preflight_error.empty());
      EXPECT_EQ(init_error, preflight_error);
      continue;
    }
    // Neither the caller's document nor the plan may remain the Params owner.
    config["nested"]["prefix"] = "changed:";
    plan.normalized_config["nested"]["prefix"] = "changed:";
    AlgContext context;
    context.Publish("input", TextBatch{{7, 2, "hello"}});
    context.Publish("context", TextBatch{});
    ASSERT_EQ(node->Process(&context), 0);
    const auto* output = context.Read<TextBatch>("output");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 1u);
    EXPECT_EQ((*output)[0].data, "tag:hello");
  }
}

TEST(FunctionNodeTest, LogicObjectIsRecreatedForEachProcessOnSameNode) {
  auto node = NodeRegistry::Instance().Create("RequestLocalNode");
  ASSERT_NE(node, nullptr);
  SessionContext session;
  ASSERT_TRUE(InitNodeForTest(*node, nlohmann::json::object(), &session));
  const int before = local_logic_constructions;
  for (const std::string text : {"first", "second", "third"}) {
    AlgContext context;
    context.Publish("input", TextBatch{{7, 2, text}});
    ASSERT_EQ(node->Process(&context), 0);
    const auto* output = context.Read<TextBatch>("output");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 1u);
    EXPECT_EQ((*output)[0].data, text);
  }
  EXPECT_EQ(local_logic_constructions, before + 3);
}

}  // namespace llm_edgeflow
