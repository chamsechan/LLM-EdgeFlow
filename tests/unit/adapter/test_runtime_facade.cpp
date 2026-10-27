#include <gtest/gtest.h>

#include <any>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "adapter/converter_authoring.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/operator/operator_output_pool.h"
#include "adapter/shared_algorithm_runtime.h"
#include "contracts/error_codes.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "tests/support/registry_test_access.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {
namespace {
constexpr auto kText = MakeBlackboardKey<TextBatch>("text");
constexpr auto kResult = MakeBlackboardKey<TextBatch>("result");
constexpr int kProbeCommand = 9817;

struct Trace {
  int initialized = 0;
  int nodes_destroyed = 0;
  int allocated = 0;
  int live = 0;
  int resets = 0;
  int processed = 0;
  int control_calls = 0;
  int fail_allocate_at = -1;
  int execution_code = 0;
  bool fail_init = false;
  bool fail_write = false;
  bool saw_old_context = false;
  RuntimeOptions options;
  std::vector<std::string> observed;
  std::mutex mutex;
  std::condition_variable cv;
  bool pause = false;
  bool entered = false;
  bool released = false;
  void PauseIfRequested() {
    std::unique_lock<std::mutex> lock(mutex);
    if (!pause) return;
    // 只有第一个操作会暂停；若未串行化，第二个操作就能完成并使阻塞断言
    // 失败。
    pause = false;
    entered = true;
    cv.notify_all();
    cv.wait(lock, [this] { return released; });
  }
  bool AwaitEntry() {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(5),
                       [this] { return entered; });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    cv.notify_all();
  }
};

class RecordingNode final : public INode {
 public:
  explicit RecordingNode(std::shared_ptr<Trace> trace)
      : trace_(std::move(trace)) {}
  ~RecordingNode() override { ++trace_->nodes_destroyed; }
  bool Init(const NodeInitContext& context) override {
    ++trace_->initialized;
    trace_->options = context.session_ctx->GetRuntimeOptions();
    if (trace_->fail_init) return context.Fail("node initialization rejected");
    input_ = context.plan->FindPort("text")->blackboard_key;
    output_ =
        context.plan->FindPort("text", PortDirection::kOutput)->blackboard_key;
    return true;
  }
  int Process(AlgContext* context) override {
    ++trace_->processed;
    trace_->saw_old_context |= context->Has("probe.private_marker");
    context->Publish("probe.private_marker", 1);
    const auto& input = *context->Read<TextBatch>(input_);
    trace_->observed.clear();
    for (const auto& row : input) trace_->observed.push_back(row.data);
    trace_->PauseIfRequested();
    if (trace_->execution_code) {
      context->SetError(trace_->execution_code, "node execution rejected");
      return trace_->execution_code;
    }
    return context->Publish(output_, input) ? 0 : -72;
  }
  NodeControlResult Control(int, const std::string&) override {
    ++trace_->control_calls;
    trace_->PauseIfRequested();
    return NodeControlResult::Handled();
  }
  const std::string& Name() const override { return name_; }

 private:
  std::shared_ptr<Trace> trace_;
  const std::string name_ = "runtime_facade_probe";
  std::string input_, output_;
};

struct InputCarrier {
  std::string text;
};
struct OutputCarrier {
  explicit OutputCarrier(std::shared_ptr<Trace> trace, size_t capacity)
      : trace(std::move(trace)) {
    text.reserve(capacity);
    ++this->trace->live;
  }
  ~OutputCarrier() { --trace->live; }
  std::shared_ptr<Trace> trace;
  std::string text;
  int32_t service = 0;
};
struct OutputParameters {
  int64_t result_json_max_bytes = 64;
  bool misreport = false;
};

int EncodeProbe(AlgContext* context, const OutputEncodeOptions& options,
                ExternalOutputBatchView* destination, size_t* written,
                AdapterStatus* status) {
  const int code = EncodeResultRows<JsonOutputValue>(
      context, options, destination, written, status, "json_out", kResult,
      [](const std::string& text, JsonOutputValue* value) {
        value->result_json = text;
        return AdapterStatus::Ok();
      });
  if (code == 0 && options.Params<OutputParameters>().misreport)
    *written = destination->count;
  return code;
}

class ScopedConverters {
 public:
  ScopedConverters()
      : inputs_(IoConverterRegistry::Instance().AllInputConverters()),
        outputs_(IoConverterRegistry::Instance().AllOutputConverters()) {}
  ~ScopedConverters() {
    auto& registry = IoConverterRegistry::Instance();
    test_support::RegistryTestAccess::ResetConverters();
    for (const auto& definition : inputs_)
      registry.RegisterInputConverter(definition);
    for (const auto& definition : outputs_)
      registry.RegisterOutputConverter(definition);
  }

 private:
  std::vector<InputConverterDefinition> inputs_;
  std::vector<OutputConverterDefinition> outputs_;
};

class RuntimeFacadeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    trace = std::make_shared<Trace>();
    NodeDefinition node;
    node.node_type = "runtime_facade_probe";
    node.inputs = {RequiredInputPort(kText)};
    node.outputs = {OutputPort(kText)};
    node.control_commands = {
        {kProbeCommand, "probe", "", {{"type", "object"}}}};
    ASSERT_TRUE(NodeRegistry::Instance().Register(
        node.node_type,
        [state = trace] { return std::make_unique<RecordingNode>(state); },
        node));
    auto original_input =
        OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("entity_in");
    auto original_output =
        OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("json_out");
    ASSERT_TRUE(original_input.has_value());
    ASSERT_TRUE(original_output.has_value());
    input_binding = *original_input;
    input_binding.external_c_type_name = "InputCarrier";
    input_binding.validate_external = [](const void* value, const InputLimits&,
                                         std::string* error) {
      if (static_cast<const InputCarrier*>(value)->text == "decode_fail") {
        *error = "input carrier rejected";
        return error::kInvalidInput;
      }
      return 0;
    };
    SetInputValue<InputCarrier, TextInputValue>(
        &input_binding,
        [](const InputCarrier& value) { return TextInputValue{value.text}; });
    test_support::RegistryTestAccess::SetValueBinding(input_binding);
    output_binding = *original_output;
    output_binding.external_c_type_name = "OutputCarrier";
    SetServiceTypeMember<OutputCarrier>(&output_binding,
                                        &OutputCarrier::service);
    output_binding.services["runtime_facade"] = 9817;
    output_binding.output_layout.compute_block_payload_bytes =
        [](const ResolvedOutputPoolSpec& spec, size_t* bytes, std::string*) {
          *bytes = sizeof(OutputCarrier) + spec.GetCapacity("result_json");
          return true;
        };
    output_binding.allocate_external =
        [state = trace](const ResolvedOutputPoolSpec& spec,
                        OwnedExternalBlock* block, std::string* error) {
          const int index = state->allocated++;
          block->raw_struct = block->Own(std::make_unique<OutputCarrier>(
              state, spec.GetCapacity("result_json")));
          if (index == state->fail_allocate_at) {
            *error = "output allocation rejected";
            return -71;
          }
          return 0;
        };
    output_binding.reset_external =
        [state = trace](void* raw, const ResolvedOutputPoolSpec&) {
          ++state->resets;
          static_cast<OutputCarrier*>(raw)->text.clear();
        };
    output_binding.destroy_external = [](OwnedExternalBlock* block) {
      block->Destroy();
    };
    SetOutputValue<OutputCarrier, JsonOutputValue>(
        &output_binding,
        [state = trace](OutputCarrier& output, const JsonOutputValue& value,
                        const ResolvedOutputPoolSpec& spec) {
          if (state->fail_write && value.result_json == "B")
            return AdapterStatus::InvalidInput("output writer rejected",
                                               "result_json");
          if (value.result_json.size() > spec.GetCapacity("result_json"))
            return AdapterStatus::BufferTooSmall("capacity exceeded",
                                                 "result_json");
          output.text = value.result_json;
          return AdapterStatus::Ok();
        });
    test_support::RegistryTestAccess::SetValueBinding(output_binding);
    OutputConverterDefinition output;
    output.type = "json_out";
    output.name = "runtime_facade";
    output.slot = ExternalOutputSlot<JsonOutputValue>();
    output.slot.required = false;
    output.logical_ports = {RequiredInputPort(kResult)};
    output.params = Parameters<OutputParameters>(
        {MaxBytes("result_json", &OutputParameters::result_json_max_bytes)
             .Default(64),
         Field("misreport", &OutputParameters::misreport).Default(false)});
    output.encode_fn = &EncodeProbe;
    ASSERT_TRUE(
        IoConverterRegistry::Instance().RegisterOutputConverter(output));
    options.output_pool_depth = 2;
    options.device_id = 3;
    options.compute_platform = "CPU";
  }
  void TearDown() override { runtime.reset(); }
  std::unique_ptr<ValidatedIoPlan> Plan(bool misreport = false,
                                        int capacity = 64) {
    const nlohmann::json document = {
        {"pipeline",
         {{{"name", "probe"},
           {"type", "runtime_facade_probe"},
           {"inputs", {{"text", "input.sentence_text"}}}}}},
        {"io",
         {{"input", {{{"type", "entity_in"}, {"name", "entity_extract"}}}},
          {"output",
           {{{"type", "json_out"},
             {"name", "runtime_facade"},
             {"params",
              {{"result_json_max_bytes", capacity}, {"misreport", misreport}}},
             {"inputs", {{"result", "probe.text"}}}}}}}}};
    std::unique_ptr<ValidatedIoPlan> plan;
    std::string error;
    EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(document, "", &plan,
                                                      &error, nullptr, 2),
              0)
        << error;
    return plan;
  }
  void Create(bool misreport = false, int capacity = 64) {
    auto plan = Plan(misreport, capacity);
    ASSERT_NE(plan, nullptr);
    std::string error;
    ASSERT_EQ(SharedAlgorithmRuntime::CreateFromIoPlan(std::move(plan), options,
                                                       &runtime, &error),
              0)
        << error;
  }
  RuntimeInputBatch Input(const std::vector<std::string>& values) const {
    RuntimeInputBatch input;
    input.count = values.size();
    ExternalInputBatchView view;
    view.count = values.size();
    view.binding = &input_binding;
    view.slot = "entity_in";
    view.external_c_type_name = "InputCarrier";
    for (const auto& value : values)
      view.values.push_back(
          std::make_shared<InputCarrier>(InputCarrier{value}));
    input.views.push_back(std::move(view));
    return input;
  }
  RuntimeOutputBatch Output() const { return {{{{0, {}}}, {{0, {}}}}}; }
  test_support::RegistryTestAccess::ScopedNodeState nodes;
  test_support::RegistryTestAccess::ScopedValueTypeState values;
  ScopedConverters converters;
  std::shared_ptr<Trace> trace;
  OperatorValueTypeBinding input_binding, output_binding;
  RuntimeCreateOptions options;
  std::unique_ptr<SharedAlgorithmRuntime> runtime;
};

TEST_F(RuntimeFacadeTest, CreationCommitsOnlyAfterNodesAndPoolsSucceed) {
  trace->fail_init = true;
  auto plan = Plan();
  ASSERT_NE(plan, nullptr);
  std::string error;
  RuntimeFailureStage stage;
  EXPECT_EQ(SharedAlgorithmRuntime::CreateFromIoPlan(std::move(plan), options,
                                                     &runtime, &error, &stage),
            error::kNodeInitFailed);
  EXPECT_EQ(runtime, nullptr);
  EXPECT_EQ(stage, RuntimeFailureStage::kPreparation);
  EXPECT_EQ(trace->allocated, 0);
  EXPECT_EQ(trace->nodes_destroyed, 1);
  trace->fail_init = false;
  trace->fail_allocate_at = 1;
  plan = Plan();
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(SharedAlgorithmRuntime::CreateFromIoPlan(std::move(plan), options,
                                                     &runtime, &error, &stage),
            -71);
  EXPECT_EQ(runtime, nullptr);
  EXPECT_EQ(trace->live, 0);
  EXPECT_EQ(trace->nodes_destroyed, 2);
  EXPECT_NE(error.find("output allocation rejected"), std::string::npos);
}

TEST_F(RuntimeFacadeTest, ProcessUsesFreshContextAndCopiedRuntimeOptions) {
  Create();
  ASSERT_NE(runtime, nullptr);
  options.device_id = 9;
  options.compute_platform = "changed";
  EXPECT_EQ(trace->options.device_id, 3);
  EXPECT_EQ(trace->options.chip_type, "CPU");
  EXPECT_TRUE(trace->options.has_device_id);
  EXPECT_EQ(runtime->ProcessBatchLimit(), 2U);
  for (const auto& texts : {std::vector<std::string>{"A", "B"},
                            std::vector<std::string>{"C", "D"}}) {
    auto output = Output();
    std::string error;
    ASSERT_EQ(runtime->Process(Input(texts), &output, &error), 0) << error;
    ASSERT_NE(output.rows[0][0].value, nullptr);
    ASSERT_NE(output.rows[1][0].value, nullptr);
    EXPECT_EQ(static_cast<OutputCarrier*>(output.rows[0][0].value.get())->text,
              texts[0]);
    EXPECT_EQ(static_cast<OutputCarrier*>(output.rows[1][0].value.get())->text,
              texts[1]);
  }
  EXPECT_FALSE(trace->saw_old_context);
  EXPECT_EQ(trace->observed, (std::vector<std::string>{"C", "D"}));
  EXPECT_EQ(trace->resets, 4);
}

TEST_F(RuntimeFacadeTest,
       InputViewsUseOnlyPlannedCallbacksAndMatchingCarriers) {
  Create();
  ASSERT_NE(runtime, nullptr);
  int caller_callbacks = 0;
  auto caller_binding = input_binding;
  caller_binding.validate_external = [&](const void*, const InputLimits&,
                                         std::string*) {
    ++caller_callbacks;
    return error::kInvalidInput;
  };
  caller_binding.read_value = [&](const void*) -> std::any {
    ++caller_callbacks;
    return TextInputValue{"untrusted"};
  };
  auto input = Input({"A", "B"});
  input.views[0].binding = &caller_binding;
  for (const bool wrong_slot : {true, false}) {
    auto mismatched = input;
    if (wrong_slot)
      mismatched.views[0].slot = "other";
    else
      mismatched.views[0].external_c_type_name = "OtherCarrier";
    auto output = Output();
    std::string error;
    EXPECT_NE(runtime->Process(mismatched, &output, &error), 0);
    EXPECT_NE(error.find("Missing or mismatched input binding"),
              std::string::npos);
    EXPECT_EQ(trace->processed, 0);
    EXPECT_EQ(trace->resets, 0);
    EXPECT_EQ(output.rows[0][0].value, nullptr);
    EXPECT_EQ(output.rows[1][0].value, nullptr);
  }
  auto output = Output();
  std::string error;
  ASSERT_EQ(runtime->Process(input, &output, &error), 0) << error;
  EXPECT_EQ(caller_callbacks, 0);
  EXPECT_EQ(trace->observed, (std::vector<std::string>{"A", "B"}));
}

TEST_F(RuntimeFacadeTest, BatchAndViewBoundsFailBeforeExecutionOrAcquisition) {
  Create();
  ASSERT_NE(runtime, nullptr);
  struct Case {
    size_t count;
    bool omit_view;
    size_t rows;
    int code;
  };
  const std::vector<Case> cases = {{0, false, 0, error::kInvalidInput},
                                   {3, false, 3, error::kInvalidInput},
                                   {2, true, 2, error::kInvalidInput},
                                   {2, false, 1, error::kInvalidOutput}};
  for (const auto& test : cases) {
    auto input = Input({"A", "B"});
    input.count = test.count;
    if (test.omit_view) input.views.clear();
    auto output = Output();
    output.rows.resize(test.rows);
    std::string error;
    EXPECT_EQ(runtime->Process(input, &output, &error), test.code);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(trace->processed, 0);
    EXPECT_EQ(trace->resets, 0);
  }
}

TEST_F(RuntimeFacadeTest,
       DecodeExecutionEncodeAndCapacityFailuresLeaveAllSlotsEmpty) {
  struct Case {
    const char* name;
    const char* second;
    int execution;
    bool writer;
    int capacity;
    int expected;
    int resets;
  };
  const std::vector<Case> cases = {
      {"decode", "decode_fail", 0, false, 64, error::kInvalidInput, 0},
      {"execution", "B", -72, false, 64, -72, 2},
      {"encode", "B", 0, true, 64, error::kInvalidInput, 2},
      {"capacity", "long", 0, false, 1, error::kInvalidOutput, 2}};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    trace->execution_code = test.execution;
    trace->fail_write = test.writer;
    trace->resets = 0;
    Create(false, test.capacity);
    ASSERT_NE(runtime, nullptr);
    auto output = Output();
    RuntimeFailureStage stage;
    std::string error;
    const std::string first = "A";
    EXPECT_EQ(
        runtime->Process(Input({first, test.second}), &output, &error, &stage),
        test.expected);
    EXPECT_EQ(stage, test.execution ? RuntimeFailureStage::kExecution
                                    : RuntimeFailureStage::kNone);
    EXPECT_EQ(output.rows[0][0].value, nullptr);
    EXPECT_EQ(output.rows[1][0].value, nullptr);
    EXPECT_EQ(trace->resets, test.resets);
    EXPECT_FALSE(error.empty());
    runtime.reset();
    EXPECT_EQ(trace->live, 0);
  }
}

TEST_F(RuntimeFacadeTest,
       OptionalRowsUseActualRequestedCountAndRejectMisreport) {
  for (const bool misreport : {false, true}) {
    Create(misreport);
    ASSERT_NE(runtime, nullptr);
    auto output = Output();
    output.rows[0].clear();
    std::string error;
    EXPECT_EQ(runtime->Process(Input({"A", "B"}), &output, &error),
              misreport ? error::kInvalidOutput : error::kOk);
    EXPECT_TRUE(output.rows[0].empty());
    if (misreport) {
      EXPECT_EQ(output.rows[1][0].value, nullptr);
      EXPECT_NE(error.find("wrote 2 rows; expected 1"), std::string::npos);
    } else {
      ASSERT_NE(output.rows[1][0].value, nullptr);
      EXPECT_EQ(
          static_cast<OutputCarrier*>(output.rows[1][0].value.get())->text,
          "B");
    }
    output.rows[1][0].value.reset();
    runtime.reset();
  }
}

TEST_F(RuntimeFacadeTest, ProcessAndControlSerializeInBothDirections) {
  Create();
  ASSERT_NE(runtime, nullptr);
  auto invoke = [&](bool control) {
    if (control) return runtime->ExecuteControl({kProbeCommand, "{}"});
    auto output = Output();
    return runtime->Process(Input({"A", "B"}), &output);
  };
  for (const bool control : {false, true}) {
    trace->pause = true;
    trace->entered = false;
    trace->released = false;
    auto operation = std::async(std::launch::async,
                                [&, control] { return invoke(control); });
    const bool entered = trace->AwaitEntry();
    std::promise<void> initiated;
    auto started = initiated.get_future();
    auto competing = std::async(std::launch::async, [&] {
      initiated.set_value();
      return invoke(!control);
    });
    const bool launched =
        started.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    const bool blocked = competing.wait_for(std::chrono::milliseconds(100)) ==
                         std::future_status::timeout;
    trace->Release();
    const bool completed = operation.wait_for(std::chrono::seconds(5)) ==
                           std::future_status::ready;
    const bool competing_completed =
        competing.wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready;
    ASSERT_TRUE(entered);
    EXPECT_TRUE(launched);
    EXPECT_TRUE(blocked);
    ASSERT_TRUE(completed);
    ASSERT_TRUE(competing_completed);
    EXPECT_EQ(operation.get(), 0);
    EXPECT_EQ(competing.get(), 0);
  }
  EXPECT_EQ(trace->processed, 2);
  EXPECT_EQ(trace->control_calls, 2);
}

TEST_F(RuntimeFacadeTest,
       CloseConsumesResourcesEvenWhenReportingOutstandingLease) {
  Create();
  ASSERT_NE(runtime, nullptr);
  auto output = Output();
  std::string error;
  ASSERT_EQ(runtime->Process(Input({"A", "B"}), &output, &error), 0);
  EXPECT_EQ(runtime->Close(&error), error::kInvalidState);
  EXPECT_NE(error.find("2 unreturned output blocks"), std::string::npos);
  EXPECT_EQ(runtime->GetIoPlan(), nullptr);
  EXPECT_EQ(trace->live, 0);
  EXPECT_EQ(trace->nodes_destroyed, 1);
  EXPECT_EQ(runtime->Close(), 0);
  output.rows[0][0].value.reset();
  output.rows[1][0].value.reset();
  EXPECT_EQ(trace->resets, 0);
  EXPECT_EQ(runtime->Process(Input({"A", "B"}), &output), error::kInvalidState);
  EXPECT_EQ(runtime->ExecuteControl({kProbeCommand, "{}"}),
            error::kInvalidState);
}

struct PublishAllocationObserver {
  std::shared_ptr<OutputPoolState>* pool;
  RuntimeOutputBatch* output;
  long second_control_block_count;
  size_t callbacks = 0;
  bool identified_second_control_block = false;
  bool all_slots_empty = false;
  long observed_count = 0;
  std::array<std::optional<test_support::ScopedNextAllocationCallback>, 16>
      hooks;
  std::optional<test_support::ScopedAllocationFailure> failure;

  static void Observe(void* data) {
    auto& observer = *static_cast<PublishAllocationObserver*>(data);
    ++observer.callbacks;
    const long owners = observer.pool->use_count();
    if (owners == observer.second_control_block_count) {
      observer.identified_second_control_block = true;
      observer.observed_count = owners;
      observer.all_slots_empty = !observer.output->rows[0][0].value &&
                                 !observer.output->rows[1][0].value;
      observer.failure.emplace(0);
    } else if (observer.callbacks < observer.hooks.size()) {
      observer.hooks[observer.callbacks].emplace(&Observe, &observer);
    }
  }
};

TEST_F(RuntimeFacadeTest,
       PublishStagesEveryControlBlockBeforeMovingCallerSlots) {
  ResolvedOutputPoolSpec spec;
  spec.type = "json_out";
  spec.capacities["result_json"] = 64;
  std::shared_ptr<OutputPoolState> pool;
  std::string error;
  ASSERT_EQ(OutputPoolState::Create("json_out", 2, spec, &output_binding, &pool,
                                    &error),
            0)
      << error;
  auto output = Output();
  ScopedOutputLeaseGuard guard;
  std::vector<AcquiredOutputBlock> acquired;
  ASSERT_EQ(
      AcquireRuntimeOutputBlocks(output, {pool}, &guard, &acquired, &error), 0);
  ASSERT_EQ(acquired.size(), 2U);
  EXPECT_EQ(pool->CheckedOutCount(), 2U);
  // 每次 Untrack 移除一个强引用守卫持有者。容器暂存不改变该计数，由此在
  // 语义上确认是同一次 shared_ptr 分配。
  const long second_control_block_count = pool.use_count() - 2;
  bool threw = false;
  bool triggered = false;
  bool observed_second = false;
  bool caller_empty_during_allocation = false;
  long observed_count = 0;
  {
    PublishAllocationObserver observer;
    observer.pool = &pool;
    observer.output = &output;
    observer.second_control_block_count = second_control_block_count;
    observer.hooks[0].emplace(&PublishAllocationObserver::Observe, &observer);
    try {
      PublishRuntimeOutputs(acquired, &output, &guard);
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    observed_second = observer.identified_second_control_block;
    caller_empty_during_allocation = observer.all_slots_empty;
    observed_count = observer.observed_count;
    triggered = observer.failure && observer.failure->Triggered();
  }
  EXPECT_TRUE(threw);
  EXPECT_TRUE(observed_second);
  EXPECT_TRUE(triggered);
  EXPECT_TRUE(caller_empty_during_allocation);
  EXPECT_EQ(observed_count, second_control_block_count);
  EXPECT_EQ(output.rows[0][0].value, nullptr);
  EXPECT_EQ(output.rows[1][0].value, nullptr);
  EXPECT_EQ(pool->CheckedOutCount(), 0U);
  EXPECT_EQ(pool->FreeBlockCount(), 2U);
  EXPECT_EQ(trace->resets, 2);
  EXPECT_EQ(pool.use_count(), second_control_block_count);
  guard.Rollback();
  EXPECT_EQ(trace->resets, 2);
}

}  // namespace
}  // namespace llm_edgeflow
