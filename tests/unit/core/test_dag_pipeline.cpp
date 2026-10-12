#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/pipeline.h"
#include "core/thread_pool.h"
#include "tests/support/registry_test_access.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;
constexpr int kUpdate = 8127;
const SessionResourceKey<int> kWitness("runtime_test_witness");

enum class Failure { kNone, kReject, kStandard, kUnknown };
struct Behavior {
  Failure init = Failure::kNone;
  Failure process = Failure::kNone;
  int process_code = 0;
  std::string process_error;
  bool blocked = false;
  NodeControlStatus control = NodeControlStatus::kHandled;
  int control_code = 0;
};

struct Recording {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::string, Behavior> behavior;
  std::vector<std::string> initialized, started, completed, destroyed,
      controlled;
  std::map<std::string, const ValidatedNodePlan*> plans;
  std::map<std::string, SessionContext*> sessions;
  std::map<std::string, int> control_values;
  std::set<std::string> released;
  std::weak_ptr<int> resource;
  Pipeline* pipeline = nullptr;
  std::vector<Pipeline::State> init_states;
  bool retained_addresses_valid = true;
  size_t active = 0;
  size_t completed_at_return = 0;
  size_t active_at_return = 0;
  size_t config_validations = 0;
  std::thread::id diagnostic_fault_caller;
  std::function<void()> arm_diagnostic_fault;
  std::string diagnostic_name;
  bool diagnostic_fault_armed = false;
  bool diagnostic_fault_triggered = false;
  bool caught_bad_alloc = false;
};

class RecorderNode final : public INode {
 public:
  RecorderNode(std::shared_ptr<Recording> recording, std::string type)
      : recording_(std::move(recording)), type_(std::move(type)) {}
  ~RecorderNode() override {
    std::lock_guard<std::mutex> lock(recording_->mutex);
    if (plan_) {
      recording_->retained_addresses_valid &=
          plan_->node.name == instance_ && session_ &&
          session_->GetResource(kWitness) == resource_.lock();
      recording_->destroyed.push_back(instance_);
    }
  }
  bool Init(const NodeInitContext& context) override {
    plan_ = context.plan;
    session_ = context.session_ctx;
    instance_ = plan_->node.name;
    auto resource = session_->GetOrCreateResource(
        kWitness, [] { return std::make_shared<int>(19); });
    resource_ = resource;
    {
      std::lock_guard<std::mutex> lock(recording_->mutex);
      recording_->initialized.push_back(instance_);
      recording_->plans[instance_] = plan_;
      recording_->sessions[instance_] = session_;
      recording_->resource = resource;
      if (recording_->pipeline)
        recording_->init_states.push_back(recording_->pipeline->GetState());
      behavior_ = recording_->behavior[instance_];
    }
    if (behavior_.init == Failure::kStandard)
      throw std::runtime_error("init failure");
    if (behavior_.init == Failure::kUnknown) throw 19;
    if (behavior_.init == Failure::kReject)
      return context.Fail("init rejected");
    return true;
  }
  int Process(AlgContext* context) override {
    {
      std::unique_lock<std::mutex> lock(recording_->mutex);
      recording_->started.push_back(instance_);
      ++recording_->active;
      recording_->changed.notify_all();
      if (behavior_.blocked)
        recording_->changed.wait(
            lock, [&] { return recording_->released.count(instance_) > 0; });
    }
    if (!behavior_.process_error.empty())
      context->SetError(behavior_.process_code, behavior_.process_error);
    if (behavior_.process == Failure::kNone && behavior_.process_code == 0)
      context->Publish(instance_ + ".completed", 1);
    {
      std::lock_guard<std::mutex> lock(recording_->mutex);
      recording_->completed.push_back(instance_);
      --recording_->active;
      recording_->changed.notify_all();
    }
    if (behavior_.process == Failure::kStandard)
      throw std::runtime_error("process failure");
    if (behavior_.process == Failure::kUnknown) throw 23;
    return behavior_.process_code;
  }
  NodeControlResult Control(int, const std::string& payload) override {
    const auto value = Json::parse(payload).at("limit").get<int>();
    std::lock_guard<std::mutex> lock(recording_->mutex);
    recording_->controlled.push_back(instance_);
    if (behavior_.control == NodeControlStatus::kFailed)
      return NodeControlResult::Failed(behavior_.control_code,
                                       "update rejected");
    if (behavior_.control == NodeControlStatus::kUnsupported)
      return NodeControlResult::Unsupported();
    recording_->control_values[instance_] = value;
    return NodeControlResult::Handled();
  }
  const std::string& Name() const override {
    std::lock_guard<std::mutex> lock(recording_->mutex);
    if (recording_->arm_diagnostic_fault &&
        recording_->diagnostic_fault_caller == std::this_thread::get_id() &&
        !recording_->diagnostic_fault_armed) {
      recording_->arm_diagnostic_fault();
      recording_->diagnostic_fault_armed = true;
      recording_->changed.notify_all();
    }
    return recording_->diagnostic_name.empty() ? type_
                                               : recording_->diagnostic_name;
  }

 private:
  std::shared_ptr<Recording> recording_;
  std::string type_, instance_;
  const ValidatedNodePlan* plan_ = nullptr;
  SessionContext* session_ = nullptr;
  Behavior behavior_;
  std::weak_ptr<int> resource_;
};

class PipelineRuntimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_support::RegistryTestAccess::ResetNodes();
    NodeDefinition definition;
    definition.node_type = "runtime_recorder";
    definition.parallel_safe = true;
    definition.validate_config = [recording = recording_](
                                     const Json&, const BindingFacts&,
                                     std::string*, std::string*) {
      std::lock_guard<std::mutex> lock(recording->mutex);
      ++recording->config_validations;
      return true;
    };
    definition.control_commands = {
        {kUpdate,
         "update",
         "",
         {{"type", "object"},
          {"required", {"limit"}},
          {"additionalProperties", false},
          {"properties", {{"limit", {{"type", "integer"}, {"minimum", 1}}}}}}}};
    ASSERT_TRUE(NodeRegistry::Instance().Register(
        definition.node_type,
        [recording = recording_] {
          return std::make_unique<RecorderNode>(recording, "runtime_recorder");
        },
        definition));
  }

  std::unique_ptr<ValidatedPipelinePlan> Plan(
      const std::vector<std::string>& names = {"first"}, size_t workers = 1,
      bool chain = false) {
    auto nodes = Json::array();
    for (size_t i = 0; i < names.size(); ++i) {
      Json node = {{"name", names[i]}, {"type", "runtime_recorder"}};
      if (chain && i) node["depends_on"] = {names[i - 1]};
      nodes.push_back(node);
    }
    return std::make_unique<ValidatedPipelinePlan>(
        PipelineValidator::ValidateAndPlan(
            {{"pipeline", nodes}, {"max_parallel_workers", workers}}, {}));
  }

  std::unique_ptr<ValidatedPipelinePlan> ParallelPlan() {
    return std::make_unique<ValidatedPipelinePlan>(
        PipelineValidator::ValidateAndPlan(
            {{"max_parallel_workers", 2},
             {"pipeline",
              {{{"name", "first"}, {"type", "runtime_recorder"}},
               {{"name", "second"}, {"type", "runtime_recorder"}},
               {{"name", "lower"},
                {"type", "runtime_recorder"},
                {"depends_on", {"first", "second"}}}}}},
            {}));
  }

  bool WaitFor(const std::vector<std::string>& names, bool completed = false) {
    std::unique_lock<std::mutex> lock(recording_->mutex);
    return recording_->changed.wait_for(lock, std::chrono::seconds(3), [&] {
      const auto& events =
          completed ? recording_->completed : recording_->started;
      for (const auto& name : names)
        if (std::find(events.begin(), events.end(), name) == events.end())
          return false;
      return true;
    });
  }

  void Release(const std::vector<std::string>& names) {
    std::lock_guard<std::mutex> lock(recording_->mutex);
    recording_->released.insert(names.begin(), names.end());
    recording_->changed.notify_all();
  }

  void ExpectFailed(Pipeline& pipeline, PipelineDiagnostic& diagnostic) {
    EXPECT_EQ(pipeline.GetState(), Pipeline::State::kFailed);
    AlgContext context;
    EXPECT_EQ(pipeline.Execute(&context), -1);
    EXPECT_EQ(pipeline.Control(kUpdate, R"({"limit":2})"), -1);
    EXPECT_FALSE(pipeline.BuildFromPlan(Plan(), &diagnostic));
    EXPECT_EQ(diagnostic.code, DiagnosticCode::kInvalidBuildState);
    EXPECT_EQ(pipeline.GetState(), Pipeline::State::kFailed);
  }

  test_support::RegistryTestAccess::ScopedNodeState registry_;
  std::shared_ptr<Recording> recording_ = std::make_shared<Recording>();
};

TEST_F(PipelineRuntimeTest, EmptyAndReadyStateGuardsAndOneShotBuild) {
  Pipeline pipeline;
  recording_->pipeline = &pipeline;
  EXPECT_EQ(pipeline.GetState(), Pipeline::State::kEmpty);
  AlgContext context;
  EXPECT_EQ(pipeline.Execute(&context), -1);
  ControlFailureStage stage = ControlFailureStage::kNode;
  EXPECT_EQ(pipeline.Control(kUpdate, R"({"limit":2})", nullptr, &stage), -1);
  EXPECT_EQ(stage, ControlFailureStage::kNone);
  PipelineDiagnostic diagnostic{DiagnosticCode::kFieldType, "/stale", "stale"};
  ASSERT_TRUE(pipeline.BuildFromPlan(Plan(), &diagnostic));
  EXPECT_EQ(diagnostic.code, DiagnosticCode::kOk);
  EXPECT_TRUE(diagnostic.path.empty());
  EXPECT_EQ(recording_->init_states,
            std::vector<Pipeline::State>{Pipeline::State::kBuilding});
  EXPECT_TRUE(pipeline.IsReady());
  EXPECT_EQ(pipeline.GetExecutionMode(), Pipeline::ExecutionMode::kSequential);
  EXPECT_EQ(pipeline.Execute(nullptr), -1);
  EXPECT_EQ(pipeline.Execute(&context), 0);
  EXPECT_FALSE(pipeline.BuildFromPlan(Plan(), &diagnostic));
  EXPECT_EQ(diagnostic.code, DiagnosticCode::kInvalidBuildState);
  EXPECT_TRUE(pipeline.IsReady());
  EXPECT_EQ(recording_->initialized.size(), 1u);
}

TEST_F(PipelineRuntimeTest,
       InvalidAndNullPlansFailWithoutMaterializationOrRetry) {
  for (bool null_plan : {true, false}) {
    SCOPED_TRACE(null_plan);
    Pipeline pipeline;
    auto plan = null_plan ? nullptr
                          : std::make_unique<ValidatedPipelinePlan>(
                                PipelineValidator::ValidateAndPlan(
                                    {{"pipeline", Json::array()}}, {}));
    PipelineDiagnostic diagnostic;
    EXPECT_FALSE(pipeline.BuildFromPlan(std::move(plan), &diagnostic));
    EXPECT_EQ(diagnostic.code, null_plan ? DiagnosticCode::kInternalException
                                         : DiagnosticCode::kFieldRange);
    EXPECT_EQ(diagnostic.path, null_plan ? "/" : "/pipeline");
    EXPECT_TRUE(recording_->initialized.empty());
    ExpectFailed(pipeline, diagnostic);
  }
}

TEST_F(PipelineRuntimeTest,
       OwnsTheExactPlanAndStableSessionThroughNodeDestruction) {
  {
    Pipeline pipeline;
    RuntimeOptions options;
    options.has_device_id = true;
    options.device_id = 3;
    options.chip_type = "test_target";
    pipeline.GetSessionContext().SetRuntimeOptions(options);
    auto plan = std::make_unique<ValidatedPipelinePlan>(
        PipelineValidator::ValidateAndPlan(
            {{"pipeline",
              {{{"name", "second"},
                {"type", "runtime_recorder"},
                {"depends_on", {"first"}}},
               {{"name", "first"}, {"type", "runtime_recorder"}}}}},
            {}));
    ASSERT_TRUE(plan->report.ok);
    const auto* original = plan.get();
    const auto validations_after_planning = recording_->config_validations;
    EXPECT_EQ(validations_after_planning, 2u);
    ASSERT_TRUE(pipeline.BuildFromPlan(std::move(plan)));
    EXPECT_EQ(recording_->config_validations, validations_after_planning);
    EXPECT_EQ(&pipeline.GetPlan(), original);
    EXPECT_EQ(recording_->initialized,
              (std::vector<std::string>{"first", "second"}));
    for (const auto& name : {"second", "first"}) {
      EXPECT_EQ(recording_->plans.at(name),
                &pipeline.GetPlan().node_plans.at(name));
      EXPECT_EQ(recording_->sessions.at(name), &pipeline.GetSessionContext());
    }
    EXPECT_EQ(pipeline.GetSessionContext().GetRuntimeOptions().device_id, 3);
    EXPECT_EQ(pipeline.GetSessionContext().GetRuntimeOptions().chip_type,
              "test_target");
    AlgContext context;
    EXPECT_EQ(pipeline.Execute(&context), 0);
    EXPECT_EQ(recording_->started,
              (std::vector<std::string>{"first", "second"}));
    EXPECT_FALSE(recording_->resource.expired());
  }
  EXPECT_EQ(recording_->destroyed.size(), 2u);
  EXPECT_TRUE(recording_->retained_addresses_valid);
  EXPECT_TRUE(recording_->resource.expired());
}

TEST_F(PipelineRuntimeTest, SeparatePipelinesOwnSeparateNodesAndSessions) {
  Pipeline first, second;
  ASSERT_TRUE(first.BuildFromPlan(Plan({"one"})));
  ASSERT_TRUE(second.BuildFromPlan(Plan({"two"})));
  EXPECT_NE(recording_->plans.at("one"), recording_->plans.at("two"));
  EXPECT_NE(recording_->sessions.at("one"), recording_->sessions.at("two"));
  AlgContext context;
  EXPECT_EQ(first.Execute(&context), 0);
  EXPECT_EQ(second.Execute(&context), 0);
  EXPECT_EQ(recording_->started, (std::vector<std::string>{"one", "two"}));
}

TEST_F(PipelineRuntimeTest,
       CreationNullAndExceptionsDestroyOnlyTheStagedPrefix) {
  for (Failure failure :
       {Failure::kReject, Failure::kStandard, Failure::kUnknown}) {
    SCOPED_TRACE(static_cast<int>(failure));
    auto definition = *NodeRegistry::Instance().Find("runtime_recorder");
    definition.node_type =
        "runtime_create_" + std::to_string(static_cast<int>(failure));
    definition.control_commands.clear();
    ASSERT_TRUE(NodeRegistry::Instance().Register(
        definition.node_type,
        [failure]() -> std::unique_ptr<INode> {
          if (failure == Failure::kStandard)
            throw std::runtime_error("create failure");
          if (failure == Failure::kUnknown) throw 29;
          return nullptr;
        },
        definition));
    auto plan = std::make_unique<ValidatedPipelinePlan>(
        PipelineValidator::ValidateAndPlan(
            {{"pipeline",
              {{{"name", "prefix"}, {"type", "runtime_recorder"}},
               {{"name", "bad"}, {"type", definition.node_type}},
               {{"name", "untouched"}, {"type", "runtime_recorder"}}}}},
            {}));
    ASSERT_TRUE(plan->report.ok);
    Pipeline pipeline;
    auto* original_session = &pipeline.GetSessionContext();
    PipelineDiagnostic diagnostic;
    ASSERT_FALSE(pipeline.BuildFromPlan(std::move(plan), &diagnostic));
    EXPECT_EQ(diagnostic.code, DiagnosticCode::kNodeCreateFailed);
    EXPECT_EQ(diagnostic.path, "/pipeline/1/type");
    EXPECT_EQ(&pipeline.GetSessionContext(), original_session);
    EXPECT_TRUE(pipeline.GetPlan().node_plans.empty());
    EXPECT_EQ(recording_->initialized.back(), "prefix");
    EXPECT_EQ(recording_->destroyed.back(), "prefix");
    EXPECT_TRUE(recording_->resource.expired());
    EXPECT_TRUE(recording_->retained_addresses_valid);
    ExpectFailed(pipeline, diagnostic);
  }
}

TEST_F(PipelineRuntimeTest,
       InitRejectionAndExceptionsLeaveNoPublishedAssembly) {
  for (Failure failure :
       {Failure::kReject, Failure::kStandard, Failure::kUnknown}) {
    SCOPED_TRACE(static_cast<int>(failure));
    recording_->behavior["bad"].init = failure;
    recording_->initialized.clear();
    recording_->destroyed.clear();
    Pipeline pipeline;
    auto* original_session = &pipeline.GetSessionContext();
    PipelineDiagnostic diagnostic;
    ASSERT_FALSE(pipeline.BuildFromPlan(Plan({"prefix", "bad", "untouched"}),
                                        &diagnostic));
    EXPECT_EQ(diagnostic.code, DiagnosticCode::kNodeInitFailed);
    EXPECT_EQ(diagnostic.path, "/pipeline/1/params");
    EXPECT_EQ(recording_->initialized,
              (std::vector<std::string>{"prefix", "bad"}));
    EXPECT_EQ(recording_->destroyed.size(), 2u);
    EXPECT_TRUE(recording_->retained_addresses_valid);
    EXPECT_TRUE(recording_->resource.expired());
    EXPECT_EQ(&pipeline.GetSessionContext(), original_session);
    EXPECT_TRUE(pipeline.GetPlan().node_plans.empty());
    ExpectFailed(pipeline, diagnostic);
  }
}

TEST_F(PipelineRuntimeTest, AssemblyAllocationFailureConvergesToFailedState) {
  auto plan = Plan();
  ASSERT_TRUE(plan->report.ok);
  Pipeline pipeline;
  auto* original_session = &pipeline.GetSessionContext();
  PipelineDiagnostic diagnostic;
  bool built = true;
  bool injected = false;
  {
    test_support::ScopedAllocationFailure fault(0);
    built = pipeline.BuildFromPlan(std::move(plan), &diagnostic);
    injected = fault.Triggered();
  }
  ASSERT_TRUE(injected);
  EXPECT_FALSE(built);
  EXPECT_EQ(diagnostic.code, DiagnosticCode::kInternalException);
  EXPECT_EQ(&pipeline.GetSessionContext(), original_session);
  EXPECT_TRUE(pipeline.GetPlan().node_plans.empty());
  ExpectFailed(pipeline, diagnostic);
}

TEST_F(PipelineRuntimeTest,
       SequentialExecutionStopsAtFailureAndPreservesUpstreamData) {
  recording_->behavior["bad"].process_code = -31;
  recording_->behavior["bad"].process_error = "request rejected";
  Pipeline pipeline;
  ASSERT_TRUE(
      pipeline.BuildFromPlan(Plan({"prefix", "bad", "untouched"}, 1, true)));
  AlgContext context;
  EXPECT_EQ(pipeline.Execute(&context), -31);
  EXPECT_EQ(recording_->started, (std::vector<std::string>{"prefix", "bad"}));
  ASSERT_NE(context.Read<int>("prefix.completed"), nullptr);
  EXPECT_EQ(*context.Read<int>("prefix.completed"), 1);
  EXPECT_FALSE(context.Has("untouched.completed"));
  EXPECT_EQ(context.GetErrorCode(), -31);
  EXPECT_NE(context.GetErrorMessage().find("request rejected"),
            std::string::npos);
  EXPECT_TRUE(pipeline.IsReady());
}

TEST_F(PipelineRuntimeTest,
       ProcessExceptionsAreContainedInSequentialAndParallelExecution) {
  for (size_t workers : {1u, 2u}) {
    for (Failure failure : {Failure::kStandard, Failure::kUnknown}) {
      SCOPED_TRACE(workers);
      SCOPED_TRACE(static_cast<int>(failure));
      recording_->behavior["bad"].process = failure;
      Pipeline pipeline;
      ASSERT_TRUE(pipeline.BuildFromPlan(Plan({"bad", "peer"}, workers)));
      AlgContext context;
      EXPECT_EQ(pipeline.Execute(&context), -1);
      EXPECT_EQ(context.GetErrorCode(), -1);
      EXPECT_NE(context.GetErrorMessage().find("bad"), std::string::npos);
      EXPECT_NE(
          context.GetErrorMessage().find(failure == Failure::kStandard
                                             ? "process failure"
                                             : "Unknown Process exception"),
          std::string::npos);
      EXPECT_EQ(recording_->active, 0u);
      EXPECT_TRUE(pipeline.IsReady());
    }
  }
}

TEST_F(PipelineRuntimeTest,
       ParallelJoinAndErrorSelectionFollowPlanInsteadOfCompletionOrder) {
  recording_->behavior["first"].blocked = true;
  recording_->behavior["first"].process_code = -11;
  recording_->behavior["second"].blocked = true;
  recording_->behavior["second"].process_code = -22;
  recording_->behavior["second"].process_error = "second diagnostic";
  auto plan = ParallelPlan();
  ASSERT_TRUE(plan->report.ok);
  Pipeline pipeline;
  ASSERT_TRUE(pipeline.BuildFromPlan(std::move(plan)));
  EXPECT_EQ(pipeline.GetExecutionMode(), Pipeline::ExecutionMode::kParallel);
  AlgContext context;
  int code = 0;
  std::thread execution([&] {
    code = pipeline.Execute(&context);
    std::lock_guard<std::mutex> lock(recording_->mutex);
    recording_->completed_at_return = recording_->completed.size();
    recording_->changed.notify_all();
  });
  const bool both_entered = WaitFor({"first", "second"});
  Release({"second"});
  const bool second_completed = WaitFor({"second"}, true);
  Release({"first", "second"});
  execution.join();
  // No fatal assertions occur while a task or the caller thread is blocked.
  ASSERT_TRUE(both_entered);
  ASSERT_TRUE(second_completed);
  EXPECT_EQ(code, -11);
  EXPECT_EQ(recording_->completed,
            (std::vector<std::string>{"second", "first"}));
  EXPECT_EQ(recording_->completed_at_return, 2u);
  EXPECT_EQ(recording_->active, 0u);
  EXPECT_EQ(recording_->started.size(), 2u);
  EXPECT_EQ(context.GetErrorCode(), -11);
  EXPECT_NE(context.GetErrorMessage().find("first"), std::string::npos);
  EXPECT_NE(context.GetErrorMessage().find("-11"), std::string::npos);
  EXPECT_EQ(context.GetErrorMessage().find("second diagnostic"),
            std::string::npos);
}

TEST_F(PipelineRuntimeTest, FirstFailureWaitsForAnAlreadyActivePeer) {
  recording_->behavior["first"].blocked = true;
  recording_->behavior["first"].process_code = -11;
  recording_->behavior["second"].blocked = true;
  auto pipeline = std::make_unique<Pipeline>();
  ASSERT_TRUE(pipeline->BuildFromPlan(ParallelPlan()));
  AlgContext context;
  auto execution = std::async(std::launch::async, [&] {
    const int code = pipeline->Execute(&context);
    std::lock_guard<std::mutex> lock(recording_->mutex);
    recording_->active_at_return = recording_->active;
    recording_->completed_at_return = recording_->completed.size();
    return code;
  });
  const bool both_entered = WaitFor({"first", "second"});
  Release({"first"});
  const bool first_completed = WaitFor({"first"}, true);
  const auto while_peer_blocked =
      execution.wait_for(std::chrono::milliseconds(50));
  Release({"first", "second"});
  const int code = execution.get();
  pipeline.reset();
  // Release and join precede all assertions, including failed handshakes.
  ASSERT_TRUE(both_entered);
  ASSERT_TRUE(first_completed);
  EXPECT_EQ(while_peer_blocked, std::future_status::timeout);
  EXPECT_EQ(code, -11);
  EXPECT_EQ(recording_->active_at_return, 0u);
  EXPECT_EQ(recording_->completed_at_return, 2u);
  EXPECT_EQ(recording_->completed,
            (std::vector<std::string>{"first", "second"}));
  EXPECT_EQ(recording_->started.size(), 2u);
}

TEST_F(PipelineRuntimeTest,
       DiagnosticAllocationUnwindJoinsAnAlreadyActivePeer) {
  recording_->behavior["first"].blocked = true;
  recording_->behavior["first"].process_code = -11;
  recording_->behavior["second"].blocked = true;
  recording_->diagnostic_name = std::string(200, 'd');
  auto pipeline = std::make_unique<Pipeline>();
  ASSERT_TRUE(pipeline->BuildFromPlan(ParallelPlan()));
  AlgContext context;
  auto execution = std::async(std::launch::async, [&] {
    std::optional<test_support::ScopedAllocationFailure> fault;
    {
      std::lock_guard<std::mutex> lock(recording_->mutex);
      recording_->diagnostic_fault_caller = std::this_thread::get_id();
      recording_->arm_diagnostic_fault = [&fault] { fault.emplace(0); };
    }
    try {
      (void)pipeline->Execute(&context);
    } catch (const std::bad_alloc&) {
      std::lock_guard<std::mutex> lock(recording_->mutex);
      recording_->caught_bad_alloc = true;
    }
    std::lock_guard<std::mutex> lock(recording_->mutex);
    recording_->diagnostic_fault_triggered = fault && fault->Triggered();
    recording_->active_at_return = recording_->active;
    recording_->completed_at_return = recording_->completed.size();
    recording_->arm_diagnostic_fault = nullptr;
  });
  const bool both_entered = WaitFor({"first", "second"});
  Release({"first"});
  bool armed = false;
  {
    std::unique_lock<std::mutex> lock(recording_->mutex);
    armed = recording_->changed.wait_for(lock, std::chrono::seconds(3), [&] {
      return recording_->diagnostic_fault_armed;
    });
  }
  const auto while_peer_blocked =
      execution.wait_for(std::chrono::milliseconds(50));
  Release({"first", "second"});
  execution.get();
  pipeline.reset();
  // The caller-only Name callback arms real new failure on copying a long
  // diagnostic name. Workers remain unaffected; no production hook throws.
  ASSERT_TRUE(both_entered);
  ASSERT_TRUE(armed);
  EXPECT_EQ(while_peer_blocked, std::future_status::timeout);
  EXPECT_TRUE(recording_->diagnostic_fault_triggered);
  EXPECT_TRUE(recording_->caught_bad_alloc);
  EXPECT_EQ(recording_->active_at_return, 0u);
  EXPECT_EQ(recording_->completed_at_return, 2u);
  EXPECT_EQ(recording_->started.size(), 2u);
}

TEST_F(PipelineRuntimeTest,
       SuccessfulParallelLayerCompletesBeforeItsDependentLayer) {
  Pipeline pipeline;
  auto plan = ParallelPlan();
  ASSERT_TRUE(pipeline.BuildFromPlan(std::move(plan)));
  AlgContext context;
  EXPECT_EQ(pipeline.Execute(&context), 0);
  EXPECT_EQ(recording_->completed.back(), "lower");
  EXPECT_TRUE(context.Has("first.completed"));
  EXPECT_TRUE(context.Has("second.completed"));
  EXPECT_TRUE(context.Has("lower.completed"));
}

TEST_F(PipelineRuntimeTest, ControlValidatesRequestsBeforeCallingAnyNode) {
  Pipeline pipeline;
  ASSERT_TRUE(pipeline.BuildFromPlan(Plan({"first", "second"})));
  const std::vector<std::string> invalid = {
      "{",
      R"({"limit":0})",
      R"({"unknown":2})",
      R"({"$edgeflow_control":2,"node":"first","payload":{"limit":2}})",
      R"({"$edgeflow_control":1,"node":"first"})",
      R"({"$edgeflow_control":1,"node":"","payload":{"limit":2}})",
      R"({"$edgeflow_control":1,"node":"missing","payload":{"limit":2}})"};
  for (const auto& request : invalid) {
    SCOPED_TRACE(request);
    std::string error;
    ControlFailureStage stage = ControlFailureStage::kNone;
    EXPECT_EQ(pipeline.Control(kUpdate, request, &error, &stage), -1);
    EXPECT_EQ(stage, ControlFailureStage::kRequest);
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(recording_->controlled.empty());
  }
}

TEST_F(PipelineRuntimeTest,
       TargetedControlAndBroadcastApplyTheBusinessPayload) {
  Pipeline pipeline;
  ASSERT_TRUE(pipeline.BuildFromPlan(Plan({"first", "second"})));
  std::string error = "stale";
  ControlFailureStage stage = ControlFailureStage::kRequest;
  EXPECT_EQ(
      pipeline.Control(
          kUpdate,
          R"({"$edgeflow_control":1,"node":"second","payload":{"limit":3}})",
          &error, &stage),
      0);
  EXPECT_EQ(stage, ControlFailureStage::kNone);
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(recording_->controlled, std::vector<std::string>{"second"});
  EXPECT_EQ(recording_->control_values.at("second"), 3);
  EXPECT_EQ(pipeline.Control(kUpdate, R"({"limit":5})", &error, &stage), 0);
  EXPECT_EQ(recording_->controlled,
            (std::vector<std::string>{"second", "first", "second"}));
  EXPECT_EQ(recording_->control_values.at("first"), 5);
  EXPECT_EQ(recording_->control_values.at("second"), 5);
}

TEST_F(PipelineRuntimeTest,
       BroadcastFailureKeepsEarlierUpdatesAndContinuesToLaterNodes) {
  recording_->behavior["bad"].control = NodeControlStatus::kFailed;
  recording_->behavior["bad"].control_code = -37;
  Pipeline pipeline;
  ASSERT_TRUE(pipeline.BuildFromPlan(Plan({"first", "bad", "last"})));
  std::string error;
  ControlFailureStage stage;
  EXPECT_EQ(pipeline.Control(kUpdate, R"({"limit":7})", &error, &stage), -37);
  EXPECT_EQ(stage, ControlFailureStage::kNode);
  EXPECT_EQ(recording_->controlled,
            (std::vector<std::string>{"first", "bad", "last"}));
  EXPECT_EQ(recording_->control_values.at("first"), 7);
  EXPECT_EQ(recording_->control_values.at("last"), 7);
  EXPECT_EQ(recording_->control_values.count("bad"), 0u);
  EXPECT_NE(error.find("bad"), std::string::npos);
  EXPECT_TRUE(pipeline.IsReady());
}

TEST_F(PipelineRuntimeTest,
       UnsupportedAndUnhandledCommandsReportTheirOwnStage) {
  recording_->behavior["first"].control = NodeControlStatus::kUnsupported;
  Pipeline pipeline;
  ASSERT_TRUE(pipeline.BuildFromPlan(Plan()));
  ControlFailureStage stage = ControlFailureStage::kNone;
  EXPECT_EQ(pipeline.Control(kUpdate + 1, R"({"limit":2})", nullptr, &stage),
            -7);
  EXPECT_EQ(stage, ControlFailureStage::kUnsupported);
  EXPECT_TRUE(recording_->controlled.empty());
  EXPECT_EQ(pipeline.Control(kUpdate, R"({"limit":2})", nullptr, &stage), -7);
  EXPECT_EQ(stage, ControlFailureStage::kUnsupported);
  EXPECT_EQ(recording_->controlled, std::vector<std::string>{"first"});
}

TEST_F(PipelineRuntimeTest,
       ThreadPoolConstructionUnwindsEveryAllocationFailure) {
  bool reached_success = false;
  size_t failures = 0;
  for (std::ptrdiff_t point = 0; point < 64 && !reached_success; ++point) {
    test_support::ScopedAllocationFailure fault(point);
    try {
      auto pool = std::make_unique<ThreadPool>(3);
      reached_success = !fault.Triggered();
    } catch (const std::bad_alloc&) {
      ++failures;
    }
  }
  EXPECT_TRUE(reached_success);
  EXPECT_GT(failures, 0u);
}

TEST_F(PipelineRuntimeTest,
       SubmissionAllocationFailuresLeaveNoTasksUsingTheRequest) {
  Pipeline pipeline;
  ASSERT_TRUE(pipeline.BuildFromPlan(Plan({"first", "second"}, 2)));
  bool reached_success = false;
  size_t failures = 0;
  for (std::ptrdiff_t point = 0; point < 128 && !reached_success; ++point) {
    AlgContext context;
    bool injected = false;
    {
      test_support::ScopedAllocationFailure fault(point);
      try {
        (void)pipeline.Execute(&context);
      } catch (const std::bad_alloc&) { /* Before submission, allocation may
                                           escape. */
      }
      injected = fault.Triggered();
    }
    {
      std::lock_guard<std::mutex> lock(recording_->mutex);
      EXPECT_EQ(recording_->active, 0u);
      EXPECT_EQ(recording_->started.size(), recording_->completed.size());
    }
    if (injected)
      ++failures;
    else
      reached_success = true;
  }
  EXPECT_TRUE(reached_success);
  EXPECT_GT(failures, 0u);
}
}  // namespace
}  // namespace llm_edgeflow
