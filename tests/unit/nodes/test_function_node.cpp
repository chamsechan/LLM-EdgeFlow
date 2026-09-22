#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/alg_context.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "engine/model_interface.h"
#include "nodes/authoring.h"
#include "nodes/node_config_parser.h"
#include "nodes/node_error_codes.h"
#include "tests/support/node_harness.h"
#include "tests/support/node_process_pause.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/registry_test_access.h"
#include "tests/support/scoped_allocation_failure.h"

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

struct ControlledMapParams {
  std::string prefix;
  std::string suffix;
  int multiplier = 1;
};

inline constexpr int kCmdReplaceMap = 3001;
inline constexpr int kCmdPatchMap = 3002;

inline std::string ControlledMapFn(const std::string& in,
                                   const ControlledMapParams& p) {
  std::string res = p.prefix;
  for (int i = 0; i < p.multiplier; ++i) {
    res += in;
  }
  return res + p.suffix;
}

inline auto ControlledMapSpec() {
  return MakeMapSpec(
             Input<TextBatch>("input"), Output<TextBatch>("output"),
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
          PatchFields(kCmdPatchMap, "patch_map",
                      {"prefix", "suffix", "multiplier"}),
      });
}
REGISTER_FUNCTION_NODE(ControlledMapNode, ControlledMapSpec());

// ---------------------------------------------------------------------------
// Non-Copyable Params (holding unique_ptr) without Controls (Problem 1)
// ---------------------------------------------------------------------------
struct NonCopyableMapParams {
  std::string prefix;
  std::unique_ptr<int> extra_counter;
};

inline auto NonCopyableMapSpec() {
  return MakeMapSpec(
      Input<TextBatch>("input"), Output<TextBatch>("output"),
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
REGISTER_FUNCTION_NODE(NonCopyableMapNode, NonCopyableMapSpec());

struct NonCopyableBatchInputs {
  const TextBatch* texts = nullptr;
};

struct NonCopyableBatchParams {
  std::string tag;
  std::unique_ptr<int> extra_val;
};

inline auto NonCopyableBatchSpec() {
  return MakeBatchSpec(
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
REGISTER_FUNCTION_NODE(NonCopyableBatchNode, NonCopyableBatchSpec());

// ---------------------------------------------------------------------------
// Strict Unplanned Fact Checking Node (Problem 2)
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
  return MakeBatchSpec(
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
REGISTER_FUNCTION_NODE(ControlledBatchNode, ControlledBatchSpec());

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
  return MakeBatchSpec(
      InputsOf<ImageSummaryInputs>{
          Required("images", &ImageSummaryInputs::images)},
      OutputsOf<ImageSummaryOutputs>{
          Produced("names", &ImageSummaryOutputs::names, "images"),
          Produced("lengths", &ImageSummaryOutputs::lengths, "images")},
      Parameters<ImageSummaryParams>{
          Field("fault", &ImageSummaryParams::fault).Default("")},
      [](const ImageSummaryInputs& inputs, const ImageSummaryParams& params,
         const NoModels&) -> NodeResult<ImageSummaryOutputs> {
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
REGISTER_FUNCTION_NODE(ImageSummaryAuthorNode, ImageSummarySpec());

struct SourceInputs {};
auto SourceSpec() {
  return MakeBatchSpec(
      InputsOf<SourceInputs>{},
      ProducedBatch<TextBatch>("chunks", {"1:N", "generate_sub_id", "session"}),
      [](const SourceInputs&, const NoParameters&,
         const NoModels&) -> NodeResult<TextBatch> {
        return TextBatch{{41, 0, "first"}, {41, 1, "second"}, {41, 2, "third"}};
      });
}
REGISTER_FUNCTION_NODE(GeneratedSourceAuthorNode, SourceSpec());

constexpr int kComplexStateCommand = 4987;
auto ComplexStateControlSpec() {
  const nlohmann::json schema = {
      {"type", "object"},
      {"required", {"nested"}},
      {"additionalProperties", false},
      {"properties",
       {{"nested",
         {{"type", "object"},
          {"required", {"prefix"}},
          {"additionalProperties", false},
          {"properties", {{"prefix", {{"type", "string"}}}}}}}}}};
  return MakeBatchSpec(
             InputsOf<ComplexInputs>{Required("input", &ComplexInputs::input)},
             PreservedOutput<TextBatch>("output", "input"),
             Parameters<CleanParams>{
                 Field("prefix", &CleanParams::prefix).Default("old:")},
             [](const ComplexInputs& input, const CleanParams& params,
                const NoModels&) -> NodeResult<TextBatch> {
               return MapPayloads(*input.input, [&](const std::string& value) {
                 return params.prefix + value;
               });
             })
      .WithControl(
          ControlCommandDefinition(kComplexStateCommand, "replace_nested_state",
                                   "Replace compiled prefix", schema, true),
          [](const CleanParams& current, const nlohmann::json& payload,
             const BindingFacts&) -> NodeResult<CleanParams> {
            auto next = current;
            next.prefix = payload.at("nested").at("prefix").get<std::string>();
            if (next.prefix == "REJECT") {
              return NodeResult<CleanParams>::Failure(
                  NodeErrorKind::kBusinessError, "Rejected compiled prefix",
                  -9877);
            }
            return next;
          });
}
REGISTER_FUNCTION_NODE(ComplexStateControlAuthorNode,
                       ComplexStateControlSpec());

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

TEST(FunctionNodeTest, MissingValidatedPlanFailsInitialization) {
  auto node = NodeRegistry::Instance().Create("AnswerBatchNode");
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
  EXPECT_NE(result.diagnostic().find("unknown_field"), std::string::npos);
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

  init_ctx_map_unplanned.session_ctx = &session_ctx;
  init_ctx_map_unplanned.diagnostic = &map_init_diag;
  init_ctx_map_unplanned.plan = nullptr;
  EXPECT_FALSE(map_unplanned->Init(init_ctx_map_unplanned));
  EXPECT_NE(map_init_diag.find("ValidatedNodePlan"), std::string::npos);

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

TEST(FunctionNodeTest, PlannedPortBindingsPreserveAuthorDiagnosticsAndOrder) {
  struct Case {
    const char* node;
    const char* port;
    int fault;  // 0: absent, 1: empty key and wrong type, 2: wrong type, 3:
                // valid.
    const char* expected;
  };
  const Case cases[] = {
      {"BindingMapNode", "input", 3, ""},
      {"BindingTestNode", "texts", 3, ""},
      {"BindingMapNode", "input", 0,
       "Required input port 'input' has no binding in plan"},
      {"BindingMapNode", "input", 1,
       "Required input port 'input' has no binding in plan"},
      {"BindingMapNode", "input", 2,
       "Input port type mismatch for 'input' (expected: TextBatch, bound: "
       "integer)"},
      {"BindingMapNode", "output", 0,
       "Output port 'output' has no binding in plan"},
      {"BindingMapNode", "output", 1,
       "Output port 'output' has no binding in plan"},
      {"BindingMapNode", "output", 2,
       "Output port type mismatch for 'output' (expected: TextBatch, bound: "
       "integer)"},
      {"BindingTestNode", "texts", 0,
       "Required input port 'texts' is unbound in plan"},
      {"BindingTestNode", "texts", 1,
       "Required input port 'texts' is unbound in plan"},
      {"BindingTestNode", "texts", 2, "Input port type mismatch for 'texts'"},
      {"BindingTestNode", "mask", 0, ""},
      {"BindingTestNode", "mask", 1, ""},
      {"BindingTestNode", "mask", 2, "Input port type mismatch for 'mask'"},
      {"BindingTestNode", "output", 0,
       "Output port 'output' has no binding in plan"},
      {"BindingTestNode", "output", 1,
       "Output port 'output' has no binding in plan"},
      {"BindingTestNode", "output", 2,
       "Output port type mismatch for 'output'"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.node);
    SCOPED_TRACE(test.port);
    SCOPED_TRACE(test.fault);
    const bool map = std::string(test.node) == "BindingMapNode";
    ValidatedNodePlan plan;
    plan.normalized_config = map ? nlohmann::json{{"require_extra", false}}
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
    // An input error must win even when the later output is invalid too.
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
      ASSERT_NE(output, nullptr);
      ASSERT_EQ(output->size(), 1u);
      EXPECT_EQ(output->at(0).data, "mapped");
      EXPECT_FALSE(context.Has("output"));
    }
  }
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
    NodeInitContext init{&plan, &session};
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

// ---------------------------------------------------------------------------
// RFC-0054 Tests: ConfigurationSnapshot & Direct Concurrency (Section 7.1)
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
  // Verifies RFC-0054 Section 7.1:
  // writer A updates prefix, writer B updates suffix.
  // Both successful updates are preserved; B cannot submit based on stale pre-A
  // state.
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

  // Writer A holds the writer lock while B attempts to run
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

  // Writer B attempts to update suffix while A is in Update callback
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
  // A owns the transaction before B is launched. No timing assumption is
  // needed: B must observe A's published value whenever it acquires the lock.

  // Release A so it publishes its update
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
  // Verifies RFC-0054 Section 7.1:
  // Writer fails during validation / candidate generation -> no new snapshot
  // published; old state remains active and intact.
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

  // Also test exception in candidate building
  auto res_ex = snapshot.Update([](const State&) -> NodeResult<State> {
    throw std::runtime_error("candidate throw");
  });
  EXPECT_EQ(res_ex.status, NodeControlStatus::kFailed);
  EXPECT_EQ(snapshot.Read()->val, "original");
}

TEST(ConfigurationSnapshotTest, ReaderHoldsOldSnapshotWhileWriterPublishes) {
  // Verifies RFC-0054 Section 7.1:
  // Reader holding old snapshot is isolated from concurrent writer publication;
  // old reader completes safely with old version; subsequent reader sees new
  // version.
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
// Functional Spec WithControls & NodeHarness Tests
// ---------------------------------------------------------------------------

TEST(FunctionNodeTest, FunctionalMapSpecWithControlsReplaceAndPatch) {
  NodeHarness harness("ControlledMapNode");
  harness.Config(
      {{"prefix", "init_p:"}, {"suffix", ":init_s"}, {"multiplier", 1}});
  harness.TextInput("input", {"payload"});

  auto res1 = harness.Run();
  ASSERT_TRUE(res1.ok()) << res1.diagnostic();
  EXPECT_EQ(res1.TextValues("output"),
            (std::vector<std::string>{"init_p:payload:init_s"}));

  // 1. ReplaceFields missing multiplier -> rejected
  auto bad_replace = harness.Control(kCmdReplaceMap, R"({"prefix":"new_p:"})");
  EXPECT_EQ(bad_replace.status, NodeControlStatus::kFailed);
  EXPECT_NE(bad_replace.message.find("multiplier"), std::string::npos);

  // State preserved
  auto res2 = harness.Run();
  ASSERT_TRUE(res2.ok());
  EXPECT_EQ(res2.TextValues("output"),
            (std::vector<std::string>{"init_p:payload:init_s"}));

  // 2. ReplaceFields with all declared fields -> handled
  auto good_replace =
      harness.Control(kCmdReplaceMap, R"({"prefix":"rep_p:","multiplier":2})");
  EXPECT_EQ(good_replace.status, NodeControlStatus::kHandled);

  // Undeclared suffix remains ":init_s", prefix and multiplier updated
  auto res3 = harness.Run();
  ASSERT_TRUE(res3.ok());
  EXPECT_EQ(res3.TextValues("output"),
            (std::vector<std::string>{"rep_p:payloadpayload:init_s"}));

  // 3. PatchFields with subset of fields -> handled
  auto good_patch = harness.Control(kCmdPatchMap, R"({"suffix":":patch_s"})");
  EXPECT_EQ(good_patch.status, NodeControlStatus::kHandled);

  auto res4 = harness.Run();
  ASSERT_TRUE(res4.ok());
  EXPECT_EQ(res4.TextValues("output"),
            (std::vector<std::string>{"rep_p:payloadpayload:patch_s"}));

  // 4. PatchFields with empty object -> rejected
  auto empty_patch = harness.Control(kCmdPatchMap, R"({})");
  EXPECT_EQ(empty_patch.status, NodeControlStatus::kFailed);

  // 5. Semantic validator failure -> rejected and state rolled back
  auto invalid_prefix =
      harness.Control(kCmdPatchMap, R"({"prefix":"INVALID"})");
  EXPECT_EQ(invalid_prefix.status, NodeControlStatus::kFailed);
  EXPECT_NE(invalid_prefix.message.find("Invalid prefix disallowed"),
            std::string::npos);

  auto res5 = harness.Run();
  ASSERT_TRUE(res5.ok());
  EXPECT_EQ(res5.TextValues("output"),
            (std::vector<std::string>{"rep_p:payloadpayload:patch_s"}));

  // 6. Unknown command -> unsupported
  auto unk = harness.Control(9999, R"({})");
  EXPECT_EQ(unk.status, NodeControlStatus::kUnsupported);
}

TEST(FunctionNodeTest, FunctionalBatchSpecWithControlsAndValidation) {
  NodeHarness harness("ControlledBatchNode");
  harness.Config({{"header", "H:"}, {"uppercase", false}});
  harness.TextInput("texts", {"abc", "def"});

  auto res1 = harness.Run();
  ASSERT_TRUE(res1.ok()) << res1.diagnostic();
  EXPECT_EQ(res1.TextValues("output"),
            (std::vector<std::string>{"H:abc", "H:def"}));

  // Replace update with uppercase = true
  auto ctrl1 =
      harness.Control(kCmdReplaceBatch, R"({"header":"G:","uppercase":true})");
  EXPECT_EQ(ctrl1.status, NodeControlStatus::kHandled);

  auto res2 = harness.Run();
  ASSERT_TRUE(res2.ok());
  EXPECT_EQ(res2.TextValues("output"),
            (std::vector<std::string>{"G:ABC", "G:DEF"}));

  // Semantic rejection
  auto ctrl2 = harness.Control(kCmdReplaceBatch,
                               R"({"header":"REJECT","uppercase":true})");
  EXPECT_EQ(ctrl2.status, NodeControlStatus::kFailed);
  EXPECT_NE(ctrl2.message.find("Rejected header"), std::string::npos);

  // Preserved on failure
  auto res3 = harness.Run();
  ASSERT_TRUE(res3.ok());
  EXPECT_EQ(res3.TextValues("output"),
            (std::vector<std::string>{"G:ABC", "G:DEF"}));
}

TEST(FunctionNodeTest, SnapshotPauseTimeoutFailsProcess) {
  NodeHarness harness("ControlledMapNode");
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
  NodeHarness harness("ControlledMapNode");
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
    // The first allocation is outputs.reserve, after AuthorNode has acquired
    // its parameter snapshot. The callback is confined to this reader thread.
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

TEST(FunctionNodeTest, WholeBatchProcessConsistencyDuringControlForBatchSpec) {
  NodeHarness harness("ControlledBatchNode");
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
    // ControlledBatchFn reserves output after AuthorNode acquires its snapshot.
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
  // Verifies MapSpec with non-copyable ParamsT (containing unique_ptr)
  NodeHarness map_harness("NonCopyableMapNode");
  map_harness.Config({{"prefix", "map_nc:"}});
  map_harness.TextInput("input", {"hello", "world"});
  auto map_res = map_harness.Run();
  ASSERT_TRUE(map_res.ok()) << map_res.diagnostic();
  EXPECT_EQ(map_res.TextValues("output"),
            (std::vector<std::string>{"map_nc:hello_100", "map_nc:world_100"}));

  auto ctrl_map = map_harness.Control(1001, R"({})");
  EXPECT_EQ(ctrl_map.status, NodeControlStatus::kUnsupported);

  // Verifies BatchSpec with non-copyable ParamsT (containing unique_ptr)
  NodeHarness batch_harness("NonCopyableBatchNode");
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
  NodeHarness harness("UpperMapNode");
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

  // 1. Invalid command ID (<= 0)
  EXPECT_THROW(ValidateControlCommands({ReplaceFields(0, "set_text", {"text"})},
                                       make_params()),
               std::invalid_argument);

  // 2. Empty command name
  EXPECT_THROW(ValidateControlCommands({ReplaceFields(1001, "", {"text"})},
                                       make_params()),
               std::invalid_argument);

  // 3. Duplicate command ID
  EXPECT_THROW(
      ValidateControlCommands({ReplaceFields(1001, "cmd_a", {"text"}),
                               ReplaceFields(1001, "cmd_b", {"count"})},
                              make_params()),
      std::invalid_argument);

  // 4. Duplicate command name
  EXPECT_THROW(
      ValidateControlCommands({ReplaceFields(1001, "same_name", {"text"}),
                               ReplaceFields(1002, "same_name", {"count"})},
                              make_params()),
      std::invalid_argument);

  // 5. Empty field names
  EXPECT_THROW(
      ValidateControlCommands({ReplaceFields(1001, "cmd", {})}, make_params()),
      std::invalid_argument);

  // 6. Duplicate field name in same command
  EXPECT_THROW(
      ValidateControlCommands({ReplaceFields(1001, "cmd", {"text", "text"})},
                              make_params()),
      std::invalid_argument);

  // 7. Unbound field name
  EXPECT_THROW(
      ValidateControlCommands({ReplaceFields(1001, "cmd", {"non_existent"})},
                              make_params()),
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

TEST(FunctionNodeTest, MixedControlDeclarationsRejectDuplicateIdInEitherOrder) {
  auto make_spec = [] {
    return MakeBatchSpec(
        InputsOf<ComplexInputs>{Required("input", &ComplexInputs::input)},
        PreservedOutput<TextBatch>("output", "input"), CleanConfig(),
        [](const ComplexInputs& inputs, const CleanParams&,
           const NoModels&) -> NodeResult<TextBatch> { return *inputs.input; });
  };
  auto update = [](const CleanParams& current, const nlohmann::json&,
                   const BindingFacts&) -> NodeResult<CleanParams> {
    return current;
  };
  const ControlCommandDefinition custom(4988, "custom_prefix");
  EXPECT_THROW(
      make_spec()
          .WithControl(custom, update)
          .WithControls({PatchFields(4988, "patch_prefix", {"prefix"})}),
      std::invalid_argument);
  EXPECT_THROW(
      make_spec()
          .WithControls({PatchFields(4988, "patch_prefix", {"prefix"})})
          .WithControl(custom, update),
      std::invalid_argument);
}

TEST(FunctionNodeTest, WithParserWithControlsRequiresExplicitPrepare) {
  struct DummyParams {
    std::string text;
  };
  NodeConfigParser<DummyParams> parser(
      {ConfigFieldDefinition{"nested", ConfigValueKind::kObject, true}},
      [](const nlohmann::json& c, DummyParams* p, std::string*) {
        if (c.contains("nested") && c["nested"].contains("text")) {
          p->text = c["nested"]["text"].get<std::string>();
        }
        return true;
      });
  auto params =
      Parameters<DummyParams>({
                                  Field("text", &DummyParams::text).Default(""),
                              })
          .WithParser(std::move(parser));

  // HasParser() == true, commands not empty, HasPrepare() == false -> throws
  EXPECT_THROW(ValidateControlCommands(
                   {ReplaceFields(1001, "set_text", {"text"})}, params),
               std::invalid_argument);

  // Adding Prepare allows validation to pass
  params.Prepare(
      [](DummyParams*, const BindingFacts&, std::string*) { return true; });
  EXPECT_NO_THROW(ValidateControlCommands(
      {ReplaceFields(1001, "set_text", {"text"})}, params));
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
};

inline auto BindingFactsProbeSpec() {
  return MakeMapSpec(
      Input<TextBatch>("input"), Output<TextBatch>("output"),
      Parameters<BindingFactsProbeParams>(
          {
              Field("mode", &BindingFactsProbeParams::mode).Default("base"),
          })
          .Prepare([](BindingFactsProbeParams* p, const BindingFacts& facts,
                      std::string*) {
            p->plan_seen = facts.has_bindings;
            p->input_connected = facts.IsConnected("input");
            return true;
          }),
      [](const std::string& in, const BindingFactsProbeParams& p) {
        return std::string(p.plan_seen ? "PLAN:" : "NO_PLAN:") +
               (p.input_connected ? "CONN:" : "DISCONN:") + in;
      });
}
REGISTER_FUNCTION_NODE(BindingFactsProbeNode, BindingFactsProbeSpec());

TEST(FunctionNodeTest, AuthorNodeInitPassesRealBindingFactsToPrepare) {
  // Test planned execution: plan_seen must be true, input must be connected
  NodeHarness harness_planned("BindingFactsProbeNode");
  harness_planned.TextInput("input", {"hello"});
  auto res_planned = harness_planned.Run();
  ASSERT_TRUE(res_planned.ok()) << res_planned.diagnostic();
  EXPECT_EQ(res_planned.TextValues("output"),
            (std::vector<std::string>{"PLAN:CONN:hello"}));
}

TEST(FunctionNodeTest, RapidInterleavedControlsAndConcurrentProcesses) {
  NodeHarness harness("ControlledMapNode");
  harness.Config({{"prefix", "p0:"}, {"suffix", ":s0"}, {"multiplier", 1}});
  ASSERT_TRUE(harness.EnsureInitialized());
  auto* node = harness.GetNode();
  ASSERT_NE(node, nullptr);

  std::promise<void> start_signal;
  const auto start = start_signal.get_future().share();
  std::atomic<int> successful_processes{0};

  // Writer thread 1: rapid ReplaceFields
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

  // Writer thread 2: rapid PatchFields
  std::thread writer2([&]() {
    start.wait();
    for (int i = 1; i <= 30; ++i) {
      std::string payload = "{\"suffix\":\":s" + std::to_string(i) + "\"}";
      auto res = node->Control(kCmdPatchMap, payload);
      EXPECT_EQ(res.status, NodeControlStatus::kHandled);
      std::this_thread::yield();
    }
  });

  // Multiple reader threads: concurrent Process calls with multi-item batches
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

        // Verify intra-batch snapshot consistency:
        // Item format: prefix + (multiplier * "payload_i") + suffix
        // Extract prefix, suffix, multiplier from item 0:
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
        // Every other item in this batch must match the exact same snapshot
        // parameters:
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

  // Verify node remains in a coherent final state
  AlgContext final_ctx;
  final_ctx.Publish("bk_in_input", TextBatch{{999, 0, "final"}});
  ASSERT_EQ(node->Process(&final_ctx), 0);
  const auto* final_out = final_ctx.Read<TextBatch>("bk_out_output");
  ASSERT_NE(final_out, nullptr);
  ASSERT_EQ(final_out->size(), 1u);
  EXPECT_EQ(final_out->at(0).data, "p30:final:s30");
}

}  // namespace llm_edgeflow

namespace llm_edgeflow {

TEST(FunctionNodeTest, ImageInputPublishesMultipleTypedOutputsWithProvenance) {
  NodeHarness harness("ImageSummaryAuthorNode");
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
    NodeHarness harness("ImageSummaryAuthorNode");
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
  plan.normalized_config = {{"fault", ""}};
  plan.ports = {{"images", "images", "ImageRefBatch", "1:1", "preserve",
                 "request", PortDirection::kInput},
                {"names", "names", "TextBatch", "1:1", "preserve", "request",
                 PortDirection::kOutput},
                {"lengths", "lengths", "TextBatch", "1:1", "preserve",
                 "request", PortDirection::kOutput}};
  auto node = NodeRegistry::Instance().Create("ImageSummaryAuthorNode");
  ASSERT_NE(node, nullptr);
  SessionContext session;
  std::string diagnostic;
  EXPECT_FALSE(node->Init({&plan, &session, &diagnostic}));
  EXPECT_NE(diagnostic.find("lengths"), std::string::npos);
  EXPECT_NE(diagnostic.find("type mismatch"), std::string::npos);
}

TEST(FunctionNodeTest, SourceWithoutInputsProducesDeclaredVariableCardinality) {
  NodeHarness harness("GeneratedSourceAuthorNode");
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
  const auto definition =
      PipelineCatalog::FindNode("GeneratedSourceAuthorNode");
  ASSERT_TRUE(definition.has_value());
  EXPECT_TRUE(definition->inputs.empty());
  ASSERT_EQ(definition->outputs.size(), 1u);
  EXPECT_EQ(definition->outputs[0].cardinality, "1:N");
  EXPECT_EQ(definition->outputs[0].provenance_policy, "generate_sub_id");
  EXPECT_EQ(definition->outputs[0].lifetime, "session");
}

TEST(FunctionNodeTest,
     ComplexControlRejectsInvalidPayloadAndCandidateWithoutLosingState) {
  NodeHarness harness("ComplexStateControlAuthorNode");
  harness.TextInput("input", {"one", "two"});
  ASSERT_TRUE(harness.Run().ok());
  auto accepted =
      harness.Control(kComplexStateCommand, R"({"nested":{"prefix":"new:"}})");
  ASSERT_EQ(accepted.status, NodeControlStatus::kHandled) << accepted.message;
  for (const std::string payload :
       {"{", R"({"nested":{"prefix":3}})",
        R"({"nested":{"prefix":"wrong:"},"extra":true})",
        R"({"nested":{"prefix":"REJECT"}})"}) {
    SCOPED_TRACE(payload);
    auto rejected = harness.Control(kComplexStateCommand, payload);
    EXPECT_EQ(rejected.status, NodeControlStatus::kFailed);
    if (payload.find("REJECT") != std::string::npos) {
      EXPECT_NE(rejected.message.find("Rejected compiled prefix"),
                std::string::npos);
    }
    auto result = harness.Run();
    ASSERT_TRUE(result.ok()) << result.diagnostic();
    EXPECT_EQ(result.TextValues("output"),
              (std::vector<std::string>{"new:one", "new:two"}));
  }
}

}  // namespace llm_edgeflow
