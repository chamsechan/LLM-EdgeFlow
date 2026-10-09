#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "contracts/parameters.h"
#include "core/alg_context.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "engine/model_interface.h"
#include "nodes/authoring.h"
#include "nodes/node_error_codes.h"
#include "tests/support/node_harness.h"
#include "tests/support/node_process_pause.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/registry_test_access.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {
namespace {

struct TextInputs {
  const TextBatch* input = nullptr;
};

// 逐条文本转换夹具：与作者用 MapPayloads 编写的 Run 相同。
template <typename Fn>
auto TextMapSpec(Fn fn) {
  return MakeNodeSpec(
      InputsOf<TextInputs>({Required("input", &TextInputs::input)}),
      PreservedOutput<TextBatch>("output", "input"),
      [fn = std::move(fn)](const TextInputs& inputs) -> NodeResult<TextBatch> {
        return MapPayloads(*inputs.input, fn);
      });
}

template <typename ParamsT, typename Fn>
auto TextMapSpec(Parameters<ParamsT> params, Fn fn) {
  return MakeNodeSpec(
      InputsOf<TextInputs>({Required("input", &TextInputs::input)}),
      PreservedOutput<TextBatch>("output", "input"), std::move(params),
      [fn = std::move(fn)](const TextInputs& inputs,
                           const ParamsT& params) -> NodeResult<TextBatch> {
        return MapPayloads(*inputs.input, [&](const std::string& text) {
          return fn(text, params);
        });
      });
}

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
  return TextMapSpec(CleanConfig(), &CleanTextFn)
      .Description("Clean text map node");
}

REGISTER_FUNCTION_NODE(clean_text_map, CleanSpec());

// 遇到特定关键字时返回失败 NodeResult 的 Node
NodeResult<std::string> FailableCleanFn(const std::string& in) {
  if (in == "FAIL") {
    return NodeResult<std::string>::Failure(
        NodeErrorKind::kBusinessError, "Forced failure on keyword FAIL", -9999);
  }
  return NodeResult<std::string>::Success("processed:" + in);
}

auto FailableSpec() {
  return TextMapSpec(&FailableCleanFn).Description("Failable map node");
}

REGISTER_FUNCTION_NODE(failable_map, FailableSpec());

// 失败时不带自身消息，由框架给出 Map 函数名。
auto SilentFailureSpec() {
  return TextMapSpec([](const std::string& in) {
    if (in == "FAIL") {
      return NodeResult<std::string>::Failure(NodeErrorKind::kBusinessError,
                                              "");
    }
    return NodeResult<std::string>::Success(in);
  });
}

REGISTER_FUNCTION_NODE(silent_failure_map, SilentFailureSpec());

// 无参数、返回普通 string 的 Node
std::string UpperFn(const std::string& in) {
  std::string out = in;
  for (auto& c : out) c = static_cast<char>(toupper(c));
  return out;
}

auto UpperSpec() {
  return TextMapSpec(&UpperFn).Description("Uppercase map node");
}

REGISTER_FUNCTION_NODE(upper_map, UpperSpec());

int move_only_map_calls = 0;
int move_only_map_instances = 0;

// 可调用对象持有其不可变资源；参数仍可拷贝。
auto MoveOnlyMapSpec() {
  auto transform =
      [owned = std::make_unique<std::string>(
           "owned:" + std::to_string(++move_only_map_instances) + ":")](
          const std::string& input, const CleanParams& params) {
        ++move_only_map_calls;
        return params.prefix + *owned + input;
      };
  static_assert(!std::is_copy_constructible_v<decltype(transform)>);
  return TextMapSpec(CleanConfig(), std::move(transform))
      .WithControls({ReplaceFields(3005, "set_prefix", {"prefix"})});
}
REGISTER_FUNCTION_NODE(move_only_map, MoveOnlyMapSpec());

auto MoveOnlyResultMapSpec() {
  auto transform = [owned = std::make_unique<std::string>("result:")](
                       const std::string& input) -> NodeResult<std::string> {
    if (input == "FAIL") {
      return NodeResult<std::string>::Failure(
          NodeErrorKind::kBusinessError, "move-only callback failure", -9998);
    }
    return NodeResult<std::string>::Success(*owned + input);
  };
  static_assert(!std::is_copy_constructible_v<decltype(transform)>);
  return TextMapSpec(std::move(transform));
}
REGISTER_FUNCTION_NODE(move_only_result_map, MoveOnlyResultMapSpec());

// ---------------------------------------------------------------------------
// Batch 夹具
// ---------------------------------------------------------------------------

class CountingMockLlmModel final : public ILlmModel {
 public:
  const std::string& ImplName() const noexcept override {
    static const std::string t = "counting_mock_llm";
    return t;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string cap = "llm";
    return cap;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }

  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs,
               std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
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
  return MakeNodeSpec(InputsOf<AnswerInputs>({
                          Required("questions", &AnswerInputs::questions),
                          Optional("context", &AnswerInputs::context,
                                   InputFlow::AggregateByRequest),
                      }),
                      PreservedOutput<TextBatch>("output", "questions"),
                      AnswerConfig(),
                      ModelsOf<AnswerModels>({
                          Model("generator", "bind_model", &AnswerModels::llm),
                      }),
                      &AnswerBatchFn)
      .Description("Batch answering node with retry");
}

REGISTER_FUNCTION_NODE(answer_batch, AnswerBatchSpec());

struct OptionalValueInputs {
  const TextBatch* input = nullptr;
  const TextBatch* context = nullptr;
};

std::atomic<int> optional_value_run_count{0};

auto OptionalValueSpec() {
  return MakeNodeSpec(
      InputsOf<OptionalValueInputs>({
          Required("input", &OptionalValueInputs::input),
          OptionalValue("context", &OptionalValueInputs::context),
      }),
      PreservedOutput<TextBatch>("output", "input"),
      [](const OptionalValueInputs& inputs) -> NodeResult<TextBatch> {
        ++optional_value_run_count;
        std::string prefix = "[no-context] ";
        if (inputs.context) {
          prefix = inputs.context->empty()
                       ? "[empty-context] "
                       : "[CTX:" + inputs.context->front().data + "] ";
        }
        return MapPayloads(*inputs.input, [&](const std::string& text) {
          return prefix + text;
        });
      });
}
REGISTER_FUNCTION_NODE(optional_value_batch, OptionalValueSpec());

struct TwoStageModels {
  LlmCall draft;
  LlmCall revise;
};

auto TwoStageSpec() {
  return MakeNodeSpec(
      InputsOf<AnswerInputs>({Required("questions", &AnswerInputs::questions)}),
      PreservedOutput<TextBatch>("output", "questions"),
      ModelsOf<TwoStageModels>({
          Model("draft", "draft_model", &TwoStageModels::draft),
          Model("revise", "revise_model", &TwoStageModels::revise),
      }),
      [](const AnswerInputs& inputs,
         const TwoStageModels& models) -> NodeResult<TextBatch> {
        auto draft = models.draft.Generate(*inputs.questions);
        if (!draft.ok()) return draft;
        return models.revise.Generate(draft.value());
      });
}

class TwoStageHarness : public AuthorNode<decltype(TwoStageSpec())> {
 public:
  static constexpr const char* kNodeType = "two_stage_harness";
  TwoStageHarness() : AuthorNode(kNodeType, TwoStageSpec()) {}
};

NodeDefinition TwoStageDefinition() {
  return TwoStageSpec().BuildDefinition(TwoStageHarness::kNodeType);
}

REGISTER_NODE_WITH_DEFINITION(TwoStageHarness, TwoStageDefinition());

struct TextToScoreInputs {
  const TextBatch* texts = nullptr;
};

auto TextToScoreSpec() {
  return MakeNodeSpec(
      InputsOf<TextToScoreInputs>({
          Required("texts", &TextToScoreInputs::texts),
      }),
      PreservedOutput<ScoreBatch>("scores", "texts"),
      [](const TextToScoreInputs& in) -> NodeResult<ScoreBatch> {
        ScoreBatch scores;
        if (!in.texts) return NodeResult<ScoreBatch>::Success(scores);
        scores.reserve(in.texts->size());
        for (const auto& item : *in.texts) {
          scores.emplace_back(item.req_id, item.sub_id, 1.0f);
        }
        return NodeResult<ScoreBatch>::Success(std::move(scores));
      });
}
REGISTER_FUNCTION_NODE(text_to_score, TextToScoreSpec());

struct ContextPollutionInputs {
  const TextBatch* input = nullptr;
};

auto FailingAfterPublishSpec() {
  return MakeNodeSpec(
      InputsOf<ContextPollutionInputs>({
          Required("input", &ContextPollutionInputs::input),
      }),
      PreservedOutput<TextBatch>("output", "input"),
      [](const ContextPollutionInputs&) -> NodeResult<TextBatch> {
        return NodeResult<TextBatch>::Failure(NodeErrorKind::kBusinessError,
                                              "forced error");
      });
}
REGISTER_FUNCTION_NODE(failing_after_publish, FailingAfterPublishSpec());

struct BindingTestParams {
  bool check_mask = false;
};

struct BindingTestInputs {
  const TextBatch* texts = nullptr;
  const TextBatch* mask = nullptr;
};

inline auto BindingTestSpec() {
  return MakeNodeSpec(
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
      [](const BindingTestInputs& in,
         const BindingTestParams&) -> NodeResult<TextBatch> {
        return NodeResult<TextBatch>::Success(*in.texts);
      });
}
REGISTER_FUNCTION_NODE(binding_test, BindingTestSpec());

struct BindingMapParams {
  bool require_extra = false;
};

inline auto BindingMapSpec() {
  return TextMapSpec(
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
REGISTER_FUNCTION_NODE(binding_map, BindingMapSpec());

struct ControlledMapParams {
  std::string prefix;
  std::string suffix;
  int multiplier = 1;
};

inline constexpr int kCmdReplaceMap = 3001;
inline constexpr int kCmdSuffixMap = 3002;

inline std::string ControlledMapFn(const std::string& in,
                                   const ControlledMapParams& p) {
  std::string res = p.prefix;
  for (int i = 0; i < p.multiplier; ++i) {
    res += in;
  }
  return res + p.suffix;
}

inline auto ControlledMapSpec() {
  return TextMapSpec(
             Parameters<ControlledMapParams>(
                 {
                     Field("prefix", &ControlledMapParams::prefix).Default(""),
                     Field("suffix", &ControlledMapParams::suffix).Default(""),
                     Field("multiplier", &ControlledMapParams::multiplier)
                         .Default(1)
                         .Minimum(1)
                         .Maximum(10),
                 })
                 .Validate([](const ControlledMapParams& p, std::string* err) {
                   if (p.prefix == "INVALID") {
                     if (err) *err = "Invalid prefix disallowed";
                     return false;
                   }
                   return true;
                 }),
             &ControlledMapFn)
      .WithControls({
          ReplaceFields(kCmdReplaceMap, "replace_map",
                        {"prefix", "multiplier"}),
          ReplaceFields(kCmdSuffixMap, "replace_suffix", {"suffix"}),
      });
}
REGISTER_FUNCTION_NODE(controlled_map, ControlledMapSpec());

struct ControlledElement {
  std::string text;
  int repeats = 0;
  std::string prepared;
};

auto ControlledElementParameters() {
  return Parameters<ControlledElement>{
      Field("text", &ControlledElement::text).Required(),
      Field("repeats", &ControlledElement::repeats).Default(1).Range(1, 3)}
      .Prepare([](ControlledElement* value, std::string* error) {
        if (value->text == "FAIL_PREPARE") {
          if (error) *error = "element prepare rejected";
          return false;
        }
        value->prepared = "prepared:" + value->text;
        return true;
      })
      .Validate([](const ControlledElement& value, std::string* error) {
        if (value.text == "FAIL_VALIDATE") {
          if (error) *error = "element validation rejected";
          return false;
        }
        return true;
      });
}

struct ControlledContainersParams {
  std::map<std::string, std::vector<std::string>> groups;
  std::vector<ControlledElement> rules;
  nlohmann::json fallback;
  std::string label;
  std::string prepared;
};

constexpr int kCmdContainers = 3010;

auto ControlledContainersSpec() {
  auto params =
      Parameters<ControlledContainersParams>{
          Field("groups", &ControlledContainersParams::groups)
              .Default({{"old", {"fast"}}})
              .Enum({"fast", "slow"}),
          Field("rules", &ControlledContainersParams::rules)
              .Default({})
              .Items(ControlledElementParameters()),
          Field("fallback", &ControlledContainersParams::fallback)
              .Default(nlohmann::json::object()),
          Field("label", &ControlledContainersParams::label).Default("keep")}
          .Prepare([](ControlledContainersParams* value, std::string* error) {
            if (value->fallback.is_object() &&
                value->fallback.value("reject_prepare", false)) {
              if (error) *error = "container prepare rejected";
              return false;
            }
            value->prepared = value->label + ":" +
                              std::to_string(value->groups.size()) + ":" +
                              std::to_string(value->rules.size());
            return true;
          })
          .Validate(
              [](const ControlledContainersParams& value, std::string* error) {
                if (value.groups.count("blocked")) {
                  if (error) *error = "blocked group rejected";
                  return false;
                }
                return true;
              });
  return TextMapSpec(
             std::move(params),
             [](const std::string&, const ControlledContainersParams& value) {
               auto rules = nlohmann::json::array();
               for (const auto& rule : value.rules) {
                 rules.push_back({{"text", rule.text},
                                  {"repeats", rule.repeats},
                                  {"prepared", rule.prepared}});
               }
               return nlohmann::json{{"groups", value.groups},
                                     {"rules", rules},
                                     {"fallback", value.fallback},
                                     {"label", value.label},
                                     {"prepared", value.prepared}}
                   .dump();
             })
      .WithControls({ReplaceFields(kCmdContainers, "replace_containers",
                                   {"groups", "rules", "fallback", "label"})});
}
REGISTER_FUNCTION_NODE(controlled_containers, ControlledContainersSpec());

// ---------------------------------------------------------------------------
// 不可拷贝的参数 (持有 unique_ptr)，不带 Control
// ---------------------------------------------------------------------------
struct NonCopyableMapParams {
  std::string prefix;
  std::unique_ptr<int> extra_counter;
};

inline auto NonCopyableMapSpec() {
  return TextMapSpec(
      Parameters<NonCopyableMapParams>(
          {
              Field("prefix", &NonCopyableMapParams::prefix).Default("nc:"),
          })
          .Prepare(
              [](NonCopyableMapParams* p, const BindingFacts&, std::string*) {
                p->extra_counter = std::make_unique<int>(100);
                return true;
              }),
      [](const std::string& in, const NonCopyableMapParams& p) {
        return p.prefix + in + "_" +
               (p.extra_counter ? std::to_string(*p.extra_counter) : "null");
      });
}
REGISTER_FUNCTION_NODE(non_copyable_map, NonCopyableMapSpec());

struct NonCopyableBatchInputs {
  const TextBatch* texts = nullptr;
};

struct NonCopyableBatchParams {
  std::string tag;
  std::unique_ptr<int> extra_val;
};

inline auto NonCopyableBatchSpec() {
  return MakeNodeSpec(
      InputsOf<NonCopyableBatchInputs>({
          Required("texts", &NonCopyableBatchInputs::texts),
      }),
      PreservedOutput<TextBatch>("output", "texts"),
      Parameters<NonCopyableBatchParams>(
          {
              Field("tag", &NonCopyableBatchParams::tag).Default("batch_nc:"),
          })
          .Prepare(
              [](NonCopyableBatchParams* p, const BindingFacts&, std::string*) {
                p->extra_val = std::make_unique<int>(200);
                return true;
              }),
      [](const NonCopyableBatchInputs& in,
         const NonCopyableBatchParams& p) -> NodeResult<TextBatch> {
        TextBatch out;
        if (!in.texts) return out;
        for (const auto& item : *in.texts) {
          out.emplace_back(
              item.req_id, item.sub_id,
              p.tag + item.data + "_" +
                  (p.extra_val ? std::to_string(*p.extra_val) : "null"));
        }
        return out;
      });
}
REGISTER_FUNCTION_NODE(non_copyable_batch, NonCopyableBatchSpec());

// ---------------------------------------------------------------------------
// 严格的无计划事实检查 Node
// ---------------------------------------------------------------------------

struct ControlledBatchInputs {
  const TextBatch* texts = nullptr;
};

struct ControlledBatchParams {
  std::string header;
  bool uppercase = false;
};

inline constexpr int kCmdReplaceBatch = 3003;

inline NodeResult<TextBatch> ControlledBatchFn(
    const ControlledBatchInputs& inputs, const ControlledBatchParams& params) {
  TextBatch out;
  if (!inputs.texts) return out;
  out.reserve(inputs.texts->size());
  for (const auto& item : *inputs.texts) {
    std::string text = params.header + item.data;
    if (params.uppercase) {
      for (char& c : text)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    out.emplace_back(item.req_id, item.sub_id, std::move(text));
  }
  return out;
}

inline auto ControlledBatchSpec() {
  return MakeNodeSpec(
             InputsOf<ControlledBatchInputs>({
                 Required("texts", &ControlledBatchInputs::texts),
             }),
             PreservedOutput<TextBatch>("output", "texts"),
             Parameters<ControlledBatchParams>(
                 {
                     Field("header", &ControlledBatchParams::header)
                         .Default(""),
                     Field("uppercase", &ControlledBatchParams::uppercase)
                         .Default(false),
                 })
                 .Validate(
                     [](const ControlledBatchParams& p, std::string* err) {
                       if (p.header == "REJECT") {
                         if (err) *err = "Rejected header";
                         return false;
                       }
                       return true;
                     }),
             &ControlledBatchFn)
      .WithControls({
          ReplaceFields(kCmdReplaceBatch, "set_batch_params",
                        {"header", "uppercase"}),
      });
}
REGISTER_FUNCTION_NODE(controlled_batch, ControlledBatchSpec());

struct ImageSummaryInputs {
  const ImageRefBatch* images = nullptr;
};
struct ImageSummaryOutputs {
  TextBatch names;
  Int32Batch lengths;
};
struct ImageSummaryParams {
  std::string fault;
};
auto ImageSummarySpec() {
  return MakeNodeSpec(
      InputsOf<ImageSummaryInputs>{
          Required("images", &ImageSummaryInputs::images)},
      OutputsOf<ImageSummaryOutputs>{
          Produced("names", &ImageSummaryOutputs::names, "images"),
          Produced("lengths", &ImageSummaryOutputs::lengths, "images")},
      Parameters<ImageSummaryParams>{
          Field("fault", &ImageSummaryParams::fault).Default("")},
      [](const ImageSummaryInputs& inputs,
         const ImageSummaryParams& params) -> NodeResult<ImageSummaryOutputs> {
        ImageSummaryOutputs output;
        for (const auto& image : *inputs.images) {
          output.names.emplace_back(image.req_id, image.sub_id, image.data);
          output.lengths.emplace_back(image.req_id, image.sub_id,
                                      static_cast<int32_t>(image.data.size()));
        }
        if (params.fault == "business") {
          return NodeResult<ImageSummaryOutputs>::Failure(
              NodeErrorKind::kBusinessError, "Cannot summarize image", -9876);
        }
        if (params.fault == "count") output.lengths.pop_back();
        if (params.fault == "provenance") ++output.lengths.back().sub_id;
        return output;
      });
}
REGISTER_FUNCTION_NODE(image_summary_author, ImageSummarySpec());

struct SourceInputs {};
auto SourceSpec() {
  return MakeNodeSpec(
      InputsOf<SourceInputs>{},
      ProducedBatch<TextBatch>("chunks", {"1:N", "generate_sub_id", "session"}),
      [](const SourceInputs&) -> NodeResult<TextBatch> {
        return TextBatch{{41, 0, "first"}, {41, 1, "second"}, {41, 2, "third"}};
      });
}
REGISTER_FUNCTION_NODE(generated_source_author, SourceSpec());

}  // namespace

// ===========================================================================
// 测试
// ===========================================================================

// A1: Map 多请求/非零 sub_id 保序、输入未修改、空批次不调用；中间项失败无输出
TEST(FunctionNodeTest, MapPreservesOrderingAndProvenanceAcrossRequests) {
  NodeHarness harness("clean_text_map");
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

// 所有能力调用共享同一份仅可移动的所有权契约。
template <typename CallT, typename ModelT>
void ExpectModelCallContract(const char* default_slot) {
  static_assert(std::is_same_v<typename CallT::Interface, ModelT>);
  static_assert(!std::is_copy_constructible_v<CallT>);
  static_assert(!std::is_copy_assignable_v<CallT>);
  static_assert(std::is_nothrow_move_constructible_v<CallT>);
  static_assert(std::is_nothrow_move_assignable_v<CallT>);
  CallT unbound;
  EXPECT_FALSE(unbound.IsBound());
  CallT named(nullptr);
  EXPECT_EQ(named.SlotName(), default_slot);
  EXPECT_TRUE(named.ModelName().empty());
}

TEST(FunctionNodeTest, MoveOnlyMapCallbackOwnsResourceAndPreservesProvenance) {
  const TextBatch input = {
      {1001, 3, "first"}, {1001, 8, "second"}, {2002, 5, "third"}};
  NodeHarness first("move_only_map");
  first.TextInputWithBatch("input", input);
  const auto result = first.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* output = result.Output<TextBatch>("output");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), input.size());
  const auto prefix = (*output)[0].data.substr(
      0, (*output)[0].data.size() - input[0].data.size());
  EXPECT_EQ(prefix.find("owned:"), 0u);
  for (size_t i = 0; i < input.size(); ++i) {
    EXPECT_EQ((*output)[i].req_id, input[i].req_id);
    EXPECT_EQ((*output)[i].sub_id, input[i].sub_id);
    EXPECT_EQ((*output)[i].data, prefix + input[i].data);
  }

  NodeHarness second("move_only_map");
  second.TextInputWithBatch("input", input);
  const auto independent = second.Run();
  ASSERT_TRUE(independent.ok()) << independent.diagnostic();
  EXPECT_NE(independent.TextValues("output"), result.TextValues("output"));

  const auto repeated = first.Run();
  ASSERT_TRUE(repeated.ok()) << repeated.diagnostic();
  EXPECT_EQ(repeated.TextValues("output"), result.TextValues("output"));
  const auto* original = result.Context()->Read<TextBatch>("bk_in_input");
  ASSERT_NE(original, nullptr);
  ASSERT_EQ(original->size(), input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    EXPECT_EQ((*original)[i].data, input[i].data);
  }
}

TEST(FunctionNodeTest, MoveOnlyMapCallbackSkipsEmptyBatchAndSurvivesControl) {
  NodeHarness harness("move_only_map");
  harness.TextInput("input", {"hello"});
  const auto initial = harness.Run();
  ASSERT_TRUE(initial.ok()) << initial.diagnostic();
  const auto initial_values = initial.TextValues("output");
  const int calls_before_empty = move_only_map_calls;
  harness.TextInput("input", {});
  const auto empty = harness.Run();
  ASSERT_TRUE(empty.ok()) << empty.diagnostic();
  ASSERT_NE(empty.Output<TextBatch>("output"), nullptr);
  EXPECT_TRUE(empty.TextValues("output").empty());
  EXPECT_EQ(move_only_map_calls, calls_before_empty);

  const auto control = harness.Control(3005, R"({"prefix":"patched:"})");
  ASSERT_EQ(control.status, NodeControlStatus::kHandled) << control.message;
  harness.TextInput("input", {"hello"});
  const auto patched = harness.Run();
  ASSERT_TRUE(patched.ok()) << patched.diagnostic();
  EXPECT_EQ(patched.TextValues("output"),
            (std::vector<std::string>{"patched:" + initial_values[0]}));
}

TEST(FunctionNodeTest, MoveOnlyMapCallbackSupportsNodeResultAndFailure) {
  NodeHarness harness("move_only_result_map");
  const TextBatch input = {{111, 4, "a"}, {222, 9, "b"}};
  harness.TextInputWithBatch("input", input);
  const auto success = harness.Run();
  ASSERT_TRUE(success.ok()) << success.diagnostic();
  const auto* output = success.Output<TextBatch>("output");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    EXPECT_EQ((*output)[i].req_id, input[i].req_id);
    EXPECT_EQ((*output)[i].sub_id, input[i].sub_id);
    EXPECT_EQ((*output)[i].data, "result:" + input[i].data);
  }

  harness.TextInput("input", {"a", "FAIL", "b"});
  const auto failure = harness.Run();
  EXPECT_FALSE(failure.ok());
  EXPECT_EQ(failure.process_code(), -9998);
  EXPECT_EQ(failure.Output<TextBatch>("output"), nullptr);
}

TEST(FunctionNodeTest, ModelCallsShareOwnershipContract) {
  ExpectModelCallContract<LlmCall, ILlmModel>("generator");
  ExpectModelCallContract<EmbeddingCall, IEmbeddingModel>("encoder");
  ExpectModelCallContract<AsrCall, IAsrModel>("transcriber");
  ExpectModelCallContract<OcrCall, IOcrModel>("detector");
  ExpectModelCallContract<RerankCall, IRerankModel>("reranker");
}

TEST(FunctionNodeTest, EmptyBatchReturnsEmptyWithoutCallingFunction) {
  NodeHarness harness("clean_text_map");
  harness.TextInput("input", {});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_TRUE(result.TextValues("output").empty());
}

TEST(FunctionNodeTest, IntermediateFailureProducesNoOutput) {
  NodeHarness harness("failable_map");
  harness.TextInput("input", {"ok1", "FAIL", "ok3"});

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.process_code(), -9999);

  const auto* out = result.Output<TextBatch>("output");
  EXPECT_EQ(out, nullptr);
}

TEST(FunctionNodeTest, MapPayloadsFailureNamesItem) {
  NodeHarness harness("failable_map");
  harness.TextInput("input", {"ok1", "FAIL", "ok3"});
  auto result = harness.Run();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.process_code(), -9999);
  EXPECT_NE(result.diagnostic().find("Forced failure on keyword FAIL"),
            std::string::npos)
      << result.diagnostic();
  EXPECT_NE(result.diagnostic().find("MapPayloads"), std::string::npos)
      << result.diagnostic();
  EXPECT_NE(result.diagnostic().find("sub_id=0"), std::string::npos)
      << result.diagnostic();

  NodeHarness silent("silent_failure_map");
  silent.TextInput("input", {"ok", "FAIL"});
  auto silent_result = silent.Run();
  ASSERT_FALSE(silent_result.ok());
  EXPECT_EQ(silent_result.process_code(),
            node_error::author_node::kBusinessError);
  EXPECT_NE(
      silent_result.diagnostic().find("silent_failure_map process failed"),
      std::string::npos)
      << silent_result.diagnostic();
  EXPECT_NE(silent_result.diagnostic().find("MapPayloads"), std::string::npos)
      << silent_result.diagnostic();
  EXPECT_EQ(silent_result.Output<TextBatch>("output"), nullptr);

  NodeHarness empty("silent_failure_map");
  empty.TextInput("input", {});
  auto empty_result = empty.Run();
  ASSERT_TRUE(empty_result.ok()) << empty_result.diagnostic();
  ASSERT_NE(empty_result.Output<TextBatch>("output"), nullptr);
  EXPECT_TRUE(empty_result.TextValues("output").empty());
}

// A5: 重复 output key 保留旧值且失败；最终业务失败不发布新输出
TEST(FunctionNodeTest, DuplicateOutputKeyFailsAndKeepsExistingValue) {
  auto node = NodeRegistry::Instance().Create("upper_map");
  ASSERT_NE(node, nullptr);

  SessionContext session_ctx;
  ASSERT_TRUE(InitNodeForTest(*node, nlohmann::json::object(), &session_ctx));

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
  test_support::RegistryTestAccess::ClearNodeFailures();
}

TEST(FunctionNodeTest, NodeWithoutParametersOperatesCorrectly) {
  NodeHarness harness("upper_map");
  harness.TextInput("input", {"hello", "world"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"HELLO", "WORLD"}));
}

TEST(FunctionNodeTest, HarnessAllowsModelSlotsToShareOneModel) {
  auto model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("two_stage_harness");
  harness.Config({{"draft_model", "shared"}, {"revise_model", "shared"}});
  harness.BindModel("shared", model);
  harness.TextInput("questions", {"hello"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"ans:ans:hello"}));
  EXPECT_EQ(model->call_count, 2);
}

TEST(FunctionNodeTest, ModelReferencesRequireExplicitConfiguration) {
  auto draft = std::make_shared<CountingMockLlmModel>();
  auto revise = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("two_stage_harness");
  harness.BindModel("default_draft_model", draft);
  harness.BindModel("default_revise_model", revise);
  harness.TextInput("questions", {"hello"});

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(draft->call_count, 0);
  EXPECT_EQ(revise->call_count, 0);

  const auto definition = TwoStageDefinition();
  ASSERT_EQ(definition.config_fields.size(), 2U);
  for (const auto& field : definition.config_fields) {
    EXPECT_TRUE(field.required);
    EXPECT_TRUE(field.default_value.is_null());
    EXPECT_NE(field.semantic.find("models[].name"), std::string::npos);
    EXPECT_NE(field.semantic.find("llm"), std::string::npos);
  }

  harness.Config({{"draft_model", "default_draft_model"},
                  {"revise_model", "default_revise_model"}});
  result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"ans:ans:hello"}));
  EXPECT_EQ(draft->call_count, 1);
  EXPECT_EQ(revise->call_count, 1);
}

// A2: Batch 空输入调用 Run；optional 未连/已连空/已连缺值区分；anchor 错误失败
TEST(FunctionNodeTest, BatchEmptyInputCallsRunAndSucceeds) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("answer_batch");
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
  NodeHarness harness("answer_batch");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_model);
  harness.OmitPortFromPlan("context");
  harness.TextInput("questions", {"What is AI?"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  // 无上下文时，输出不含 [CTX:...]
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"final:ans:What is AI?"}));
  EXPECT_EQ(mock_model->call_count, 1);
}

TEST(FunctionNodeTest, BatchOptionalPortConnectedProvidesContext) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("answer_batch");
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
  NodeHarness harness("answer_batch");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("questions", {"What is AI?"});
  // 计划中有 context，但未提供给 AlgContext

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.diagnostic().find("context"), std::string::npos);
  EXPECT_EQ(mock_model->call_count, 0);
  EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
}

TEST(FunctionNodeTest, BatchOptionalPortWrongRuntimeTypeFailsBeforeModelCall) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("answer_batch");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("questions", {"What is AI?"});
  harness.CustomInput("context", std::string("not a TextBatch"));

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(result.init_failed());
  EXPECT_NE(result.diagnostic().find("context"), std::string::npos);
  EXPECT_NE(result.diagnostic().find("bk_in_context"), std::string::npos);
  EXPECT_NE(result.diagnostic().find("type mismatch"), std::string::npos);
  EXPECT_EQ(mock_model->call_count, 0);
  EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
}

TEST(FunctionNodeTest, BatchOptionalValueUnconnectedProvidesNullptr) {
  optional_value_run_count = 0;
  NodeHarness harness("optional_value_batch");
  harness.OmitPortFromPlan("context");
  harness.TextInput("input", {"hello"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"[no-context] hello"}));
  EXPECT_EQ(optional_value_run_count.load(), 1);
}

TEST(FunctionNodeTest, BatchOptionalValueConnectedButMissingProvidesNullptr) {
  optional_value_run_count = 0;
  NodeHarness harness("optional_value_batch");
  harness.TextInput("input", {"hello"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"[no-context] hello"}));
  EXPECT_EQ(optional_value_run_count.load(), 1);
}

TEST(FunctionNodeTest, BatchOptionalValueConnectedEmptyProvidesBatch) {
  optional_value_run_count = 0;
  NodeHarness harness("optional_value_batch");
  harness.TextInput("input", {"hello"});
  harness.TextInput("context", {});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"[empty-context] hello"}));
  EXPECT_EQ(optional_value_run_count.load(), 1);
}

TEST(FunctionNodeTest, BatchOptionalValueUsesContextAndPreservesProvenance) {
  optional_value_run_count = 0;
  NodeHarness harness("optional_value_batch");
  harness.TextInputWithBatch("input", {{71, 5, "hello"}, {72, 9, "world"}});
  harness.TextInputWithBatch("context", {{71, 4, "knowledge"}});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* output = result.Output<TextBatch>("output");
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), 2U);
  EXPECT_EQ((*output)[0].req_id, 71U);
  EXPECT_EQ((*output)[0].sub_id, 5U);
  EXPECT_EQ((*output)[0].data, "[CTX:knowledge] hello");
  EXPECT_EQ((*output)[1].req_id, 72U);
  EXPECT_EQ((*output)[1].sub_id, 9U);
  EXPECT_EQ((*output)[1].data, "[CTX:knowledge] world");
  EXPECT_EQ(optional_value_run_count.load(), 1);
}

TEST(FunctionNodeTest, BatchOptionalValueWrongRuntimeTypeFailsBeforeRun) {
  optional_value_run_count = 0;
  NodeHarness harness("optional_value_batch");
  harness.TextInput("input", {"hello"});
  harness.CustomInput("context", std::string("not a TextBatch"));

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(result.init_failed());
  EXPECT_NE(result.diagnostic().find("context"), std::string::npos);
  EXPECT_NE(result.diagnostic().find("bk_in_context"), std::string::npos);
  EXPECT_NE(result.diagnostic().find("type mismatch"), std::string::npos);
  EXPECT_EQ(optional_value_run_count.load(), 0);
  EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
}

TEST(FunctionNodeTest, MissingValidatedPlanFailsInitialization) {
  auto node = NodeRegistry::Instance().Create("answer_batch");
  ASSERT_NE(node, nullptr);
  SessionContext session;
  std::string diagnostic;
  NodeInitContext init;
  init.session_ctx = &session;
  init.diagnostic = &diagnostic;
  EXPECT_FALSE(node->Init(init));
  EXPECT_NE(diagnostic.find("ValidatedNodePlan"), std::string::npos);
}

// M1: 一次非空批次一次 Model 调用；空输入零调用；options 完整传递
TEST(FunctionNodeTest, BatchSingleModelCallAndOptionsPassing) {
  auto mock_model = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("answer_batch");
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
  // 情形 1：模型返回推理失败
  {
    auto mock_model = std::make_shared<CountingMockLlmModel>();
    mock_model->always_fail = true;
    NodeHarness harness("answer_batch");
    harness.Config({{"bind_model", "test_llm"}, {"max_revisions", 0}});
    harness.BindModel("test_llm", mock_model);
    harness.TextInput("questions", {"Q1"});
    harness.OmitPortFromPlan("context");

    auto result = harness.Run();
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
  }

  // 情形 2：模型返回的条目数不对 (偏少)
  {
    auto mock_model = std::make_shared<CountingMockLlmModel>();
    mock_model->return_wrong_count = true;
    NodeHarness harness("answer_batch");
    harness.Config({{"bind_model", "test_llm"}, {"max_revisions", 0}});
    harness.BindModel("test_llm", mock_model);
    harness.TextInput("questions", {"Q1", "Q2"});
    harness.OmitPortFromPlan("context");

    auto result = harness.Run();
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
  }

  // 情形 3：模型返回损坏的来源信息
  {
    auto mock_model = std::make_shared<CountingMockLlmModel>();
    mock_model->corrupt_provenance = true;
    NodeHarness harness("answer_batch");
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
  mock_model->fail_first_n = 1;  // 第 1 次失败，第 2 次成功

  NodeHarness harness("answer_batch");
  harness.Config({{"bind_model", "test_llm"}, {"max_revisions", 2}});
  harness.BindModel("test_llm", mock_model);
  harness.TextInput("questions", {"Q1"});
  harness.OmitPortFromPlan("context");

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(mock_model->call_count, 2);
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"final:ans:Q1"}));

  // AlgContext 必须完全干净
  EXPECT_TRUE(result.Context()->IsOk());
}

TEST(FunctionNodeTest, BatchCrossTypeOutputAlignmentSucceeds) {
  NodeHarness harness("text_to_score");
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
  NodeHarness harness("answer_batch");
  harness.Config({{"bind_model", "test_llm"}, {"unknown_field", 123}});
  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.init_failed());
  EXPECT_NE(result.diagnostic().find("unknown_field"), std::string::npos);
}

TEST(FunctionNodeTest, ProcessFailedPreservesAlgContextForOutputInspection) {
  NodeHarness harness("failing_after_publish");
  harness.TextInput("input", {"item1"});
  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(result.init_failed());
  EXPECT_EQ(result.process_code(), node_error::author_node::kBusinessError);
  ASSERT_NE(result.Context(), nullptr);
  EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
}

TEST(FunctionNodeTest, BindingValidationEnforcedInInitAndHarness) {
  // Map Node 测试
  const auto map_def = PipelineCatalog::FindNode("binding_map");
  ASSERT_TRUE(map_def.has_value());
  ASSERT_TRUE(map_def->validate_config);

  // 1. 缺少额外端口时，Map Definition 预检拒绝配置
  std::string map_diag;
  EXPECT_FALSE(map_def->validate_config({{"require_extra", true}},
                                        BindingFacts{true, {"input"}, {}},
                                        &map_diag, nullptr));
  EXPECT_NE(map_diag.find("require_extra requires extra port"),
            std::string::npos);

  // 2. 缺少额外端口时，无计划的 Map Init 失败
  auto map_unplanned = NodeRegistry::Instance().Create("binding_map");
  ASSERT_NE(map_unplanned, nullptr);
  nlohmann::json invalid_map_cfg = {{"require_extra", true}};
  SessionContext session_ctx;
  std::string map_init_diag;
  NodeInitContext init_ctx_map_unplanned;

  init_ctx_map_unplanned.session_ctx = &session_ctx;
  init_ctx_map_unplanned.diagnostic = &map_init_diag;
  init_ctx_map_unplanned.plan = nullptr;
  EXPECT_FALSE(map_unplanned->Init(init_ctx_map_unplanned));
  EXPECT_NE(map_init_diag.find("ValidatedNodePlan"), std::string::npos);

  // 3. 缺少额外端口时，手动计划的 Map Init 失败
  auto map_manual = NodeRegistry::Instance().Create("binding_map");
  ASSERT_NE(map_manual, nullptr);
  ValidatedNodePlan manual_map_plan;
  manual_map_plan.normalized_params = invalid_map_cfg;
  manual_map_plan.ports.push_back(
      {"input", "bk_input", BlackboardTypeTraits<TextBatch>::TypeName(), "1:1",
       "preserve", "request", PortDirection::kInput});
  manual_map_plan.ports.push_back(
      {"output", "bk_output", BlackboardTypeTraits<TextBatch>::TypeName(),
       "1:1", "preserve", "request", PortDirection::kOutput});
  std::string manual_map_diag;
  NodeInitContext init_ctx_map_manual;

  init_ctx_map_manual.session_ctx = &session_ctx;
  init_ctx_map_manual.diagnostic = &manual_map_diag;
  init_ctx_map_manual.plan = &manual_map_plan;
  EXPECT_FALSE(map_manual->Init(init_ctx_map_manual));
  EXPECT_NE(manual_map_diag.find("require_extra requires extra port"),
            std::string::npos);

  // 4. Batch Node 测试
  const auto def = PipelineCatalog::FindNode("binding_test");
  ASSERT_TRUE(def.has_value());
  ASSERT_TRUE(def->validate_config);

  // 缺少 mask 时，Batch Definition 预检拒绝配置
  std::string diag;
  EXPECT_FALSE(def->validate_config({{"check_mask", true}},
                                    BindingFacts{true, {"texts"}, {}}, &diag,
                                    nullptr));
  EXPECT_NE(diag.find("check_mask requires mask port"), std::string::npos);

  // mask 未连接时，手动计划的 Batch Init 失败
  auto node_manual = NodeRegistry::Instance().Create("binding_test");
  ASSERT_NE(node_manual, nullptr);
  ValidatedNodePlan manual_plan;
  nlohmann::json invalid_cfg = {{"check_mask", true}};
  manual_plan.normalized_params = invalid_cfg;
  manual_plan.ports.push_back(
      {"texts", "bk_texts", BlackboardTypeTraits<TextBatch>::TypeName(), "1:1",
       "preserve", "request", PortDirection::kInput});
  manual_plan.ports.push_back(
      {"output", "bk_output", BlackboardTypeTraits<TextBatch>::TypeName(),
       "1:1", "preserve", "request", PortDirection::kOutput});
  std::string manual_diag;
  NodeInitContext init_ctx_manual;

  init_ctx_manual.session_ctx = &session_ctx;
  init_ctx_manual.diagnostic = &manual_diag;
  init_ctx_manual.plan = &manual_plan;
  EXPECT_FALSE(node_manual->Init(init_ctx_manual));
  EXPECT_NE(manual_diag.find("check_mask requires mask port"),
            std::string::npos);

  // 5. 省略可选端口时 NodeHarness 的 Init 失败
  NodeHarness harness_fail("binding_test");
  harness_fail.Config({{"check_mask", true}});
  harness_fail.OmitPortFromPlan("mask");
  harness_fail.TextInput("texts", {"hello"});
  auto result_fail = harness_fail.Run();
  EXPECT_FALSE(result_fail.ok());
  EXPECT_TRUE(result_fail.init_failed());
  EXPECT_NE(result_fail.diagnostic().find("check_mask requires mask port"),
            std::string::npos);

  // 6. 连接 mask 后 NodeHarness 成功
  NodeHarness harness_ok("binding_test");
  harness_ok.Config({{"check_mask", true}});
  harness_ok.TextInput("texts", {"hello"});
  harness_ok.TextInput("mask", {"m1"});
  auto result_ok = harness_ok.Run();
  EXPECT_TRUE(result_ok.ok()) << result_ok.diagnostic();
  EXPECT_EQ(result_ok.TextValues("output"),
            (std::vector<std::string>{"hello"}));
}

TEST(FunctionNodeTest, PlannedPortBindingsPreserveAuthorDiagnosticsAndOrder) {
  struct Case {
    const char* node;
    const char* port;
    int fault;  // 0: 缺失，1: 空键且类型错误，2: 类型错误，3: 合法
    const char* expected;
  };
  const Case cases[] = {
      {"binding_map", "input", 3, ""},
      {"binding_test", "texts", 3, ""},
      {"binding_map", "input", 0,
       "Required input port 'input' has no binding in plan"},
      {"binding_map", "input", 1,
       "Required input port 'input' has no binding in plan"},
      {"binding_map", "input", 2,
       "Input port type mismatch for 'input' (expected: TextBatch, bound: "
       "integer)"},
      {"binding_map", "output", 0, ""},
      {"binding_map", "output", 1, ""},
      {"binding_map", "output", 2,
       "Output port type mismatch for 'output' (expected: TextBatch, bound: "
       "integer)"},
      {"binding_test", "texts", 0,
       "Required input port 'texts' has no binding in plan"},
      {"binding_test", "texts", 1,
       "Required input port 'texts' has no binding in plan"},
      {"binding_test", "texts", 2,
       "Input port type mismatch for 'texts' (expected: TextBatch, bound: "
       "integer)"},
      {"binding_test", "mask", 0, ""},
      {"binding_test", "mask", 1, ""},
      {"binding_test", "mask", 2,
       "Input port type mismatch for 'mask' (expected: TextBatch, bound: "
       "integer)"},
      {"binding_test", "output", 0, ""},
      {"binding_test", "output", 1, ""},
      {"binding_test", "output", 2,
       "Output port type mismatch for 'output' (expected: TextBatch, bound: "
       "integer)"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.node);
    SCOPED_TRACE(test.port);
    SCOPED_TRACE(test.fault);
    const bool map = std::string(test.node) == "binding_map";
    ValidatedNodePlan plan;
    plan.normalized_params = map ? nlohmann::json{{"require_extra", false}}
                                 : nlohmann::json{{"check_mask", false}};
    for (const auto& name :
         map ? std::vector<std::string>{"input", "output"}
             : std::vector<std::string>{"texts", "mask", "output"}) {
      if (name == test.port && test.fault == 0) continue;
      const bool broken = name == test.port && test.fault != 3;
      plan.ports.push_back(
          {name, broken && test.fault == 1 ? "" : "actual_" + name,
           broken ? "integer" : "TextBatch", "1:1", "preserve", "request",
           name == "output" ? PortDirection::kOutput : PortDirection::kInput});
    }
    // 即使后面的输出也非法，也必须优先报告输入错误。
    if (std::string(test.port) != "output" && *test.expected != '\0') {
      plan.ports.back().type_id = "integer";
    }
    auto node = NodeRegistry::Instance().Create(test.node);
    ASSERT_NE(node, nullptr);
    SessionContext session;
    std::string diagnostic;
    const bool succeeds = *test.expected == '\0';
    ASSERT_EQ(node->Init({&plan, &session, &diagnostic}), succeeds);
    EXPECT_EQ(diagnostic, test.expected);
    if (succeeds) {
      AlgContext context;
      context.Publish(map ? "actual_input" : "actual_texts",
                      TextBatch{{7, 2, "mapped"}});
      context.Publish(map ? "input" : "texts", TextBatch{{7, 2, "wrong"}});
      context.Publish("actual_mask", TextBatch{{7, 2, "mask"}});
      context.Publish("mask", TextBatch{{7, 2, "stale optional"}});
      ASSERT_EQ(node->Process(&context), 0);
      const auto* output = context.Read<TextBatch>("actual_output");
      if (std::string(test.port) == "output" && test.fault < 2) {
        EXPECT_EQ(output, nullptr);
      } else {
        ASSERT_NE(output, nullptr);
        ASSERT_EQ(output->size(), 1u);
        EXPECT_EQ(output->at(0).data, "mapped");
      }
      EXPECT_FALSE(context.Has("output"));
    }
  }
}

// ---------------------------------------------------------------------------
// ConfigurationSnapshot 与直接并发
// ---------------------------------------------------------------------------

TEST(ConfigurationSnapshotTest, UninitializedAndNullStateHandled) {
  ConfigurationSnapshot<std::string> snapshot;
  EXPECT_FALSE(snapshot.IsInitialized());
  EXPECT_EQ(snapshot.Read(), nullptr);

  auto res = snapshot.Update([](const std::string& s) {
    return NodeResult<std::string>::Success(s + "_next");
  });
  EXPECT_EQ(res.status, NodeControlStatus::kFailed);
  EXPECT_EQ(res.code, node_error::control::kInvalidRequest);

  EXPECT_FALSE(
      snapshot.Initialize(std::shared_ptr<const std::string>(nullptr)));
  EXPECT_FALSE(snapshot.IsInitialized());
}

TEST(ConfigurationSnapshotTest, InitializeAndRead) {
  ConfigurationSnapshot<std::string> snapshot;
  EXPECT_TRUE(snapshot.Initialize("initial_val"));
  EXPECT_TRUE(snapshot.IsInitialized());
  auto read_val = snapshot.Read();
  ASSERT_NE(read_val, nullptr);
  EXPECT_EQ(*read_val, "initial_val");
}

TEST(ConfigurationSnapshotTest, WriterSerializationAndIndependentPatchMerging) {
  // 写者 A 更新 prefix，写者 B 更新 suffix。
  // 两次成功更新都会保留；B 不能基于 A 之前的过期状态提交。
  struct TwoFields {
    std::string prefix;
    std::string suffix;
  };
  ConfigurationSnapshot<TwoFields> snapshot(
      TwoFields{"init_pre:", ":init_suf"});

  std::promise<void> a_entered_lock;
  std::promise<void> release_a;
  std::promise<void> b_called;

  std::atomic<bool> b_saw_a_prefix{false};

  // 写者 A 持有写锁时 B 尝试运行
  std::thread thread_a([&]() {
    snapshot.Update([&](const TwoFields& cur) {
      a_entered_lock.set_value();
      release_a.get_future().wait();
      TwoFields next = cur;
      next.prefix = "A_pre:";
      return NodeResult<TwoFields>::Success(next);
    });
  });

  a_entered_lock.get_future().wait();

  // A 处于 Update 回调中时，写者 B 尝试更新 suffix
  std::thread thread_b([&]() {
    b_called.set_value();
    snapshot.Update([&](const TwoFields& cur) {
      if (cur.prefix == "A_pre:") {
        b_saw_a_prefix = true;
      }
      TwoFields next = cur;
      next.suffix = ":B_suf";
      return NodeResult<TwoFields>::Success(next);
    });
  });

  b_called.get_future().wait();
  // B 启动前 A 已持有事务。无需时序假设：
  // B 获取锁时必然看到 A 已发布的值。

  // 放行 A，使其发布更新
  release_a.set_value();

  thread_a.join();
  thread_b.join();

  EXPECT_TRUE(b_saw_a_prefix);
  auto final_state = snapshot.Read();
  ASSERT_NE(final_state, nullptr);
  EXPECT_EQ(final_state->prefix, "A_pre:");
  EXPECT_EQ(final_state->suffix, ":B_suf");
}

TEST(ConfigurationSnapshotTest, FailedWriterRollbackPreservesActiveState) {
  // 写者在校验/候选生成阶段失败 -> 不发布新快照；
  // 旧状态保持生效且完好。
  struct State {
    std::string val;
    int rev;
  };
  ConfigurationSnapshot<State> snapshot(State{"original", 1});

  auto res = snapshot.Update([](const State&) -> NodeResult<State> {
    return NodeResult<State>::Failure(NodeErrorKind::kBusinessError,
                                      "validation failed",
                                      node_error::control::kInvalidRequest);
  });
  EXPECT_EQ(res.status, NodeControlStatus::kFailed);
  EXPECT_EQ(res.code, node_error::control::kInvalidRequest);

  auto cur = snapshot.Read();
  ASSERT_NE(cur, nullptr);
  EXPECT_EQ(cur->val, "original");
  EXPECT_EQ(cur->rev, 1);

  // 同时测试候选构建中的异常
  auto res_ex = snapshot.Update([](const State&) -> NodeResult<State> {
    throw std::runtime_error("candidate throw");
  });
  EXPECT_EQ(res_ex.status, NodeControlStatus::kFailed);
  EXPECT_EQ(snapshot.Read()->val, "original");
}

TEST(ConfigurationSnapshotTest, ReaderHoldsOldSnapshotWhileWriterPublishes) {
  // 持有旧快照的读者不受并发写者发布的影响；旧读者使用旧版本安全完成，
  // 后续读者看到新版本。
  struct State {
    std::string val;
    int rev;
  };
  ConfigurationSnapshot<State> snapshot(State{"v1", 1});

  std::promise<void> reader_acquired;
  std::promise<void> writer_published;
  std::promise<void> reader_done;

  std::shared_ptr<const State> reader_held_state;

  std::thread reader_thread([&]() {
    reader_held_state = snapshot.Read();
    reader_acquired.set_value();
    writer_published.get_future().wait();
    EXPECT_EQ(reader_held_state->val, "v1");
    EXPECT_EQ(reader_held_state->rev, 1);
    reader_done.set_value();
  });

  reader_acquired.get_future().wait();

  auto res = snapshot.Update(
      [](const State&) { return NodeResult<State>::Success(State{"v2", 2}); });
  EXPECT_EQ(res.status, NodeControlStatus::kHandled);

  writer_published.set_value();
  reader_done.get_future().wait();
  reader_thread.join();

  auto fresh = snapshot.Read();
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh->val, "v2");
  EXPECT_EQ(fresh->rev, 2);
}

TEST(ConfigurationSnapshotTest, TwoWritersSameFieldOrdered) {
  struct State {
    std::string tag;
  };
  ConfigurationSnapshot<State> snapshot(State{"init"});

  auto res1 = snapshot.Update(
      [](const State&) { return NodeResult<State>::Success(State{"first"}); });
  EXPECT_EQ(res1.status, NodeControlStatus::kHandled);

  auto res2 = snapshot.Update(
      [](const State&) { return NodeResult<State>::Success(State{"second"}); });
  EXPECT_EQ(res2.status, NodeControlStatus::kHandled);

  EXPECT_EQ(snapshot.Read()->tag, "second");
}

TEST(ConfigurationSnapshotTest, OldReaderOutlivesOwnerAndReleasesState) {
  auto owner = std::make_unique<ConfigurationSnapshot<std::string>>("old");
  auto reader = owner->Read();
  std::weak_ptr<const std::string> old_state = reader;
  ASSERT_EQ(owner
                ->Update([](const std::string&) {
                  return NodeResult<std::string>::Success("new");
                })
                .status,
            NodeControlStatus::kHandled);
  EXPECT_FALSE(old_state.expired());
  owner.reset();
  ASSERT_FALSE(old_state.expired());
  EXPECT_EQ(*reader, "old");
  reader.reset();
  EXPECT_TRUE(old_state.expired());
}

TEST(ConfigurationSnapshotTest, MoveOnlyStateHandled) {
  struct MoveOnlyState {
    std::unique_ptr<std::string> val;
    explicit MoveOnlyState(std::string s)
        : val(std::make_unique<std::string>(std::move(s))) {}
    MoveOnlyState(MoveOnlyState&&) noexcept = default;
    MoveOnlyState& operator=(MoveOnlyState&&) noexcept = default;
    MoveOnlyState(const MoveOnlyState&) = delete;
    MoveOnlyState& operator=(const MoveOnlyState&) = delete;
  };

  ConfigurationSnapshot<MoveOnlyState> snapshot(MoveOnlyState("init"));
  EXPECT_TRUE(snapshot.IsInitialized());
  auto cur = snapshot.Read();
  ASSERT_NE(cur, nullptr);
  EXPECT_EQ(*cur->val, "init");

  auto res = snapshot.Update([](const MoveOnlyState& current) {
    return NodeResult<MoveOnlyState>::Success(
        MoveOnlyState(*current.val + "_updated"));
  });
  EXPECT_EQ(res.status, NodeControlStatus::kHandled);
  auto next = snapshot.Read();
  ASSERT_NE(next, nullptr);
  EXPECT_EQ(*next->val, "init_updated");
}

// ---------------------------------------------------------------------------
// 函数式 Spec 的 WithControls 与 NodeHarness 测试
// ---------------------------------------------------------------------------

TEST(FunctionNodeTest, ItemwiseNodeWithFieldControls) {
  NodeHarness harness("controlled_map");
  harness.Config(
      {{"prefix", "init_p:"}, {"suffix", ":init_s"}, {"multiplier", 1}});
  harness.TextInput("input", {"payload"});

  auto res1 = harness.Run();
  ASSERT_TRUE(res1.ok()) << res1.diagnostic();
  EXPECT_EQ(res1.TextValues("output"),
            (std::vector<std::string>{"init_p:payload:init_s"}));

  // 1. 只替换给出的 prefix，保留 multiplier 和 suffix。
  auto partial_replace =
      harness.Control(kCmdReplaceMap, R"({"prefix":"new_p:"})");
  EXPECT_EQ(partial_replace.status, NodeControlStatus::kHandled);

  auto res2 = harness.Run();
  ASSERT_TRUE(res2.ok());
  EXPECT_EQ(res2.TextValues("output"),
            (std::vector<std::string>{"new_p:payload:init_s"}));

  // 2. ReplaceFields 带齐所有声明字段 -> 处理成功
  auto good_replace =
      harness.Control(kCmdReplaceMap, R"({"prefix":"rep_p:","multiplier":2})");
  EXPECT_EQ(good_replace.status, NodeControlStatus::kHandled);

  // 未声明的 suffix 仍为 ":init_s"，prefix 和 multiplier 已更新
  auto res3 = harness.Run();
  ASSERT_TRUE(res3.ok());
  EXPECT_EQ(res3.TextValues("output"),
            (std::vector<std::string>{"rep_p:payloadpayload:init_s"}));

  // 3. 另一命令只控制 suffix -> 处理成功，其余字段不变
  auto good_suffix = harness.Control(kCmdSuffixMap, R"({"suffix":":patch_s"})");
  EXPECT_EQ(good_suffix.status, NodeControlStatus::kHandled);

  auto res4 = harness.Run();
  ASSERT_TRUE(res4.ok());
  EXPECT_EQ(res4.TextValues("output"),
            (std::vector<std::string>{"rep_p:payloadpayload:patch_s"}));

  // 4. 空对象缺少受控字段 -> 拒绝
  auto empty_payload = harness.Control(kCmdSuffixMap, R"({})");
  EXPECT_EQ(empty_payload.status, NodeControlStatus::kFailed);

  // 5. 语义校验失败 -> 拒绝并回滚状态
  auto invalid_prefix =
      harness.Control(kCmdReplaceMap, R"({"prefix":"INVALID","multiplier":2})");
  EXPECT_EQ(invalid_prefix.status, NodeControlStatus::kFailed);
  EXPECT_NE(invalid_prefix.message.find("Invalid prefix disallowed"),
            std::string::npos);

  auto res5 = harness.Run();
  ASSERT_TRUE(res5.ok());
  EXPECT_EQ(res5.TextValues("output"),
            (std::vector<std::string>{"rep_p:payloadpayload:patch_s"}));

  // 6. 未知命令 -> 不支持
  auto unk = harness.Control(9999, R"({})");
  EXPECT_EQ(unk.status, NodeControlStatus::kUnsupported);
}

TEST(FunctionNodeTest, BatchNodeWithControlsAndValidation) {
  NodeHarness harness("controlled_batch");
  harness.Config({{"header", "H:"}, {"uppercase", false}});
  harness.TextInput("texts", {"abc", "def"});

  auto res1 = harness.Run();
  ASSERT_TRUE(res1.ok()) << res1.diagnostic();
  EXPECT_EQ(res1.TextValues("output"),
            (std::vector<std::string>{"H:abc", "H:def"}));

  // 用 uppercase = true 做 Replace 更新
  auto ctrl1 =
      harness.Control(kCmdReplaceBatch, R"({"header":"G:","uppercase":true})");
  EXPECT_EQ(ctrl1.status, NodeControlStatus::kHandled);

  auto res2 = harness.Run();
  ASSERT_TRUE(res2.ok());
  EXPECT_EQ(res2.TextValues("output"),
            (std::vector<std::string>{"G:ABC", "G:DEF"}));

  // 语义拒绝
  auto ctrl2 = harness.Control(kCmdReplaceBatch,
                               R"({"header":"REJECT","uppercase":true})");
  EXPECT_EQ(ctrl2.status, NodeControlStatus::kFailed);
  EXPECT_NE(ctrl2.message.find("Rejected header"), std::string::npos);

  // 失败时保持不变
  auto res3 = harness.Run();
  ASSERT_TRUE(res3.ok());
  EXPECT_EQ(res3.TextValues("output"),
            (std::vector<std::string>{"G:ABC", "G:DEF"}));
}

TEST(FunctionNodeTest,
     FieldControlRejectsInvalidPartialPayloadsWithoutChangingState) {
  NodeHarness harness("controlled_map");
  harness.Config({{"prefix", "p:"}, {"suffix", ":s"}, {"multiplier", 2}});
  harness.TextInput("input", {"x"});
  ASSERT_TRUE(harness.Run().ok());
  for (const char* payload :
       {R"({})", R"({"unknown":1})", R"({"suffix":":bad"})",
        R"({"prefix":null})", R"({"prefix":42})", R"({"multiplier":"2"})",
        R"({"multiplier":0})", R"({"multiplier":11})", R"([])", "null"}) {
    SCOPED_TRACE(payload);
    const auto control = harness.Control(kCmdReplaceMap, payload);
    EXPECT_EQ(control.status, NodeControlStatus::kFailed);
    EXPECT_FALSE(control.message.empty());
    const auto after = harness.Run();
    ASSERT_TRUE(after.ok()) << after.diagnostic();
    EXPECT_EQ(after.TextValues("output"), (std::vector<std::string>{"p:xx:s"}));
  }
  ASSERT_EQ(harness.Control(kCmdReplaceMap, R"({"multiplier":3})").status,
            NodeControlStatus::kHandled);
  EXPECT_EQ(harness.Run().TextValues("output"),
            (std::vector<std::string>{"p:xxx:s"}));
}

TEST(FunctionNodeTest, ContainerControlSchemasExposeDeclaredNestedConstraints) {
  const auto definition =
      ControlledContainersSpec().BuildDefinition("ContainerSchemaProbe");
  ASSERT_EQ(definition.control_commands.size(), 1U);
  const auto& schema = definition.control_commands[0].payload_schema;
  EXPECT_EQ(schema["type"], "object");
  EXPECT_EQ(schema["minProperties"], 1);
  EXPECT_EQ(schema["additionalProperties"], false);
  EXPECT_FALSE(schema.contains("required"));
  EXPECT_EQ(schema["properties"]["groups"]["type"], "object");
  EXPECT_EQ(schema["properties"]["groups"]["additionalProperties"]["type"],
            "array");
  EXPECT_EQ(
      schema["properties"]["groups"]["additionalProperties"]["items"]["enum"],
      (nlohmann::json{"fast", "slow"}));
  const auto& element = schema["properties"]["rules"]["items"];
  EXPECT_EQ(element["type"], "object");
  EXPECT_EQ(element["required"], (nlohmann::json{"text"}));
  EXPECT_EQ(element["additionalProperties"], false);
  EXPECT_EQ(element["properties"]["repeats"]["minimum"], 1);
  EXPECT_EQ(element["properties"]["repeats"]["maximum"], 3);
  EXPECT_EQ(element["properties"]["repeats"]["default"], 1);
  EXPECT_EQ(schema["properties"]["fallback"]["not"],
            (nlohmann::json{{"type", "null"}}));
}

TEST(FunctionNodeTest,
     ContainerControlReplacesProvidedFieldsAndRollsBackAllFailures) {
  NodeHarness harness("controlled_containers");
  harness.TextInput("input", {"payload"});
  auto initial = harness.Run();
  ASSERT_TRUE(initial.ok()) << initial.diagnostic();
  auto state = nlohmann::json::parse(initial.TextValues("output")[0]);
  EXPECT_EQ(state["groups"], (nlohmann::json{{"old", {"fast"}}}));
  ASSERT_EQ(
      harness.Control(kCmdContainers, R"({"groups":{"new":["slow"]}})").status,
      NodeControlStatus::kHandled);
  ASSERT_EQ(
      harness
          .Control(kCmdContainers,
                   R"({"rules":[{"text":"one"},{"text":"two","repeats":2}]})")
          .status,
      NodeControlStatus::kHandled);
  ASSERT_EQ(
      harness
          .Control(kCmdContainers, R"({"fallback":[1,null],"label":"updated"})")
          .status,
      NodeControlStatus::kHandled);
  auto after = harness.Run();
  ASSERT_TRUE(after.ok()) << after.diagnostic();
  state = nlohmann::json::parse(after.TextValues("output")[0]);
  EXPECT_EQ(state["groups"], (nlohmann::json{{"new", {"slow"}}}));
  EXPECT_EQ(state["label"], "updated");
  EXPECT_EQ(state["prepared"], "updated:1:2");
  EXPECT_EQ(state["fallback"], (nlohmann::json{1, nullptr}));
  ASSERT_EQ(state["rules"].size(), 2U);
  EXPECT_EQ(state["rules"][0]["repeats"], 1);
  EXPECT_EQ(state["rules"][0]["prepared"], "prepared:one");

  for (const char* payload :
       {R"({"groups":{"bad":["other"]}})", R"({"groups":{"new":[null]}})",
        R"({"rules":[{"text":"bad","repeats":0}]})", R"({"rules":[{}]})",
        R"({"rules":[{"text":"bad","extra":1}]})", R"({"fallback":null})",
        R"({"label":"should rollback","rules":[{"text":"FAIL_PREPARE"}]})",
        R"({"label":"should rollback","rules":[{"text":"FAIL_VALIDATE"}]})",
        R"({"label":"should rollback","fallback":{"reject_prepare":true}})",
        R"({"label":"should rollback","groups":{"blocked":["fast"]}})"}) {
    SCOPED_TRACE(payload);
    const auto control = harness.Control(kCmdContainers, payload);
    EXPECT_EQ(control.status, NodeControlStatus::kFailed);
    EXPECT_FALSE(control.message.empty());
    auto unchanged = harness.Run();
    ASSERT_TRUE(unchanged.ok()) << unchanged.diagnostic();
    EXPECT_EQ(nlohmann::json::parse(unchanged.TextValues("output")[0]), state);
  }
  ASSERT_EQ(
      harness.Control(kCmdContainers, R"({"rules":[{"text":"replacement"}]})")
          .status,
      NodeControlStatus::kHandled);
  auto replaced = harness.Run();
  ASSERT_TRUE(replaced.ok());
  const auto final_state =
      nlohmann::json::parse(replaced.TextValues("output")[0]);
  ASSERT_EQ(final_state["rules"].size(), 1U);
  EXPECT_EQ(final_state["rules"][0]["text"], "replacement");
  EXPECT_EQ(final_state["groups"], state["groups"]);
  EXPECT_EQ(final_state["fallback"], state["fallback"]);
}

TEST(FunctionNodeTest, IncludedParameterBindingsSurviveCopyAndFieldControl) {
  struct Inner {
    std::string prefix;
    std::string prepared;
  };
  struct Params {
    Inner inner;
    int count = 0;
    std::string prepared;
  };
  auto group =
      Parameters<Inner>{Field("prefix", &Inner::prefix).Default("old:")}
          .Prepare([](Inner* value, std::string* error) {
            if (value->prefix == "reject") {
              if (error) *error = "included prepare rejected";
              return false;
            }
            value->prepared = "compiled:" + value->prefix;
            return true;
          });
  const auto original =
      Parameters<Params>{Field("count", &Params::count).Default(2)}
          .Include(&Params::inner, std::move(group))
          .Prepare([](Params* value, std::string*) {
            value->prepared = value->inner.prepared;
            return true;
          });
  auto copy = original;
  std::string error;
  const auto initial = copy.Parse(nlohmann::json::object(), &error);
  ASSERT_TRUE(initial.has_value()) << error;
  ConfigurationSnapshot<Params> snapshot(*initial);
  const auto command =
      ReplaceFields(3020, "set_included_prefix", {"prefix", "count"});
  EXPECT_NO_THROW(ValidateControlCommands({command}, copy));
  const auto result =
      command.Execute(copy, R"({"prefix":"new:"})", BindingFacts{}, snapshot);
  ASSERT_EQ(result.status, NodeControlStatus::kHandled) << result.message;
  auto value = snapshot.Read();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->inner.prefix, "new:");
  EXPECT_EQ(value->count, 2);
  EXPECT_EQ(value->prepared, "compiled:new:");
  const auto failed = command.Execute(copy, R"({"prefix":"reject","count":9})",
                                      BindingFacts{}, snapshot);
  EXPECT_EQ(failed.status, NodeControlStatus::kFailed);
  value = snapshot.Read();
  EXPECT_EQ(value->inner.prefix, "new:");
  EXPECT_EQ(value->count, 2);
  EXPECT_EQ(value->prepared, "compiled:new:");
}

TEST(FunctionNodeTest, SnapshotPauseTimeoutFailsProcess) {
  NodeHarness harness("controlled_map");
  ASSERT_TRUE(harness.EnsureInitialized());
  AlgContext ctx;
  ctx.Publish("bk_in_input", TextBatch{{101, 3, "sample"}});
  test_support::NodeProcessPause pause(std::chrono::milliseconds(0));
  int result = 0;
  {
    test_support::ScopedNextAllocationCallback callback(
        &test_support::NodeProcessPause::OnAllocation, &pause);
    result = harness.GetNode()->Process(&ctx);
  }
  EXPECT_NE(result, 0);
  EXPECT_NE(ctx.GetErrorMessage().find("handshake timed out"),
            std::string::npos);
  EXPECT_EQ(ctx.Read<TextBatch>("bk_out_output"), nullptr);
}

TEST(FunctionNodeTest, WholeBatchProcessConsistencyDuringControl) {
  NodeHarness harness("controlled_map");
  harness.Config({{"prefix", "v1:"}, {"suffix", ":s1"}, {"multiplier", 1}});
  ASSERT_TRUE(harness.EnsureInitialized());
  auto* node = harness.GetNode();
  ASSERT_NE(node, nullptr);

  TextBatch batch;
  for (uint64_t i = 0; i < 50; ++i) {
    batch.emplace_back(100 + i, i, "sample_" + std::to_string(i));
  }
  AlgContext old_ctx;
  old_ctx.Publish("bk_in_input", batch);
  test_support::NodeProcessPause pause;
  auto reader = std::async(std::launch::async, [&] {
    // 第一次分配是 outputs.reserve，发生在 AuthorNode 获取参数快照之后。
    // 回调仅限于该读者线程。
    test_support::ScopedNextAllocationCallback callback(
        &test_support::NodeProcessPause::OnAllocation, &pause);
    return node->Process(&old_ctx);
  });
  const bool paused = pause.WaitUntilPaused();
  NodeControlResult control = NodeControlResult::Unsupported();
  if (paused) {
    control =
        node->Control(kCmdReplaceMap, R"({"prefix":"v2:","multiplier":2})");
  }
  pause.Resume();
  const int process_result = reader.get();
  ASSERT_TRUE(paused) << "Reader did not reach its snapshot pause";
  ASSERT_EQ(control.status, NodeControlStatus::kHandled);
  ASSERT_EQ(process_result, 0) << old_ctx.GetErrorMessage();

  const auto* old_output = old_ctx.Read<TextBatch>("bk_out_output");
  ASSERT_NE(old_output, nullptr);
  ASSERT_EQ(old_output->size(), batch.size());
  for (size_t i = 0; i < batch.size(); ++i) {
    EXPECT_EQ((*old_output)[i].data, "v1:" + batch[i].data + ":s1");
    EXPECT_EQ((*old_output)[i].req_id, batch[i].req_id);
    EXPECT_EQ((*old_output)[i].sub_id, batch[i].sub_id);
  }
  AlgContext new_ctx;
  new_ctx.Publish("bk_in_input", batch);
  ASSERT_EQ(node->Process(&new_ctx), 0);
  const auto* new_output = new_ctx.Read<TextBatch>("bk_out_output");
  ASSERT_NE(new_output, nullptr);
  ASSERT_EQ(new_output->size(), batch.size());
  for (size_t i = 0; i < batch.size(); ++i) {
    EXPECT_EQ((*new_output)[i].data,
              "v2:" + batch[i].data + batch[i].data + ":s1");
    EXPECT_EQ((*new_output)[i].req_id, batch[i].req_id);
    EXPECT_EQ((*new_output)[i].sub_id, batch[i].sub_id);
  }
}

TEST(FunctionNodeTest, WholeBatchProcessConsistencyDuringControlForBatchNode) {
  NodeHarness harness("controlled_batch");
  harness.Config({{"header", "old:"}, {"uppercase", false}});
  ASSERT_TRUE(harness.EnsureInitialized());
  auto* node = harness.GetNode();
  ASSERT_NE(node, nullptr);

  TextBatch batch;
  for (uint64_t i = 0; i < 50; ++i) {
    batch.emplace_back(300 + i, i, "sample");
  }
  AlgContext old_ctx;
  old_ctx.Publish("bk_in_texts", batch);
  test_support::NodeProcessPause pause;
  auto reader = std::async(std::launch::async, [&] {
    // ControlledBatchFn 在 AuthorNode 获取快照后预留输出。
    test_support::ScopedNextAllocationCallback callback(
        &test_support::NodeProcessPause::OnAllocation, &pause);
    return node->Process(&old_ctx);
  });
  const bool paused = pause.WaitUntilPaused();
  NodeControlResult control = NodeControlResult::Unsupported();
  if (paused) {
    control = node->Control(kCmdReplaceBatch,
                            R"({"header":"new:","uppercase":true})");
  }
  pause.Resume();
  const int process_result = reader.get();
  ASSERT_TRUE(paused) << "Reader did not reach its snapshot pause";
  ASSERT_EQ(control.status, NodeControlStatus::kHandled);
  ASSERT_EQ(process_result, 0) << old_ctx.GetErrorMessage();

  const auto* old_output = old_ctx.Read<TextBatch>("bk_out_output");
  ASSERT_NE(old_output, nullptr);
  ASSERT_EQ(old_output->size(), batch.size());
  for (size_t i = 0; i < batch.size(); ++i) {
    EXPECT_EQ((*old_output)[i].data, "old:sample");
    EXPECT_EQ((*old_output)[i].req_id, batch[i].req_id);
    EXPECT_EQ((*old_output)[i].sub_id, batch[i].sub_id);
  }
  AlgContext new_ctx;
  new_ctx.Publish("bk_in_texts", batch);
  ASSERT_EQ(node->Process(&new_ctx), 0);
  const auto* new_output = new_ctx.Read<TextBatch>("bk_out_output");
  ASSERT_NE(new_output, nullptr);
  ASSERT_EQ(new_output->size(), batch.size());
  for (size_t i = 0; i < batch.size(); ++i) {
    EXPECT_EQ((*new_output)[i].data, "NEW:SAMPLE");
    EXPECT_EQ((*new_output)[i].req_id, batch[i].req_id);
    EXPECT_EQ((*new_output)[i].sub_id, batch[i].sub_id);
  }
}

TEST(FunctionNodeTest,
     SpecWithNonCopyableParamsCompilesAndExecutesWithoutControls) {
  // 验证 ParamsT 不可拷贝 (含 unique_ptr) 的 MapSpec
  NodeHarness map_harness("non_copyable_map");
  map_harness.Config({{"prefix", "map_nc:"}});
  map_harness.TextInput("input", {"hello", "world"});
  auto map_res = map_harness.Run();
  ASSERT_TRUE(map_res.ok()) << map_res.diagnostic();
  EXPECT_EQ(map_res.TextValues("output"),
            (std::vector<std::string>{"map_nc:hello_100", "map_nc:world_100"}));

  auto ctrl_map = map_harness.Control(1001, R"({})");
  EXPECT_EQ(ctrl_map.status, NodeControlStatus::kUnsupported);

  // 验证 ParamsT 不可拷贝 (含 unique_ptr) 的 BatchSpec
  NodeHarness batch_harness("non_copyable_batch");
  batch_harness.Config({{"tag", "batch_nc:"}});
  batch_harness.TextInput("texts", {"foo", "bar"});
  auto batch_res = batch_harness.Run();
  ASSERT_TRUE(batch_res.ok()) << batch_res.diagnostic();
  EXPECT_EQ(batch_res.TextValues("output"),
            (std::vector<std::string>{"batch_nc:foo_200", "batch_nc:bar_200"}));

  auto ctrl_batch = batch_harness.Control(1001, R"({})");
  EXPECT_EQ(ctrl_batch.status, NodeControlStatus::kUnsupported);
}

TEST(FunctionNodeTest, SpecWithoutWithControlsReturnsUnsupported) {
  NodeHarness harness("upper_map");
  harness.TextInput("input", {"hello"});
  ASSERT_TRUE(harness.EnsureInitialized());
  auto ctrl = harness.Control(1001, R"({})");
  EXPECT_EQ(ctrl.status, NodeControlStatus::kUnsupported);
}

TEST(FunctionNodeTest, DeclarationValidationRejectsInvalidControlCommands) {
  struct DummyParams {
    std::string text;
    int count = 0;
  };
  auto make_params = []() {
    return Parameters<DummyParams>({
        Field("text", &DummyParams::text).Default(""),
        Field("count", &DummyParams::count).Default(0),
    });
  };

  // 1. 非法命令 ID (<= 0)
  EXPECT_THROW(ValidateControlCommands({ReplaceFields(0, "set_text", {"text"})},
                                       make_params()),
               std::invalid_argument);

  // 2. 命令名为空
  EXPECT_THROW(ValidateControlCommands({ReplaceFields(1001, "", {"text"})},
                                       make_params()),
               std::invalid_argument);

  // 3. 命令 ID 重复
  EXPECT_THROW(
      ValidateControlCommands({ReplaceFields(1001, "cmd_a", {"text"}),
                               ReplaceFields(1001, "cmd_b", {"count"})},
                              make_params()),
      std::invalid_argument);

  // 4. 命令名重复
  EXPECT_THROW(
      ValidateControlCommands({ReplaceFields(1001, "same_name", {"text"}),
                               ReplaceFields(1002, "same_name", {"count"})},
                              make_params()),
      std::invalid_argument);

  // 5. 字段名为空
  EXPECT_THROW(
      ValidateControlCommands({ReplaceFields(1001, "cmd", {})}, make_params()),
      std::invalid_argument);

  // 6. 同一命令内字段名重复
  EXPECT_THROW(
      ValidateControlCommands({ReplaceFields(1001, "cmd", {"text", "text"})},
                              make_params()),
      std::invalid_argument);

  // 7. 字段名未绑定
  EXPECT_THROW(
      ValidateControlCommands({ReplaceFields(1001, "cmd", {"non_existent"})},
                              make_params()),
      std::invalid_argument);
}

TEST(FunctionNodeTest, FieldControlsCannotReplaceModelReferences) {
  struct Params {
    std::string bind_model;
  };
  const auto params =
      Parameters<Params>{Field("bind_model", &Params::bind_model).Required()};
  const auto models = ModelsOf<AnswerModels>{
      Model("generator", "bind_model", &AnswerModels::llm)};
  EXPECT_THROW(ValidateControlCommands(
                   {ReplaceFields(3030, "replace_model", {"bind_model"})},
                   params, &models),
               std::invalid_argument);
}

TEST(FunctionNodeTest, PreservedOutputDeclarationsRequireNamedAnchors) {
  struct Outputs {
    TextBatch text;
  };
  EXPECT_THROW(PreservedOutput<TextBatch>("text", ""), std::invalid_argument);
  EXPECT_THROW(Produced("text", &Outputs::text, std::string{}),
               std::invalid_argument);
}

TEST(FunctionNodeTest,
     OutputDeclarationsRejectReusedMembersAcrossFlowOverloads) {
  struct Outputs {
    TextBatch items;
  };
  const auto first = Produced("a", &Outputs::items);
  const std::vector<OutputBindingHolder<Outputs>> aliases = {
      Produced("b", &Outputs::items),
      Produced("b", &Outputs::items,
               PortFlow{"1:N", "generate_sub_id", "request"}),
      Produced("b", &Outputs::items, "input"),
      Produced("b", &Outputs::items, PortFlow{}, "input")};
  for (size_t i = 0; i < aliases.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_THROW((OutputsOf<Outputs>{first, aliases[i]}),
                 std::invalid_argument);
  }
}

TEST(FunctionNodeTest, CopiedOutputBindingStillRejectsReusedMember) {
  struct Outputs {
    TextBatch items;
  };
  const auto original = Produced("a", &Outputs::items);
  const auto copy = original;
  EXPECT_THROW((OutputsOf<Outputs>{copy, Produced("b", &Outputs::items)}),
               std::invalid_argument);
}

TEST(FunctionNodeTest,
     OutputDeclarationsAllowDistinctMembersWithoutValueCreation) {
  struct Outputs {
    Outputs() = delete;
    TextBatch first;
    TextBatch second;
    Int32Batch counts;
  };
  const OutputsOf<Outputs> outputs{Produced("first", &Outputs::first),
                                   Produced("second", &Outputs::second),
                                   Produced("counts", &Outputs::counts)};
  const auto definitions = outputs.ToPortDefinitions();
  ASSERT_EQ(definitions.size(), 3u);
  EXPECT_EQ(definitions[0].logical_name, "first");
  EXPECT_EQ(definitions[1].logical_name, "second");
  EXPECT_EQ(definitions[2].logical_name, "counts");
}

TEST(FunctionNodeTest, OutputDeclarationsStillRejectDuplicatePortNames) {
  struct Outputs {
    TextBatch first;
    TextBatch second;
  };
  EXPECT_THROW((OutputsOf<Outputs>{Produced("same", &Outputs::first),
                                   Produced("same", &Outputs::second)}),
               std::invalid_argument);
}

TEST(FunctionNodeTest, TypedPrepareHookExecutesAndCanReject) {
  struct PreparedParams {
    std::string raw;
    std::string derived;
  };

  auto params = Parameters<PreparedParams>(
                    {
                        Field("raw", &PreparedParams::raw).Default(""),
                    })
                    .Prepare([](PreparedParams* p, const BindingFacts&,
                                std::string* err) {
                      if (p->raw == "FAIL_PREPARE") {
                        if (err) *err = "Prepare rejected";
                        return false;
                      }
                      p->derived = "prepared:" + p->raw;
                      return true;
                    });

  BindingFacts facts;
  std::string err;
  auto ok_res = params.ParseNormalized({{"raw", "hello"}}, facts, &err);
  ASSERT_TRUE(ok_res.has_value());
  EXPECT_EQ(ok_res->derived, "prepared:hello");

  auto fail_res =
      params.ParseNormalized({{"raw", "FAIL_PREPARE"}}, facts, &err);
  EXPECT_FALSE(fail_res.has_value());
  EXPECT_NE(err.find("Prepare rejected"), std::string::npos);
}

struct BindingFactsProbeParams {
  std::string mode;
  bool plan_seen = false;
  bool input_connected = false;
  std::string input_lifetime;
};

inline auto BindingFactsProbeSpec() {
  return TextMapSpec(
      Parameters<BindingFactsProbeParams>(
          {
              Field("mode", &BindingFactsProbeParams::mode).Default("base"),
          })
          .Prepare([](BindingFactsProbeParams* p, const BindingFacts& facts,
                      std::string*) {
            p->plan_seen = facts.has_bindings;
            p->input_connected = facts.IsConnected("input");
            p->input_lifetime = facts.InputLifetime("input");
            return true;
          }),
      [](const std::string& in, const BindingFactsProbeParams& p) {
        return std::string(p.plan_seen ? "PLAN:" : "NO_PLAN:") +
               (p.input_connected ? "CONN:" : "DISCONN:") +
               (p.mode == "with_lifetime" ? p.input_lifetime + ":" : "") + in;
      });
}
REGISTER_FUNCTION_NODE(binding_facts_probe, BindingFactsProbeSpec());

TEST(FunctionNodeTest, AuthorNodeInitPassesRealBindingFactsToPrepare) {
  // 测试有计划的执行：plan_seen 必须为 true，且输入必须已连接
  NodeHarness harness_planned("binding_facts_probe");
  harness_planned.TextInput("input", {"hello"});
  auto res_planned = harness_planned.Run();
  ASSERT_TRUE(res_planned.ok()) << res_planned.diagnostic();
  EXPECT_EQ(res_planned.TextValues("output"),
            (std::vector<std::string>{"PLAN:CONN:hello"}));
}

TEST(FunctionNodeTest, AuthorNodePrepareReadsActualInputLifetime) {
  for (const char* lifetime : {"request", "session"}) {
    SCOPED_TRACE(lifetime);
    NodeHarness harness("binding_facts_probe");
    harness.Config({{"mode", "with_lifetime"}})
        .InputLifetime("input", lifetime)
        .TextInput("input", {"hello"});
    auto result = harness.Run();
    ASSERT_TRUE(result.ok()) << result.diagnostic();
    EXPECT_EQ(result.TextValues("output"),
              (std::vector<std::string>{"PLAN:CONN:" + std::string(lifetime) +
                                        ":hello"}));
  }
}

TEST(FunctionNodeTest, RapidInterleavedControlsAndConcurrentProcesses) {
  NodeHarness harness("controlled_map");
  harness.Config({{"prefix", "p0:"}, {"suffix", ":s0"}, {"multiplier", 1}});
  ASSERT_TRUE(harness.EnsureInitialized());
  auto* node = harness.GetNode();
  ASSERT_NE(node, nullptr);

  std::promise<void> start_signal;
  const auto start = start_signal.get_future().share();
  std::atomic<int> successful_processes{0};

  // 写线程 1：快速 ReplaceFields
  std::thread writer1([&]() {
    start.wait();
    for (int i = 1; i <= 30; ++i) {
      std::string payload =
          "{\"prefix\":\"p" + std::to_string(i) +
          ":\",\"multiplier\":" + std::to_string((i % 3) + 1) + "}";
      auto res = node->Control(kCmdReplaceMap, payload);
      EXPECT_EQ(res.status, NodeControlStatus::kHandled);
      std::this_thread::yield();
    }
  });

  // 写线程 2：快速替换 suffix
  std::thread writer2([&]() {
    start.wait();
    for (int i = 1; i <= 30; ++i) {
      std::string payload = "{\"suffix\":\":s" + std::to_string(i) + "\"}";
      auto res = node->Control(kCmdSuffixMap, payload);
      EXPECT_EQ(res.status, NodeControlStatus::kHandled);
      std::this_thread::yield();
    }
  });

  // 多个读线程：用多条目批次并发调用 Process
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&, r]() {
      start.wait();
      uint64_t req_id = 1000 + r * 10000;
      for (int iteration = 0; iteration < 30; ++iteration) {
        AlgContext ctx;
        TextBatch input;
        for (int i = 0; i < 8; ++i) {
          input.emplace_back(req_id, i, "payload_" + std::to_string(i));
        }
        req_id++;
        ctx.Publish("bk_in_input", std::move(input));
        int rc = node->Process(&ctx);
        EXPECT_EQ(rc, 0);
        const auto* out = ctx.Read<TextBatch>("bk_out_output");
        ASSERT_NE(out, nullptr);
        ASSERT_EQ(out->size(), 8u);

        // 验证批内快照一致性：
        // 条目格式：prefix + (multiplier * "payload_i") + suffix
        // 从条目 0 提取 prefix、suffix 和 multiplier：
        const std::string& item0 = (*out)[0].data;
        auto pos0 = item0.find("payload_0");
        ASSERT_NE(pos0, std::string::npos);
        std::string prefix = item0.substr(0, pos0);
        auto last_pos0 = item0.rfind("payload_0");
        std::string suffix =
            item0.substr(last_pos0 + std::string("payload_0").size());
        int multiplier = 0;
        size_t sp = 0;
        while ((sp = item0.find("payload_0", sp)) != std::string::npos) {
          multiplier++;
          sp += std::string("payload_0").size();
        }
        // 本批中其他条目必须与同一份快照参数完全一致：
        for (int i = 1; i < 8; ++i) {
          std::string expected = prefix;
          for (int m = 0; m < multiplier; ++m) {
            expected += "payload_" + std::to_string(i);
          }
          expected += suffix;
          EXPECT_EQ((*out)[i].data, expected)
              << "Batch mixed configuration snapshots between items!";
        }
        successful_processes.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  start_signal.set_value();
  writer1.join();
  writer2.join();
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(successful_processes.load(), 4 * 30);

  // 验证 Node 最终仍处于一致状态
  AlgContext final_ctx;
  final_ctx.Publish("bk_in_input", TextBatch{{999, 0, "final"}});
  ASSERT_EQ(node->Process(&final_ctx), 0);
  const auto* final_out = final_ctx.Read<TextBatch>("bk_out_output");
  ASSERT_NE(final_out, nullptr);
  ASSERT_EQ(final_out->size(), 1u);
  EXPECT_EQ(final_out->at(0).data, "p30:final:s30");
}

TEST(FunctionNodeTest, SessionResultCachesSuccessfulValueAcrossFacades) {
  SessionContext session;
  SessionResources resources(session);
  const SessionResourceKey<std::string> key("result-cache-success");
  int constructions = 0;
  auto first = resources.GetOrCreateResult<std::string>(key, [&] {
    ++constructions;
    return NodeResult<std::string>::Success("prepared value");
  });
  ASSERT_TRUE(first.ok());
  ASSERT_NE(first.value(), nullptr);
  EXPECT_EQ(*first.value(), "prepared value");

  auto cached =
      SessionResources(session).GetOrCreateResult<std::string>(key, [&] {
        ++constructions;
        return NodeResult<std::string>::Failure(NodeErrorKind::kInternalError,
                                                "must not run");
      });
  ASSERT_TRUE(cached.ok());
  EXPECT_EQ(cached.value(), first.value());
  EXPECT_EQ(constructions, 1);
}

TEST(FunctionNodeTest,
     SessionResultPreservesFailureAndRetriesWithoutCachingIt) {
  SessionContext session;
  SessionResources resources(session);
  const SessionResourceKey<std::string> key("result-cache-retry");
  const NodeFailure expected(
      NodeErrorKind::kModelCallError, "embedding unavailable",
      BatchFailureDetail{"embed corpus", BatchFailureReason::kCallbackFailed,
                         TraceableItemKey{17, 3}},
      -47, "prepare corpus", "embedding", "corpus_builder.cpp:42");
  int constructions = 0;
  auto failed = resources.GetOrCreateResult<std::string>(key, [&] {
    ++constructions;
    return NodeResult<std::string>::Failure(expected);
  });
  ASSERT_FALSE(failed.ok());
  const auto& actual = failed.failure();
  EXPECT_EQ(actual.kind, expected.kind);
  EXPECT_EQ(actual.message, expected.message);
  EXPECT_EQ(actual.cause_code, expected.cause_code);
  EXPECT_EQ(actual.stage, expected.stage);
  EXPECT_EQ(actual.model_slot, expected.model_slot);
  EXPECT_EQ(actual.source_location, expected.source_location);
  ASSERT_TRUE(actual.batch_detail.has_value());
  EXPECT_EQ(actual.batch_detail->operation, expected.batch_detail->operation);
  EXPECT_EQ(actual.batch_detail->reason, expected.batch_detail->reason);
  EXPECT_EQ(actual.batch_detail->key, expected.batch_detail->key);

  auto retried = resources.GetOrCreateResult<std::string>(key, [&] {
    ++constructions;
    return NodeResult<std::string>::Success("recovered");
  });
  ASSERT_TRUE(retried.ok());
  ASSERT_NE(retried.value(), nullptr);
  EXPECT_EQ(*retried.value(), "recovered");
  EXPECT_EQ(constructions, 2);
}

TEST(FunctionNodeTest,
     SessionResultLeavesUnexpectedExceptionsForRuntimeBarrier) {
  SessionContext session;
  SessionResources resources(session);
  const SessionResourceKey<std::string> key("result-cache-exception");
  EXPECT_THROW(resources.GetOrCreateResult<std::string>(
                   key,
                   []() -> NodeResult<std::string> {
                     throw std::runtime_error("unexpected preparation failure");
                   }),
               std::runtime_error);
  auto retried = resources.GetOrCreateResult<std::string>(
      key, [] { return NodeResult<std::string>::Success("recovered"); });
  ASSERT_TRUE(retried.ok());
  EXPECT_EQ(*retried.value(), "recovered");
}

}  // namespace llm_edgeflow

namespace llm_edgeflow {

TEST(FunctionNodeTest, ImageInputPublishesMultipleTypedOutputsWithProvenance) {
  NodeHarness harness("image_summary_author");
  harness.CustomInput("images",
                      ImageRefBatch{{91, 7, "a.png"}, {52, 3, "bb.jpg"}});
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* names = result.Output<TextBatch>("names");
  const auto* lengths = result.Output<Int32Batch>("lengths");
  ASSERT_NE(names, nullptr);
  ASSERT_NE(lengths, nullptr);
  ASSERT_EQ(names->size(), 2u);
  ASSERT_EQ(lengths->size(), 2u);
  EXPECT_EQ(names->at(0).data, "a.png");
  EXPECT_EQ(names->at(1).data, "bb.jpg");
  EXPECT_EQ(lengths->at(0).data, 5);
  EXPECT_EQ(lengths->at(1).data, 6);
  EXPECT_EQ(names->at(0).req_id, 91u);
  EXPECT_EQ(names->at(0).sub_id, 7u);
  EXPECT_EQ(names->at(1).req_id, 52u);
  EXPECT_EQ(names->at(1).sub_id, 3u);
  for (size_t i = 0; i < names->size(); ++i) {
    EXPECT_EQ(lengths->at(i).req_id, names->at(i).req_id);
    EXPECT_EQ(lengths->at(i).sub_id, names->at(i).sub_id);
  }
}

TEST(FunctionNodeTest,
     MultipleOutputsAreUnpublishedWhenSecondOutputOrBusinessFails) {
  for (const std::string fault : {"count", "provenance", "business"}) {
    SCOPED_TRACE(fault);
    NodeHarness harness("image_summary_author");
    harness.Config({{"fault", fault}});
    harness.CustomInput("images", ImageRefBatch{{91, 7, "a.png"}});
    auto result = harness.Run();
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.Output<TextBatch>("names"), nullptr);
    EXPECT_EQ(result.Output<Int32Batch>("lengths"), nullptr);
    if (fault == "business") {
      EXPECT_EQ(result.process_code(), -9876);
      EXPECT_NE(result.diagnostic().find("Cannot summarize image"),
                std::string::npos);
    } else {
      EXPECT_NE(result.diagnostic().find("lengths"), std::string::npos);
      EXPECT_EQ(result.process_code(),
                fault == "count"
                    ? node_error::author_node::kOutputCountMismatch
                    : node_error::author_node::kOutputProvenanceMismatch);
    }
  }
}

TEST(FunctionNodeTest, MultipleOutputsRejectWrongTypeInSecondPlannedBinding) {
  ValidatedNodePlan plan;
  plan.normalized_params = {{"fault", ""}};
  plan.ports = {{"images", "images", "ImageRefBatch", "1:1", "preserve",
                 "request", PortDirection::kInput},
                {"names", "names", "TextBatch", "1:1", "preserve", "request",
                 PortDirection::kOutput},
                {"lengths", "lengths", "TextBatch", "1:1", "preserve",
                 "request", PortDirection::kOutput}};
  auto node = NodeRegistry::Instance().Create("image_summary_author");
  ASSERT_NE(node, nullptr);
  SessionContext session;
  std::string diagnostic;
  EXPECT_FALSE(node->Init({&plan, &session, &diagnostic}));
  EXPECT_NE(diagnostic.find("lengths"), std::string::npos);
  EXPECT_NE(diagnostic.find("type mismatch"), std::string::npos);
}

TEST(FunctionNodeTest,
     UnreferencedInvalidOutputPreventsPublishingReferencedOutput) {
  for (const std::string fault : {"count", "provenance"}) {
    SCOPED_TRACE(fault);
    ValidatedNodePlan plan;
    plan.normalized_params = {{"fault", fault}};
    plan.ports = {{"images", "input.images", "ImageRefBatch", "1:1", "preserve",
                   "request", PortDirection::kInput},
                  {"names", "summary.names", "TextBatch", "1:1", "preserve",
                   "request", PortDirection::kOutput}};
    EXPECT_EQ(plan.FindPort("lengths", PortDirection::kOutput), nullptr);
    auto node = NodeRegistry::Instance().Create("image_summary_author");
    ASSERT_NE(node, nullptr);
    SessionContext session;
    std::string diagnostic;
    ASSERT_TRUE(node->Init({&plan, &session, &diagnostic})) << diagnostic;
    AlgContext context;
    context.Publish("input.images", ImageRefBatch{{91, 7, "a.png"}});
    EXPECT_EQ(node->Process(&context),
              fault == "count"
                  ? node_error::author_node::kOutputCountMismatch
                  : node_error::author_node::kOutputProvenanceMismatch);
    EXPECT_NE(context.GetErrorMessage().find("lengths"), std::string::npos);
    EXPECT_FALSE(context.Has("summary.names"));
    EXPECT_FALSE(context.Has("names"));
    EXPECT_FALSE(context.Has("lengths"));
  }
}

TEST(FunctionNodeTest, SourceWithoutInputsProducesDeclaredVariableCardinality) {
  NodeHarness harness("generated_source_author");
  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  EXPECT_EQ(result.TextValues("chunks"),
            (std::vector<std::string>{"first", "second", "third"}));
  const auto* output = result.Output<TextBatch>("chunks");
  ASSERT_NE(output, nullptr);
  for (size_t i = 0; i < output->size(); ++i) {
    EXPECT_EQ(output->at(i).req_id, 41u);
    EXPECT_EQ(output->at(i).sub_id, i);
  }
  const auto definition = PipelineCatalog::FindNode("generated_source_author");
  ASSERT_TRUE(definition.has_value());
  EXPECT_TRUE(definition->inputs.empty());
  ASSERT_EQ(definition->outputs.size(), 1u);
  EXPECT_EQ(definition->outputs[0].cardinality, "1:N");
  EXPECT_EQ(definition->outputs[0].provenance_policy, "generate_sub_id");
  EXPECT_EQ(definition->outputs[0].lifetime, "session");
}

}  // namespace llm_edgeflow
