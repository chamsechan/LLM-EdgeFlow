#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/alg_context.h"
#include "core/session_context.h"
#include "core/validated_node_plan.h"
#include "nodes/authoring.h"

namespace llm_edgeflow {
namespace {

const TextBatch kSample{{41, 6, "red"}, {52, 8, "blue"}};

struct TextInputs {
  const TextBatch* text = nullptr;
  const TextBatch* extra = nullptr;
  const TextBatch* fallback = nullptr;
};

ResolvedPortBinding TextPort(std::string name, std::string key,
                             PortDirection direction = PortDirection::kInput,
                             std::string lifetime = "request") {
  return {std::move(name), std::move(key),      "TextBatch", "1:1",
          "preserve",      std::move(lifetime), direction};
}

ValidatedNodePlan TextPlan(nlohmann::json params = nlohmann::json::object()) {
  // 这些是 Init 使用的已校验信息；计划的生成由 PipelineValidator 自己的
  // 契约测试覆盖。
  ValidatedNodePlan plan;
  plan.normalized_params = std::move(params);
  plan.ports = {TextPort("text", "request.text"),
                TextPort("answer", "result.answer", PortDirection::kOutput)};
  return plan;
}

TEST(AuthorNodeTest, OutputDeclarationsRejectAliasingAndInvalidAnchors) {
  struct Outputs {
    TextBatch first;
    TextBatch second;
  };
  EXPECT_THROW((OutputsOf<Outputs>{Produced("same", &Outputs::first, "text"),
                                   Produced("same", &Outputs::second, "text")}),
               std::invalid_argument);
  EXPECT_THROW((OutputsOf<Outputs>{Produced("one", &Outputs::first, "text"),
                                   Produced("two", &Outputs::first, "text")}),
               std::invalid_argument);
  EXPECT_THROW(Produced("one", &Outputs::first, ""), std::invalid_argument);
  auto inputs = InputsOf<TextInputs>({Required("text", &TextInputs::text)});
  auto output = OutputsOf<Outputs>{Produced("one", &Outputs::first, "missing")};
  auto run = [](const TextInputs&) { return NodeResult<Outputs>::Success({}); };
  EXPECT_THROW(MakeNodeSpec(inputs, output, run), std::invalid_argument);
}

TEST(AuthorNodeTest, PopulationDistinguishesConnectionsFromRequestValues) {
  enum class Value { kMissing, kText, kEmpty, kWrongType };
  struct Case {
    const char* name;
    Value required;
    bool connect_extra;
    Value extra;
    bool connect_fallback;
    Value fallback;
    bool succeeds;
    const char* seen_extra;
    const char* seen_fallback;
    const char* error;
  };
  const std::vector<Case> cases{
      {"unconnected optional ignores same logical key", Value::kText, false,
       Value::kText, false, Value::kText, true, "absent", "absent", ""},
      {"missing required value", Value::kMissing, false, Value::kMissing, false,
       Value::kMissing, false, "", "", "Missing required input"},
      {"wrong required type", Value::kWrongType, false, Value::kMissing, false,
       Value::kMissing, false, "", "", "type mismatch"},
      {"connected Optional needs a value", Value::kText, true, Value::kMissing,
       false, Value::kMissing, false, "", "", "Missing required input"},
      {"connected Optional receives value", Value::kText, true, Value::kText,
       false, Value::kMissing, true, "red", "absent", ""},
      {"OptionalValue permits missing value", Value::kText, false,
       Value::kMissing, true, Value::kMissing, true, "absent", "absent", ""},
      {"OptionalValue receives value", Value::kText, false, Value::kMissing,
       true, Value::kText, true, "absent", "red", ""},
      {"OptionalValue rejects wrong type", Value::kText, false, Value::kMissing,
       true, Value::kWrongType, false, "", "", "type mismatch"},
      {"present empty required value still runs", Value::kEmpty, false,
       Value::kMissing, false, Value::kMissing, true, "absent", "absent", ""},
  };
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    SessionContext session;
    int calls = 0;
    std::string extra, fallback;
    auto spec = MakeNodeSpec(
        InputsOf<TextInputs>(
            {Required("text", &TextInputs::text),
             Optional("extra", &TextInputs::extra),
             OptionalValue("fallback", &TextInputs::fallback)}),
        PreservedOutput<TextBatch>("answer", "text"),
        [&](const TextInputs& input) {
          ++calls;
          extra = input.extra ? input.extra->at(0).data : "absent";
          fallback = input.fallback ? input.fallback->at(0).data : "absent";
          return NodeResult<TextBatch>::Success(*input.text);
        });
    AuthorNode<decltype(spec)> node("population", std::move(spec));
    auto plan = TextPlan();
    if (row.connect_extra)
      plan.ports.push_back(TextPort("extra", "request.extra"));
    if (row.connect_fallback)
      plan.ports.push_back(TextPort("fallback", "request.fallback"));
    ASSERT_TRUE(node.Init({&plan, &session, nullptr}));
    AlgContext context;
    auto publish = [&](const char* key, Value value) {
      if (value == Value::kText) context.Publish(key, kSample);
      if (value == Value::kEmpty) context.Publish(key, TextBatch{});
      if (value == Value::kWrongType)
        context.Publish(key, Int32Batch{{41, 6, 7}});
    };
    publish("request.text", row.required);
    publish(row.connect_extra ? "request.extra" : "extra", row.extra);
    publish(row.connect_fallback ? "request.fallback" : "fallback",
            row.fallback);
    EXPECT_EQ(node.Process(&context), row.succeeds ? 0 : error::kMissingInput);
    EXPECT_EQ(calls, row.succeeds ? 1 : 0);
    if (row.succeeds) {
      EXPECT_EQ(extra, row.seen_extra);
      EXPECT_EQ(fallback, row.seen_fallback);
      const auto* output = context.Read<TextBatch>("result.answer");
      ASSERT_NE(output, nullptr);
      EXPECT_EQ(output->size(), row.required == Value::kEmpty ? 0U : 2U);
    } else {
      EXPECT_EQ(context.Read<TextBatch>("result.answer"), nullptr);
      EXPECT_NE(context.GetErrorMessage().find(row.error), std::string::npos);
    }
  }
}

TEST(AuthorNodeTest, InitRejectsAbsentPrerequisitesAndIncompatiblePorts) {
  SessionContext session;
  auto spec =
      MakeNodeSpec(InputsOf<TextInputs>({Required("text", &TextInputs::text)}),
                   PreservedOutput<TextBatch>("answer", "text"),
                   [](const TextInputs& input) {
                     return NodeResult<TextBatch>::Success(*input.text);
                   });
  struct Case {
    const char* name;
    int defect;
    const char* error;
  };
  for (const auto& row :
       std::vector<Case>{{"no plan", 0, "ValidatedNodePlan"},
                         {"no session", 1, "SessionContext"},
                         {"no required binding", 2, "has no binding"},
                         {"input type", 3, "Input port type mismatch"},
                         {"output type", 4, "Output port type mismatch"}}) {
    SCOPED_TRACE(row.name);
    AuthorNode<decltype(spec)> node("init", spec);
    auto plan = TextPlan();
    if (row.defect == 2) plan.ports.erase(plan.ports.begin());
    if (row.defect == 3) plan.ports[0].type_id = "Int32Batch";
    if (row.defect == 4) plan.ports[1].type_id = "Int32Batch";
    std::string diagnostic = "stale";
    EXPECT_FALSE(
        node.Init({row.defect == 0 ? nullptr : &plan,
                   row.defect == 1 ? nullptr : &session, &diagnostic}));
    EXPECT_NE(diagnostic.find(row.error), std::string::npos);
    EXPECT_EQ(diagnostic.find("stale"), std::string::npos);
  }
}

TEST(AuthorNodeTest, AllDeclaredOutputsAreValidatedBeforeAnyPublication) {
  enum class Fault { kNone, kFailure, kExplicitCode, kCount, kRequest, kSubId };
  struct Outputs {
    TextBatch answer;
    TextBatch shadow;
  };
  struct Case {
    const char* name;
    Fault fault;
    bool reference_shadow;
    int code;
  };
  const std::vector<Case> cases{
      {"both outputs valid", Fault::kNone, true, 0},
      {"unreferenced output stays unpublished", Fault::kNone, false, 0},
      {"Run failure", Fault::kFailure, true, error::kBusinessError},
      {"Run preserves cause code", Fault::kExplicitCode, true, -7341},
      {"count", Fault::kCount, true, error::kOutputCountMismatch},
      {"request provenance", Fault::kRequest, true,
       error::kOutputProvenanceMismatch},
      {"sub provenance", Fault::kSubId, true, error::kOutputProvenanceMismatch},
      {"unreferenced output still validated", Fault::kCount, false,
       error::kOutputCountMismatch},
  };
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    SessionContext session;
    auto spec = MakeNodeSpec(
        InputsOf<TextInputs>({Required("text", &TextInputs::text)}),
        OutputsOf<Outputs>({Produced("answer", &Outputs::answer, "text"),
                            Produced("shadow", &Outputs::shadow, "text")}),
        [&](const TextInputs& input) {
          if (row.fault == Fault::kFailure || row.fault == Fault::kExplicitCode)
            return NodeResult<Outputs>::Failure(
                "literal Run failure",
                row.fault == Fault::kExplicitCode ? -7341 : 0);
          Outputs output{*input.text, *input.text};
          if (row.fault == Fault::kCount) output.shadow.pop_back();
          if (row.fault == Fault::kRequest) output.shadow.back().req_id = 53;
          if (row.fault == Fault::kSubId) output.shadow.back().sub_id = 9;
          return NodeResult<Outputs>::Success(std::move(output));
        });
    AuthorNode<decltype(spec)> node("publication", std::move(spec));
    auto plan = TextPlan();
    if (row.reference_shadow)
      plan.ports.push_back(
          TextPort("shadow", "result.shadow", PortDirection::kOutput));
    ASSERT_TRUE(node.Init({&plan, &session, nullptr}));
    AlgContext context;
    context.Publish("request.text", kSample);
    EXPECT_EQ(node.Process(&context), row.code);
    EXPECT_FALSE(context.Has(""));
    EXPECT_FALSE(context.Has("shadow"));
    if (row.code) {
      EXPECT_EQ(context.Read<TextBatch>("result.answer"), nullptr);
      EXPECT_EQ(context.Read<TextBatch>("result.shadow"), nullptr);
      EXPECT_FALSE(context.GetErrorMessage().empty());
    } else {
      const auto* answer = context.Read<TextBatch>("result.answer");
      ASSERT_NE(answer, nullptr);
      EXPECT_EQ(answer->at(1).data, "blue");
      EXPECT_EQ(context.Read<TextBatch>("result.shadow") != nullptr,
                row.reference_shadow);
    }
  }
}

class RuntimeBoundaryProbe : public NodeBase {
 public:
  RuntimeBoundaryProbe() : NodeBase("runtime_boundary") {}
  int init_fault = 0;
  int run_fault = 0;
  int control_fault = 0;

 protected:
  static void Throw(int fault) {
    if (fault == 1) throw std::runtime_error("literal boundary exception");
    if (fault == 2) throw 42;
  }
  bool InitNode(const NodeInitContext&, const nlohmann::json&,
                SessionContext&) override {
    Throw(init_fault);
    return true;
  }
  int ProcessNode(AlgContext&) override {
    Throw(run_fault);
    return 0;
  }
  NodeControlResult ControlNode(int, const std::string&) override {
    Throw(control_fault);
    return NodeControlResult::Handled();
  }
};

TEST(AuthorNodeTest, RuntimeBoundariesContainStandardAndUnknownExceptions) {
  SessionContext session;
  auto plan = TextPlan();
  RuntimeBoundaryProbe node;
  EXPECT_EQ(node.Process(nullptr), error::kInvalidArgument);
  for (int fault : {1, 2}) {
    SCOPED_TRACE(fault);
    std::string diagnostic;
    node.init_fault = fault;
    EXPECT_FALSE(node.Init({&plan, &session, &diagnostic}));
    EXPECT_NE(diagnostic.find(fault == 1 ? "literal boundary exception"
                                         : "Unknown exception"),
              std::string::npos);
    node.init_fault = 0;
    ASSERT_TRUE(node.Init({&plan, &session, nullptr}));
    AlgContext context;
    node.run_fault = fault;
    const int expected =
        fault == 1 ? error::kInternalException : error::kUnknownException;
    EXPECT_EQ(node.Process(&context), expected);
    EXPECT_EQ(context.GetErrorCode(), expected);
    EXPECT_NE(context.GetErrorMessage().find(
                  fault == 1 ? "literal boundary exception" : "unknown"),
              std::string::npos);
    node.control_fault = fault;
    const auto result = node.Control(17, "{}");
    EXPECT_EQ(result.status, NodeControlStatus::kFailed);
    EXPECT_EQ(result.code, expected);
    EXPECT_NE(result.message.find(fault == 1 ? "literal boundary exception"
                                             : "Unknown control"),
              std::string::npos);
  }
}

class RecordingLlm final : public ILlmModel {
 public:
  enum class Reply { kValid, kError, kCount, kRequest, kSubId };
  Reply reply = Reply::kValid;
  int calls = 0;
  TextBatch received;
  GenerateOptions options;
  const std::string& ImplName() const noexcept override {
    static const std::string name = "recording_llm";
    return name;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string name = "llm";
    return name;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  int Generate(const TextBatch& input, const GenerateOptions& call_options,
               TextBatch* output, std::string* diagnostic) noexcept override {
    ++calls;
    received = input;
    options = call_options;
    *output = {{41, 6, "first reply"}, {52, 8, "second reply"}};
    if (reply == Reply::kCount) *output = {{41, 6, "first reply"}};
    if (reply == Reply::kRequest)
      *output = {{41, 6, "first reply"}, {53, 8, "second reply"}};
    if (reply == Reply::kSubId)
      *output = {{41, 6, "first reply"}, {52, 9, "second reply"}};
    if (reply == Reply::kError) {
      *diagnostic = "literal model failure";
      return -771;
    }
    return 0;
  }
};

TEST(ModelCallTest, SharedAlignedInvocationOwnsEmptyErrorAndProvenanceRules) {
  struct Case {
    const char* name;
    bool empty;
    bool bound;
    RecordingLlm::Reply reply;
    int code;
    const char* error;
  };
  const std::vector<Case> cases{
      {"empty bound", true, true, RecordingLlm::Reply::kValid, 0, ""},
      {"empty unbound", true, false, RecordingLlm::Reply::kValid, 0, ""},
      {"unbound nonempty", false, false, RecordingLlm::Reply::kValid,
       error::kModelCallFailed, "model is null"},
      {"model error", false, true, RecordingLlm::Reply::kError, -771,
       "literal model failure"},
      {"count", false, true, RecordingLlm::Reply::kCount,
       error::kOutputCountMismatch, "count mismatch"},
      {"request", false, true, RecordingLlm::Reply::kRequest,
       error::kOutputProvenanceMismatch, "provenance mismatch"},
      {"sub", false, true, RecordingLlm::Reply::kSubId,
       error::kOutputProvenanceMismatch, "provenance mismatch"},
      {"valid", false, true, RecordingLlm::Reply::kValid, 0, ""},
  };
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    auto model = std::make_shared<RecordingLlm>();
    model->reply = row.reply;
    LlmCall call(row.bound ? model : nullptr, "chosen_slot", "chosen_model");
    auto result = call.Generate(row.empty ? TextBatch{} : kSample);
    EXPECT_EQ(model->calls, !row.empty && row.bound ? 1 : 0);
    if (row.code) {
      ASSERT_FALSE(result.ok());
      EXPECT_EQ(result.failure().cause_code, row.code);
      EXPECT_NE(result.failure().message.find(row.error), std::string::npos);
    } else {
      ASSERT_TRUE(result.ok());
      ASSERT_EQ(result.value().size(), row.empty ? 0U : 2U);
      if (!row.empty) {
        EXPECT_EQ(result.value()[0].data, "first reply");
        EXPECT_EQ(result.value()[1].req_id, 52U);
        EXPECT_EQ(result.value()[1].sub_id, 8U);
        EXPECT_EQ(result.value()[1].data, "second reply");
      }
    }
  }
}

TEST(AuthorNodeTest, InitCopiesResolvedKeysParametersFactsAndModelBinding) {
  struct Params {
    std::string prefix;
    int tokens = 0;
  };
  struct Models {
    LlmCall generator;
  };
  SessionContext session;
  auto model = std::make_shared<RecordingLlm>();
  ModelRegistration registration;
  registration.model_name = "resolved_model";
  registration.model = model;
  ASSERT_TRUE(session.GetModelManager().RegisterBatch({registration}));
  int model_validations = 0;
  std::string seen_model, seen_slot, seen_lifetime;
  auto params =
      Parameters<Params>({Field("prefix", &Params::prefix).Required(),
                          Field("tokens", &Params::tokens).Required()})
          .Prepare([&](Params* value, const BindingFacts& facts, std::string*) {
            seen_lifetime = facts.InputLifetime("text");
            value->prefix += "/" + seen_lifetime;
            return true;
          });
  auto spec =
      MakeNodeSpec(InputsOf<TextInputs>({Required("text", &TextInputs::text)}),
                   PreservedOutput<TextBatch>("answer", "text"), params,
                   ModelsOf<Models>({Model("generator", "bind_generator",
                                           &Models::generator)}),
                   [&](const TextInputs& input, const Params& value,
                       const Models& models) {
                     seen_model = models.generator.ModelName();
                     seen_slot = models.generator.SlotName();
                     TextBatch prompts = *input.text;
                     for (auto& item : prompts)
                       item.data = value.prefix + ":" + item.data;
                     GenerateOptions options;
                     options.max_tokens = value.tokens;
                     options.system_prompt = "literal system";
                     return models.generator.Generate(prompts, options);
                   })
          .ValidateModels(
              [&](const Params&, const Models& models, std::string*) {
                ++model_validations;
                return models.generator.IsBound();
              });
  AuthorNode<decltype(spec)> node("owned_init", std::move(spec));
  {
    auto plan = TextPlan({{"prefix", "normalized"},
                          {"tokens", 23},
                          {"bind_generator", "decoy"}});
    plan.node.params = {{"prefix", "raw must be ignored"}, {"tokens", 99}};
    plan.ports[0].lifetime = "session";
    plan.model_bindings.push_back(
        {"generator", "llm", "bind_generator", "resolved_model"});
    std::string diagnostic = "stale diagnostic";
    ASSERT_TRUE(node.Init({&plan, &session, &diagnostic}));
    EXPECT_TRUE(diagnostic.empty());
    plan.normalized_params["prefix"] = "mutated";
    plan.ports.clear();
    plan.model_bindings.clear();
  }
  AlgContext context;
  context.Publish("text", TextBatch{{41, 6, "wrong default key"}});
  context.Publish("request.text", kSample);
  ASSERT_EQ(node.Process(&context), 0);
  EXPECT_EQ(model_validations, 1);
  EXPECT_EQ(seen_lifetime, "session");
  EXPECT_EQ(seen_model, "resolved_model");
  EXPECT_EQ(seen_slot, "generator");
  ASSERT_EQ(model->received.size(), 2U);
  EXPECT_EQ(model->received[0].data, "normalized/session:red");
  EXPECT_EQ(model->received[1].data, "normalized/session:blue");
  EXPECT_EQ(model->options.max_tokens, 23);
  EXPECT_EQ(model->options.system_prompt, "literal system");
  EXPECT_EQ(context.Read<TextBatch>("answer"), nullptr);
  const auto* output = context.Read<TextBatch>("result.answer");
  ASSERT_NE(output, nullptr);
  EXPECT_EQ(output->at(1).data, "second reply");
}

struct PauseGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool released = false;
};
struct ControlledParams {
  std::string prefix, suffix, prepared;
  bool needs_extra = false;
};
struct ControlObservations {
  int prepares = 0, validates = 0, bindings = 0;
  std::vector<std::string> lifetimes;
  std::vector<std::unordered_set<std::string>> connections;
};

auto ControlledSpec(ControlObservations& observed,
                    std::shared_ptr<PauseGate> gate = {}) {
  auto params =
      Parameters<ControlledParams>(
          {Field("prefix", &ControlledParams::prefix).Required(),
           Field("suffix", &ControlledParams::suffix).Required(),
           Field("needs_extra", &ControlledParams::needs_extra).Required()})
          .Prepare([&](ControlledParams* p, const BindingFacts& facts,
                       std::string* error) {
            ++observed.prepares;
            observed.lifetimes.push_back(facts.InputLifetime("text"));
            if (p->prefix == "throw")
              throw std::runtime_error("literal prepare exception");
            if (p->prefix == "prepare-fail") {
              *error = "literal prepare rejection";
              return false;
            }
            p->prepared =
                p->prefix + "/" + p->suffix + ":" + facts.InputLifetime("text");
            return true;
          })
          .Validate([&](const ControlledParams& p, std::string* error) {
            ++observed.validates;
            if (p.prefix != "invalid") return true;
            *error = "literal semantic rejection";
            return false;
          })
          .ValidateBindings([&](const ControlledParams& p,
                                const std::unordered_set<std::string>& inputs,
                                std::string* error) {
            ++observed.bindings;
            observed.connections.push_back(inputs);
            if (!p.needs_extra || inputs.count("extra")) return true;
            *error = "literal binding rejection";
            return false;
          });
  return MakeNodeSpec(
             InputsOf<TextInputs>({Required("text", &TextInputs::text),
                                   Optional("extra", &TextInputs::extra)}),
             PreservedOutput<TextBatch>("answer", "text"), params,
             [gate](const TextInputs& inputs, const ControlledParams& p) {
               TextBatch output;
               for (const auto& item : *inputs.text) {
                 output.emplace_back(item.req_id, item.sub_id,
                                     p.prepared + "|" + item.data);
                 if (gate && output.size() == 1) {
                   std::unique_lock<std::mutex> lock(gate->mutex);
                   gate->entered = true;
                   gate->changed.notify_all();
                   gate->changed.wait(lock, [&] { return gate->released; });
                 }
               }
               return NodeResult<TextBatch>::Success(std::move(output));
             })
      .WithControls(
          {ReplaceFields(17, "replace", {"prefix", "suffix", "needs_extra"})});
}

TEST(AuthorNodeTest,
     ControlsReplaceOnlyProvidedFieldsAndRevalidateOriginalFacts) {
  SessionContext session;
  ControlObservations observed;
  auto spec = ControlledSpec(observed);
  AuthorNode<decltype(spec)> node("controlled", std::move(spec));
  auto plan =
      TextPlan({{"prefix", "old"}, {"suffix", "tail"}, {"needs_extra", false}});
  plan.ports[0].lifetime = "session";
  ASSERT_TRUE(node.Init({&plan, &session, nullptr}));
  plan.ports.clear();
  EXPECT_EQ(node.Control(99, "{}").status, NodeControlStatus::kUnsupported);
  EXPECT_EQ(node.Control(17, R"({"prefix":"new"})").status,
            NodeControlStatus::kHandled);
  {
    AlgContext context;
    context.Publish("request.text", kSample);
    ASSERT_EQ(node.Process(&context), 0);
    const auto* output = context.Read<TextBatch>("result.answer");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 2U);
    EXPECT_EQ(output->at(0).data, "new/tail:session|red");
    EXPECT_EQ(output->at(1).data, "new/tail:session|blue");
  }
  EXPECT_EQ(node.Control(17, R"({"suffix":"final"})").status,
            NodeControlStatus::kHandled);
  const std::vector<std::pair<std::string, std::string>> rejected{
      {"{}", ""},
      {R"({"prefix":"prepare-fail"})", "literal prepare rejection"},
      {R"({"prefix":"invalid"})", "literal semantic rejection"},
      {R"({"needs_extra":true})", "literal binding rejection"},
      {R"({"prefix":"throw"})", "literal prepare exception"}};
  for (const auto& row : rejected) {
    SCOPED_TRACE(row.first);
    const auto control = node.Control(17, row.first);
    EXPECT_EQ(control.status, NodeControlStatus::kFailed);
    // ValidateState 拦截钩子抛出的异常，并报告状态被拒绝。
    EXPECT_EQ(control.code, error::kInvalidArgument);
    EXPECT_FALSE(control.message.empty());
    if (!row.second.empty()) {
      EXPECT_NE(control.message.find(row.second), std::string::npos);
    }
    AlgContext context;
    context.Publish("request.text", kSample);
    ASSERT_EQ(node.Process(&context), 0);
    const auto* output = context.Read<TextBatch>("result.answer");
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->size(), 2U);
    EXPECT_EQ(output->at(0).data, "new/final:session|red");
    EXPECT_EQ(output->at(1).data, "new/final:session|blue");
  }
  EXPECT_EQ(observed.prepares, 7);
  EXPECT_EQ(observed.validates, 5);
  EXPECT_EQ(observed.bindings, 4);
  for (const auto& lifetime : observed.lifetimes)
    EXPECT_EQ(lifetime, "session");
  for (const auto& inputs : observed.connections)
    EXPECT_EQ(inputs, std::unordered_set<std::string>{"text"});
}

TEST(AuthorNodeTest, OneProcessHoldsOneSnapshotForItsWholeBatch) {
  SessionContext session;
  ControlObservations observed;
  auto gate = std::make_shared<PauseGate>();
  auto spec = ControlledSpec(observed, gate);
  AuthorNode<decltype(spec)> node("whole_batch", std::move(spec));
  auto plan =
      TextPlan({{"prefix", "old"}, {"suffix", "tail"}, {"needs_extra", false}});
  plan.ports[0].lifetime = "session";
  ASSERT_TRUE(node.Init({&plan, &session, nullptr}));
  AlgContext first;
  first.Publish("request.text", kSample);
  auto running =
      std::async(std::launch::async, [&] { return node.Process(&first); });
  bool entered;
  {
    std::unique_lock<std::mutex> lock(gate->mutex);
    entered = gate->changed.wait_for(lock, std::chrono::seconds(5),
                                     [&] { return gate->entered; });
  }
  auto controlling = std::async(std::launch::async, [&] {
    return node.Control(17, R"({"prefix":"new"})");
  });
  const bool updated_while_paused =
      controlling.wait_for(std::chrono::seconds(5)) ==
      std::future_status::ready;
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->released = true;
  }
  gate->changed.notify_all();
  const int code = running.get();
  const auto update = controlling.get();
  EXPECT_TRUE(entered);
  EXPECT_TRUE(updated_while_paused);
  EXPECT_EQ(update.status, NodeControlStatus::kHandled);
  ASSERT_EQ(code, 0);
  const auto* old_output = first.Read<TextBatch>("result.answer");
  ASSERT_NE(old_output, nullptr);
  ASSERT_EQ(old_output->size(), 2U);
  EXPECT_EQ(old_output->at(0).data, "old/tail:session|red");
  EXPECT_EQ(old_output->at(1).data, "old/tail:session|blue");
  AlgContext second;
  second.Publish("request.text", kSample);
  ASSERT_EQ(node.Process(&second), 0);
  const auto* new_output = second.Read<TextBatch>("result.answer");
  ASSERT_NE(new_output, nullptr);
  ASSERT_EQ(new_output->size(), 2U);
  EXPECT_EQ(new_output->at(0).data, "new/tail:session|red");
  EXPECT_EQ(new_output->at(1).data, "new/tail:session|blue");
}

TEST(ConfigurationSnapshotTest, UpdateRequiresValueInitialization) {
  ConfigurationSnapshot<int> snapshot;
  EXPECT_EQ(snapshot.Read(), nullptr);
  bool built = false;
  const auto result = snapshot.Update([&](const int& current) {
    built = true;
    return NodeResult<int>::Success(current + 1);
  });
  EXPECT_FALSE(built);
  EXPECT_EQ(result.status, NodeControlStatus::kFailed);
  EXPECT_EQ(result.code, error::kInvalidState);
  EXPECT_EQ(result.message, "ConfigurationSnapshot is not initialized");
  ASSERT_TRUE(snapshot.Initialize(10));
  auto reader = snapshot.Read();
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(*reader, 10);
}

TEST(ConfigurationSnapshotTest,
     RejectedCandidatesPreserveOldReaderAndCurrentState) {
  ConfigurationSnapshot<int> snapshot(10);
  auto reader = snapshot.Read();
  for (int fault : {0, 1, 2}) {
    SCOPED_TRACE(fault);
    const auto result = snapshot.Update([&](const int&) -> NodeResult<int> {
      if (fault == 0)
        return NodeResult<int>::Failure("literal candidate rejection", -742);
      if (fault == 1) throw std::runtime_error("literal candidate exception");
      throw 42;
    });
    EXPECT_EQ(result.status, NodeControlStatus::kFailed);
    EXPECT_EQ(result.code, fault == 0   ? -742
                           : fault == 1 ? error::kInternalException
                                        : error::kUnknownException);
    EXPECT_EQ(result.message,
              fault == 0   ? "literal candidate rejection"
              : fault == 1 ? "literal candidate exception"
                           : "Unknown exception during configuration update");
    EXPECT_EQ(snapshot.Read(), reader);
    EXPECT_EQ(*reader, 10);
  }
  const auto success =
      snapshot.Update([](const int&) { return NodeResult<int>::Success(20); });
  EXPECT_EQ(success.status, NodeControlStatus::kHandled);
  EXPECT_EQ(*snapshot.Read(), 20);
  EXPECT_EQ(*reader, 10);
}

TEST(ConfigurationSnapshotTest,
     ReadersSeeOnlyCommittedCandidatesAndUpdatesUseTheLatestCommit) {
  ConfigurationSnapshot<int> snapshot(10);
  auto old_reader = snapshot.Read();
  PauseGate gate;
  int first_base = -1, second_base = -1;
  auto first = std::async(std::launch::async, [&] {
    return snapshot.Update([&](const int& value) {
      first_base = value;
      std::unique_lock<std::mutex> lock(gate.mutex);
      gate.entered = true;
      gate.changed.notify_all();
      gate.changed.wait(lock, [&] { return gate.released; });
      return NodeResult<int>::Success(value + 1);
    });
  });
  bool entered;
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    entered = gate.changed.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return gate.entered; });
  }
  const auto while_preparing = snapshot.Read();
  const int unpublished = *while_preparing;
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.released = true;
  }
  gate.changed.notify_all();
  const auto first_result = first.get();
  const auto second_result = snapshot.Update([&](const int& value) {
    second_base = value;
    return NodeResult<int>::Success(value + 1);
  });
  EXPECT_TRUE(entered);
  EXPECT_EQ(unpublished, 10);
  EXPECT_EQ(*while_preparing, 10);
  EXPECT_EQ(first_result.status, NodeControlStatus::kHandled);
  EXPECT_EQ(second_result.status, NodeControlStatus::kHandled);
  EXPECT_EQ(first_base, 10);
  EXPECT_EQ(second_base, 11);
  EXPECT_EQ(*snapshot.Read(), 12);
  EXPECT_EQ(*old_reader, 10);
}

TEST(ConfigurationSnapshotTest, ConcurrentUpdatesCommitEveryIncrement) {
  ConfigurationSnapshot<int> snapshot(0);
  std::promise<void> release;
  auto start = release.get_future().share();
  std::atomic<int> rejected{0};
  std::vector<std::future<void>> writers;
  for (int i = 0; i < 8; ++i) {
    writers.push_back(std::async(std::launch::async, [&] {
      start.wait();
      for (int update = 0; update < 40; ++update) {
        auto result = snapshot.Update([](const int& current) {
          return NodeResult<int>::Success(current + 1);
        });
        if (result.status != NodeControlStatus::kHandled) ++rejected;
      }
    }));
  }
  release.set_value();
  for (auto& writer : writers) writer.get();
  EXPECT_EQ(rejected, 0);
  EXPECT_EQ(*snapshot.Read(), 320);
}

}  // namespace
}  // namespace llm_edgeflow
