#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/node_registry.h"
#include "core/pipeline.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"
#include "engine/model_runtime_factory.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;
constexpr char kBackend[] = "execution_factory_probe";
constexpr char kModel[] = "execution_factory_model";
constexpr char kSerialized[] = "execution_factory_serialized";
constexpr char kUnlisted[] = "execution_factory_unlisted";

enum class Defect {
  kNone,
  kNullProvider,
  kProviderThrow,
  kProviderUnknown,
  kProviderIdentity,
  kLoad,
  kSessionIdentity,
  kSessionConcurrency,
  kProtocol,
  kZeroBatch,
  kFixedBatch,
  kNullModel,
  kModelThrow,
  kModelUnknown,
  kModelIdentity,
  kCategory,
  kModelConcurrency
};
struct BackendParams {
  int workers;
  std::string label;
};
struct ModelParams {
  int gain;
};
struct Recording {
  Defect defect = Defect::kNone;
  int providers = 0, loads = 0, models = 0;
  std::vector<std::string> files;
  std::vector<ExecutionTarget> targets;
  ExecutionProtocol requested = ExecutionProtocol::kTensorGraph;
  Json backend_effective, model_effective;
  int workers = 0, gain = 0;
  bool same_session = false;
  std::string fail_file;
  std::weak_ptr<int> provider_lifetime;
  std::weak_ptr<IBackendSession> session;
  std::weak_ptr<IModel> model;
  std::vector<std::weak_ptr<IModel>> all_models;
};
std::shared_ptr<Recording> current;

class FactorySession final : public IBackendSession {
 public:
  FactorySession(Defect defect, std::string backend)
      : defect_(defect), backend_(std::move(backend)) {}
  const std::string& BackendType() const noexcept override {
    static const std::string expected = kBackend, wrong = "wrong session";
    return defect_ == Defect::kSessionIdentity ? wrong : backend_;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return defect_ == Defect::kProtocol ? ExecutionProtocol::kTensorGraph
                                        : ExecutionProtocol::kFixture;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return defect_ == Defect::kSessionConcurrency
               ? InferenceConcurrency::kSerialized
               : InferenceConcurrency::kConcurrent;
  }
  BatchPolicy GetBatchPolicy() const noexcept override {
    if (defect_ == Defect::kZeroBatch) return {0, 0};
    if (defect_ == Defect::kFixedBatch) return {3, 2};
    return {3, 3};
  }

 private:
  Defect defect_;
  std::string backend_;
};

class FactoryBackend final : public IInferenceBackend {
 public:
  FactoryBackend(std::shared_ptr<Recording> recording,
                 std::string backend = kBackend)
      : recording_(std::move(recording)),
        backend_(std::move(backend)),
        lifetime_(std::make_shared<int>(0)) {
    recording_->provider_lifetime = lifetime_;
  }
  const std::string& BackendType() const noexcept override {
    static const std::string expected = kBackend, wrong = "wrong provider";
    return recording_->defect == Defect::kProviderIdentity ? wrong : backend_;
  }
  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec, std::string* diagnostic) noexcept override {
    ++recording_->loads;
    recording_->files.push_back(spec.model_file);
    recording_->targets.push_back(spec.execution_target);
    recording_->requested = spec.requested_protocol;
    recording_->workers = spec.Params<BackendParams>().workers;
    recording_->backend_effective = spec.params->Effective();
    if (recording_->defect == Defect::kLoad ||
        spec.model_file == recording_->fail_file) {
      *diagnostic = "load refused literal resource";
      return nullptr;
    }
    auto session =
        std::make_shared<FactorySession>(recording_->defect, backend_);
    recording_->session = session;
    return session;
  }

 private:
  std::shared_ptr<Recording> recording_;
  std::string backend_;
  std::shared_ptr<int> lifetime_;
};

class FactoryModel final : public IModel {
 public:
  FactoryModel(std::shared_ptr<IBackendSession> session, Defect defect)
      : session_(std::move(session)), defect_(defect) {}
  const std::string& ImplName() const noexcept override {
    static const std::string expected = kModel, wrong = "wrong model";
    return defect_ == Defect::kModelIdentity ? wrong : expected;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string expected = "execution_probe",
                             wrong = "wrong category";
    return defect_ == Defect::kCategory ? wrong : expected;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return defect_ == Defect::kModelConcurrency
               ? InferenceConcurrency::kSerialized
               : InferenceConcurrency::kConcurrent;
  }

 private:
  std::shared_ptr<IBackendSession> session_;
  Defect defect_;
};

class MaterializationNode final : public INode {
 public:
  bool Init(const NodeInitContext& context) override {
    auto& manager = context.session_ctx->GetModelManager();
    return manager.HasModel("first") && manager.HasModel("second") &&
           manager.HasModel("third");
  }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = "factory_materialization_node";
    return name;
  }
};

class ModelRuntimeFactoryTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    BackendDefinition backend;
    backend.backend_type = kBackend;
    backend.supported_protocols = {ExecutionProtocol::kFixture};
    backend.concurrency = InferenceConcurrency::kConcurrent;
    backend.params = ParameterSet(Parameters<BackendParams>{
        Field("workers", &BackendParams::workers).Default(3).Minimum(1),
        Field("label", &BackendParams::label).Default("neutral")});
    ASSERT_TRUE(BackendRegistry::Instance().Register(
        backend, []() -> std::unique_ptr<IInferenceBackend> {
          ++current->providers;
          if (current->defect == Defect::kNullProvider) return nullptr;
          if (current->defect == Defect::kProviderThrow)
            throw std::runtime_error("provider exception");
          if (current->defect == Defect::kProviderUnknown) throw 19;
          return std::make_unique<FactoryBackend>(current);
        }));
    backend.backend_type = kSerialized;
    backend.concurrency = InferenceConcurrency::kSerialized;
    ASSERT_TRUE(BackendRegistry::Instance().Register(backend, [] {
      ++current->providers;
      return std::make_unique<FactoryBackend>(current, kSerialized);
    }));
    backend.backend_type = kUnlisted;
    ASSERT_TRUE(BackendRegistry::Instance().Register(backend, [] {
      ++current->providers;
      return std::make_unique<FactoryBackend>(current, kUnlisted);
    }));
    ModelDefinition model;
    model.impl_name = kModel;
    model.model_type = "execution_probe";
    model.required_protocol = ExecutionProtocol::kFixture;
    model.fixture_backends = {kBackend, kSerialized};
    model.concurrency = InferenceConcurrency::kConcurrent;
    model.params = ParameterSet(Parameters<ModelParams>{
        Field("gain", &ModelParams::gain).Default(2).Minimum(1)});
    ASSERT_TRUE(ModelRegistry::Instance().Register(
        model,
        [](const ModelCreateContext& ctx,
           std::string* diagnostic) -> std::shared_ptr<IModel> {
          ++current->models;
          current->gain = ctx.Params<ModelParams>().gain;
          current->model_effective = ctx.params->Effective();
          current->same_session =
              ctx.backend_session == current->session.lock();
          if (current->defect == Defect::kNullModel) {
            *diagnostic = "model refused";
            return nullptr;
          }
          if (current->defect == Defect::kModelThrow)
            throw std::runtime_error("model exception");
          if (current->defect == Defect::kModelUnknown) throw 23;
          auto value = std::make_shared<FactoryModel>(ctx.backend_session,
                                                      current->defect);
          current->model = value;
          current->all_models.push_back(value);
          return value;
        }));
    struct NodeParams {
      std::string model;
    };
    NodeDefinition node;
    node.node_type = "factory_materialization_node";
    node.parallel_safe = true;
    node.config_fields =
        Parameters<NodeParams>{Field("model", &NodeParams::model).Required()}
            .Fields();
    node.model_dependencies = {{"resource", "execution_probe", "model"}};
    ASSERT_TRUE(NodeRegistry::Instance().Register(
        node.node_type, [] { return std::make_unique<MaterializationNode>(); },
        node));
  }
  void SetUp() override { current = std::make_shared<Recording>(); }
  void TearDown() override { current.reset(); }
  ModelLoadSpec Spec() {
    ModelLoadSpec spec;
    spec.impl_name = kModel;
    spec.backend_type = kBackend;
    spec.model_file = "/literal/model.bin";
    spec.backend_params = {{"workers", 5}};
    spec.execution_target = {2, "probe-chip"};
    return spec;
  }
};

TEST_F(ModelRuntimeFactoryTest,
       ParsesBeforeLoadingAndTransfersOnlySessionOwnership) {
  std::string diagnostic;
  auto model = ModelRuntimeFactory::Create(Spec(), &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  EXPECT_EQ(current->providers, 1);
  EXPECT_EQ(current->loads, 1);
  EXPECT_EQ(current->models, 1);
  EXPECT_EQ(current->workers, 5);
  EXPECT_EQ(current->gain, 2);
  EXPECT_EQ(current->backend_effective,
            (Json{{"workers", 5}, {"label", "neutral"}}));
  EXPECT_EQ(current->model_effective, (Json{{"gain", 2}}));
  EXPECT_EQ(current->files, (std::vector<std::string>{"/literal/model.bin"}));
  EXPECT_EQ(current->targets.at(0).device_id, 2);
  EXPECT_EQ(current->targets.at(0).platform, "probe-chip");
  EXPECT_EQ(current->requested, ExecutionProtocol::kFixture);
  EXPECT_TRUE(current->same_session);
  EXPECT_TRUE(current->provider_lifetime.expired());
  EXPECT_FALSE(current->session.expired());
  model.reset();
  EXPECT_TRUE(current->session.expired());
  EXPECT_TRUE(current->model.expired());
}

TEST_F(ModelRuntimeFactoryTest,
       StaticSelectionAndParametersHaveNoProviderSideEffects) {
  struct Case {
    const char* name;
    std::function<void(ModelLoadSpec&)> change;
  };
  const std::vector<Case> cases{
      {"unknown backend", [](auto& s) { s.backend_type = "missing"; }},
      {"unknown model", [](auto& s) { s.impl_name = "missing"; }},
      {"fixture whitelist", [](auto& s) { s.backend_type = kUnlisted; }},
      {"model parameter", [](auto& s) { s.model_params = {{"gain", 0}}; }},
      {"backend parameter",
       [](auto& s) { s.backend_params = {{"workers", 0}}; }},
      {"static protocol", [](auto& s) { s.impl_name = "qwen_causal_lm"; }}};
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    auto spec = Spec();
    row.change(spec);
    std::string diagnostic;
    EXPECT_EQ(ModelRuntimeFactory::Create(spec, &diagnostic), nullptr);
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_EQ(current->providers, 0);
    EXPECT_EQ(current->loads, 0);
    EXPECT_EQ(current->models, 0);
  }
}

TEST_F(ModelRuntimeFactoryTest,
       EachRuntimeContractFailureReleasesAllStagedResources) {
  const std::vector<std::pair<const char*, Defect>> cases{
      {"null provider", Defect::kNullProvider},
      {"provider std", Defect::kProviderThrow},
      {"provider unknown", Defect::kProviderUnknown},
      {"provider identity", Defect::kProviderIdentity},
      {"load refusal", Defect::kLoad},
      {"session identity", Defect::kSessionIdentity},
      {"session concurrency", Defect::kSessionConcurrency},
      {"session protocol", Defect::kProtocol},
      {"zero batch", Defect::kZeroBatch},
      {"unequal fixed batch", Defect::kFixedBatch},
      {"null model", Defect::kNullModel},
      {"model std", Defect::kModelThrow},
      {"model unknown", Defect::kModelUnknown},
      {"model identity", Defect::kModelIdentity},
      {"category identity", Defect::kCategory},
      {"model concurrency", Defect::kModelConcurrency}};
  for (const auto& [name, defect] : cases) {
    SCOPED_TRACE(name);
    current = std::make_shared<Recording>();
    current->defect = defect;
    std::string diagnostic;
    EXPECT_EQ(ModelRuntimeFactory::Create(Spec(), &diagnostic), nullptr);
    const char* detail = "";
    switch (defect) {
      case Defect::kNullProvider:
        detail = "Failed to instantiate backend";
        break;
      case Defect::kProviderThrow:
        detail = "provider exception";
        break;
      case Defect::kProviderUnknown:
        detail = "Unknown exception creating backend";
        break;
      case Defect::kProviderIdentity:
        detail = "provider type mismatch";
        break;
      case Defect::kLoad:
        detail = "load refused literal resource";
        break;
      case Defect::kSessionIdentity:
        detail = "session type mismatch";
        break;
      case Defect::kSessionConcurrency:
        detail = "stricter";
        break;
      case Defect::kProtocol:
        detail = "does not match model required protocol";
        break;
      case Defect::kZeroBatch:
      case Defect::kFixedBatch:
        detail = "Invalid session BatchPolicy";
        break;
      case Defect::kNullModel:
        detail = "model refused";
        break;
      case Defect::kModelThrow:
        detail = "model exception";
        break;
      case Defect::kModelUnknown:
        detail = "Unknown exception creating model";
        break;
      case Defect::kModelIdentity:
        detail = "Model identity mismatch";
        break;
      case Defect::kCategory:
        detail = "Model model_type mismatch";
        break;
      case Defect::kModelConcurrency:
        detail = "Model concurrency mismatch";
        break;
      case Defect::kNone:
        break;
    }
    EXPECT_NE(diagnostic.find(detail), std::string::npos) << diagnostic;
    EXPECT_TRUE(current->provider_lifetime.expired());
    EXPECT_TRUE(current->session.expired());
    EXPECT_TRUE(current->model.expired());
    if (defect == Defect::kProviderIdentity) {
      EXPECT_EQ(current->loads, 0);
    }
    if (defect == Defect::kSessionIdentity || defect == Defect::kProtocol ||
        defect == Defect::kZeroBatch || defect == Defect::kFixedBatch ||
        defect == Defect::kSessionConcurrency) {
      EXPECT_EQ(current->models, 0);
    }
    if (defect == Defect::kLoad) {
      EXPECT_NE(diagnostic.find("load refused literal resource"),
                std::string::npos);
    }
  }
}

TEST_F(ModelRuntimeFactoryTest,
       ConcurrentSessionIsAcceptedForSerializedBackendDeclaration) {
  auto spec = Spec();
  spec.backend_type = kSerialized;
  std::string diagnostic;
  auto model = ModelRuntimeFactory::Create(spec, &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  EXPECT_EQ(model->Concurrency(), InferenceConcurrency::kConcurrent);
  EXPECT_TRUE(current->same_session);
}

std::unique_ptr<ValidatedPipelinePlan> MaterializationPlan() {
  Json models = Json::array(), nodes = Json::array();
  for (const char* name : {"first", "second", "third"}) {
    models.push_back({{"name", name},
                      {"type", "execution_probe"},
                      {"backend", {{"type", kBackend}}},
                      {"file", std::string(name) + ".bin"}});
    nodes.push_back({{"name", name},
                     {"type", "factory_materialization_node"},
                     {"params", {{"model", name}}}});
  }
  return std::make_unique<ValidatedPipelinePlan>(
      PipelineValidator::ValidateAndPlan({{"models", models},
                                          {"pipeline", nodes},
                                          {"max_parallel_workers", 1}},
                                         {}));
}

TEST_F(ModelRuntimeFactoryTest,
       PipelinePassesTheExecutionTargetAndCommitsModelsTogether) {
  for (bool specified : {false, true}) {
    SCOPED_TRACE(specified);
    current = std::make_shared<Recording>();
    {
      Pipeline pipeline;
      pipeline.GetSessionContext().SetRuntimeOptions(
          {7, specified, "pipeline-chip"});
      auto plan = MaterializationPlan();
      ASSERT_TRUE(plan->report.ok) << plan->report.ToJson().dump();
      PipelineDiagnostic diagnostic;
      ASSERT_TRUE(pipeline.BuildFromPlan(std::move(plan), &diagnostic))
          << diagnostic.message;
      EXPECT_EQ(current->files, (std::vector<std::string>{
                                    "first.bin", "second.bin", "third.bin"}));
      ASSERT_EQ(current->targets.size(), 3U);
      for (const auto& target : current->targets) {
        EXPECT_EQ(target.device_id,
                  specified ? std::optional<int>(7) : std::nullopt);
        EXPECT_EQ(target.platform, "pipeline-chip");
      }
      ASSERT_EQ(current->all_models.size(), 3U);
      size_t index = 0;
      for (const char* name : {"first", "second", "third"}) {
        auto model = current->all_models[index++].lock();
        ASSERT_NE(model, nullptr);
        EXPECT_EQ(
            pipeline.GetSessionContext().GetModelManager().GetModel<IModel>(
                name),
            model);
      }
    }
    for (const auto& model : current->all_models) EXPECT_TRUE(model.expired());
    EXPECT_TRUE(current->session.expired());
  }
}

TEST_F(ModelRuntimeFactoryTest,
       PipelineMaterializationFailureDropsPrefixAndKeepsEmptyFacade) {
  current->fail_file = "second.bin";
  Pipeline pipeline;
  auto* original_session = &pipeline.GetSessionContext();
  auto plan = MaterializationPlan();
  ASSERT_TRUE(plan->report.ok) << plan->report.ToJson().dump();
  auto* original_plan = &pipeline.GetPlan();
  PipelineDiagnostic diagnostic;
  EXPECT_FALSE(pipeline.BuildFromPlan(std::move(plan), &diagnostic));
  EXPECT_EQ(diagnostic.code, DiagnosticCode::kModelMaterializationFailed);
  EXPECT_EQ(diagnostic.path, "/models/1");
  EXPECT_EQ(current->files,
            (std::vector<std::string>{"first.bin", "second.bin"}));
  EXPECT_EQ(current->models, 1);
  EXPECT_EQ(pipeline.GetState(), Pipeline::State::kFailed);
  EXPECT_EQ(&pipeline.GetSessionContext(), original_session);
  EXPECT_EQ(&pipeline.GetPlan(), original_plan);
  for (const char* name : {"first", "second", "third"})
    EXPECT_FALSE(pipeline.GetSessionContext().GetModelManager().HasModel(name));
  ASSERT_EQ(current->all_models.size(), 1U);
  EXPECT_TRUE(current->all_models.front().expired());
  EXPECT_TRUE(current->provider_lifetime.expired());
  EXPECT_TRUE(current->session.expired());
}

}  // namespace
}  // namespace llm_edgeflow
