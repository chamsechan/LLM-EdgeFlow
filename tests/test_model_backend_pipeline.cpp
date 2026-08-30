#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "engine/backend_interface.h"
#include "engine/backend_registry.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace alg_framework {
namespace test_mb {

// Counters to verify zero side-effects in validator
static std::atomic<int> g_backend_create_count{0};
static std::atomic<int> g_backend_load_count{0};
static std::atomic<int> g_model_create_count{0};

// Mock Backend Session
class MockBackendSession : public IBackendSession {
 public:
  MockBackendSession(std::string backend_type, ExecutionProtocol protocol,
                     InferenceConcurrency concurrency)
      : backend_type_(std::move(backend_type)),
        protocol_(protocol),
        concurrency_(concurrency) {}

  const std::string& BackendType() const noexcept override {
    return backend_type_;
  }
  ExecutionProtocol Protocol() const noexcept override { return protocol_; }
  InferenceConcurrency Concurrency() const noexcept override {
    return concurrency_;
  }
  BatchPolicy GetBatchPolicy() const noexcept override {
    return BatchPolicy{1, 0};
  }

 private:
  std::string backend_type_;
  ExecutionProtocol protocol_;
  InferenceConcurrency concurrency_;
};

// Mock Backend
class MockInferenceBackend : public IInferenceBackend {
 public:
  inline static constexpr char kBackendType[] = "mock_test_backend";

  explicit MockInferenceBackend(
      std::string backend_type = "mock_test_backend",
      ExecutionProtocol protocol = ExecutionProtocol::kTensorGraph,
      InferenceConcurrency concurrency = InferenceConcurrency::kConcurrent,
      bool should_fail_load = false)
      : backend_type_(std::move(backend_type)),
        protocol_(protocol),
        concurrency_(concurrency),
        should_fail_load_(should_fail_load) {}

  const std::string& BackendType() const noexcept override {
    return backend_type_;
  }

  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec, std::string* diagnostic) noexcept override {
    g_backend_load_count.fetch_add(1);
    last_loaded_spec = spec;
    if (should_fail_load_) {
      if (diagnostic) *diagnostic = "Intentional load failure";
      return nullptr;
    }
    return std::make_shared<MockBackendSession>(backend_type_, protocol_,
                                                concurrency_);
  }

  BackendLoadSpec last_loaded_spec;

 private:
  std::string backend_type_;
  ExecutionProtocol protocol_;
  InferenceConcurrency concurrency_;
  bool should_fail_load_;
};

// Mock Model
class MockEmbeddingModel : public IModel {
 public:
  inline static constexpr char kModelType[] = "mock_bge_embedding";
  inline static constexpr char kCapability[] = "embedding";

  MockEmbeddingModel(std::string model_type, std::string capability,
                     InferenceConcurrency concurrency,
                     nlohmann::json model_config)
      : model_type_(std::move(model_type)),
        capability_(std::move(capability)),
        concurrency_(concurrency),
        model_config_(std::move(model_config)) {}

  const std::string& ModelType() const noexcept override { return model_type_; }
  const std::string& Capability() const noexcept override {
    return capability_;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return concurrency_;
  }
  size_t GetMaxBatchSize() const noexcept override { return 1; }

  const nlohmann::json& ModelConfig() const noexcept { return model_config_; }

 private:
  std::string model_type_;
  std::string capability_;
  InferenceConcurrency concurrency_;
  nlohmann::json model_config_;
};

// Mock Model Bound Node
class MockEmbeddingConsumerNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "MockEmbeddingConsumerNode";

  bool Init(const NodeInitContext& init_ctx) override {
    if (!init_ctx.config || !init_ctx.session_ctx) return false;
    std::string model_id = init_ctx.config->value("bind_model", "");
    model_ = init_ctx.session_ctx->GetModelManager().GetModel<IModel>(model_id);
    return model_ != nullptr;
  }

  int Process(AlgContext*) override { return 0; }

  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }

  std::shared_ptr<IModel> model_;
};

NodeDefinition MakeMockEmbeddingConsumerNodeDef() {
  NodeDefinition def;
  def.node_type = MockEmbeddingConsumerNode::kNodeType;
  def.category = "test";
  def.description = "Test node consuming embedding model";
  def.config_fields = {{"bind_model", ConfigValueKind::kString, true}};
  def.model_capability = "embedding";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(MockEmbeddingConsumerNode,
                              MakeMockEmbeddingConsumerNodeDef());

class ModelBackendPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_backend_create_count = 0;
    g_backend_load_count = 0;
    g_model_create_count = 0;

    if (!BackendRegistry::Instance()
             .Find(MockInferenceBackend::kBackendType)
             .has_value()) {
      BackendDefinition bdef;
      bdef.backend_type = MockInferenceBackend::kBackendType;
      bdef.description = "Mock backend for tests";
      bdef.supported_protocols = {ExecutionProtocol::kTensorGraph};
      bdef.concurrency = InferenceConcurrency::kConcurrent;
      bdef.config_fields = {
          {"device",
           ConfigValueKind::kString,
           false,
           "cpu",
           std::nullopt,
           std::nullopt,
           {"cpu", "cuda"}},
          {"threads", ConfigValueKind::kInteger, false, 4, 1.0, 64.0},
      };
      BackendRegistry::Instance().Register(bdef, []() {
        g_backend_create_count.fetch_add(1);
        return std::make_unique<MockInferenceBackend>();
      });
    }

    if (!ModelRegistry::Instance()
             .Find(MockEmbeddingModel::kModelType)
             .has_value()) {
      ModelDefinition mdef;
      mdef.model_type = MockEmbeddingModel::kModelType;
      mdef.capability = MockEmbeddingModel::kCapability;
      mdef.description = "Mock embedding model for tests";
      mdef.required_protocol = ExecutionProtocol::kTensorGraph;
      mdef.concurrency = InferenceConcurrency::kConcurrent;
      mdef.config_fields = {
          {"max_length", ConfigValueKind::kInteger, false, 512, 1.0, 4096.0},
          {"normalize", ConfigValueKind::kBoolean, false, true},
      };
      ModelRegistry::Instance().Register(
          mdef, [](const ModelCreateContext& ctx, std::string*) {
            g_model_create_count.fetch_add(1);
            return std::make_shared<MockEmbeddingModel>(
                MockEmbeddingModel::kModelType, MockEmbeddingModel::kCapability,
                InferenceConcurrency::kConcurrent, ctx.model_config);
          });
    }
  }

  void TearDown() override {}
};

// 1. ValidateAndNormalizeConfig Unit Tests
TEST_F(ModelBackendPipelineTest, ValidateAndNormalizeConfigSuccessAndDefaults) {
  std::vector<ConfigFieldDefinition> schema = {
      {"str_field", ConfigValueKind::kString, false, "default_str"},
      {"int_field", ConfigValueKind::kInteger, false, 42, 1.0, 100.0},
      {"num_field", ConfigValueKind::kNumber, false, 3.14, 0.0, 10.0},
      {"bool_field", ConfigValueKind::kBoolean, false, true},
      {"enum_field",
       ConfigValueKind::kString,
       false,
       "opt_a",
       std::nullopt,
       std::nullopt,
       {"opt_a", "opt_b"}},
      {"req_field", ConfigValueKind::kString, true},
  };

  nlohmann::json input = {{"req_field", "user_val"}, {"int_field", 50}};

  nlohmann::json normalized;
  std::vector<ValidationDiagnostic> diags;
  bool ok = ValidateAndNormalizeConfig(schema, input, &normalized, &diags,
                                       "/test_config");

  EXPECT_TRUE(ok);
  EXPECT_TRUE(diags.empty());
  EXPECT_EQ(normalized["req_field"], "user_val");
  EXPECT_EQ(normalized["int_field"], 50);
  EXPECT_EQ(normalized["str_field"], "default_str");
  EXPECT_EQ(normalized["num_field"], 3.14);
  EXPECT_EQ(normalized["bool_field"], true);
  EXPECT_EQ(normalized["enum_field"], "opt_a");

  // Verify input was not modified
  EXPECT_FALSE(input.contains("str_field"));
}

TEST_F(ModelBackendPipelineTest, ValidateAndNormalizeConfigUnknownField) {
  std::vector<ConfigFieldDefinition> schema = {
      {"known", ConfigValueKind::kString, false, "default"},
  };

  nlohmann::json input = {{"known", "hello"}, {"unexpected_field", 123}};

  nlohmann::json normalized;
  std::vector<ValidationDiagnostic> diags;
  bool ok = ValidateAndNormalizeConfig(schema, input, &normalized, &diags,
                                       "/test_config");

  EXPECT_FALSE(ok);
  ASSERT_EQ(diags.size(), 1u);
  EXPECT_EQ(diags[0].code, DiagnosticCode::kUnknownConfigField);
  EXPECT_EQ(diags[0].path, "/test_config/unexpected_field");
}

TEST_F(ModelBackendPipelineTest, ValidateAndNormalizeConfigBoundsAndEnum) {
  std::vector<ConfigFieldDefinition> schema = {
      {"int_field", ConfigValueKind::kInteger, false, 10, 5.0, 20.0},
      {"enum_field",
       ConfigValueKind::kString,
       false,
       "opt_a",
       std::nullopt,
       std::nullopt,
       {"opt_a", "opt_b"}},
  };

  // Below min
  {
    nlohmann::json input = {{"int_field", 2}};
    std::vector<ValidationDiagnostic> diags;
    EXPECT_FALSE(
        ValidateAndNormalizeConfig(schema, input, nullptr, &diags, "/base"));
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_EQ(diags[0].code, DiagnosticCode::kConfigFieldRange);
  }

  // Above max
  {
    nlohmann::json input = {{"int_field", 50}};
    std::vector<ValidationDiagnostic> diags;
    EXPECT_FALSE(
        ValidateAndNormalizeConfig(schema, input, nullptr, &diags, "/base"));
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_EQ(diags[0].code, DiagnosticCode::kConfigFieldRange);
  }

  // Invalid enum
  {
    nlohmann::json input = {{"enum_field", "opt_c"}};
    std::vector<ValidationDiagnostic> diags;
    EXPECT_FALSE(
        ValidateAndNormalizeConfig(schema, input, nullptr, &diags, "/base"));
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_EQ(diags[0].code, DiagnosticCode::kConfigFieldEnum);
  }
}

// 2. PipelineValidator and ValidatedModelPlan Tests
TEST_F(ModelBackendPipelineTest, ValidatorProducesModelPlanAndZeroSideEffects) {
  nlohmann::json cfg = {
      {"biz_name", "model_plan_test"},
      {"models", nlohmann::json::array({{
                     {"model_id", "emb_model"},
                     {"capability", "embedding"},
                     {"model_type", "mock_bge_embedding"},
                     {"backend", "mock_test_backend"},
                     {"model_path", "./models/bge/model.onnx"},
                     {"model_config", {{"max_length", 256}}},
                     {"backend_config", {{"device", "cpu"}}},
                 }})},
      {"pipeline", nlohmann::json::array({{
                       {"id", "node1"},
                       {"node_type", "MockEmbeddingConsumerNode"},
                       {"depends_on", nlohmann::json::array()},
                       {"config", {{"bind_model", "emb_model"}}},
                   }})},
  };

  auto plan = PipelineValidator::ValidateAndPlan(
      cfg, ValidationPolicy::kPrivateExtensionCompatible);

  EXPECT_TRUE(plan.report.ok)
      << (plan.report.diagnostics.empty() ? ""
                                          : plan.report.diagnostics[0].message);
  ASSERT_EQ(plan.models.size(), 1u);

  const auto& mp = plan.models[0];
  EXPECT_EQ(mp.model_id, "emb_model");
  EXPECT_EQ(mp.capability, "embedding");
  EXPECT_EQ(mp.model_type, "mock_bge_embedding");
  EXPECT_EQ(mp.backend, "mock_test_backend");
  EXPECT_EQ(mp.resolved_model_path, "models/bge/model.onnx");
  EXPECT_EQ(mp.protocol, ExecutionProtocol::kTensorGraph);
  EXPECT_EQ(mp.effective_concurrency, InferenceConcurrency::kConcurrent);

  // Check normalized configs with injected defaults
  EXPECT_EQ(mp.normalized_model_config["max_length"], 256);
  EXPECT_EQ(mp.normalized_model_config["normalize"], true);  // injected default
  EXPECT_EQ(mp.normalized_backend_config["device"], "cpu");
  EXPECT_EQ(mp.normalized_backend_config["threads"], 4);  // injected default

  // CRITICAL INVARIANT: Validator must NOT call backend create, load, or model
  // create!
  EXPECT_EQ(g_backend_create_count.load(), 0);
  EXPECT_EQ(g_backend_load_count.load(), 0);
  EXPECT_EQ(g_model_create_count.load(), 0);
}

TEST_F(ModelBackendPipelineTest, ValidatorRejectsProtocolMismatch) {
  // Register backend that only supports CausalLm protocol
  BackendDefinition bdef;
  bdef.backend_type = "causal_lm_only_backend";
  bdef.supported_protocols = {ExecutionProtocol::kCausalLm};
  bdef.concurrency = InferenceConcurrency::kConcurrent;
  BackendRegistry::Instance().Register(
      bdef, []() -> std::unique_ptr<IInferenceBackend> {
        return std::make_unique<MockInferenceBackend>(
            "causal_lm_only_backend", ExecutionProtocol::kCausalLm);
      });

  nlohmann::json cfg = {
      {"biz_name", "proto_mismatch_test"},
      {"models",
       nlohmann::json::array({{
           {"model_id", "emb_model"},
           {"capability", "embedding"},
           {"model_type", "mock_bge_embedding"},  // requires kTensorGraph
           {"backend", "causal_lm_only_backend"},
           {"model_path", "./model.bin"},
       }})},
      {"pipeline", nlohmann::json::array({{
                       {"id", "node1"},
                       {"node_type", "MockEmbeddingConsumerNode"},
                       {"depends_on", nlohmann::json::array()},
                       {"config", {{"bind_model", "emb_model"}}},
                   }})},
  };

  auto report = PipelineValidator::Validate(
      cfg, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());

  bool found_proto_mismatch = false;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kBackendProtocolMismatch) {
      found_proto_mismatch = true;
    }
  }
  EXPECT_TRUE(found_proto_mismatch);
  EXPECT_EQ(g_backend_create_count.load(), 0);
}

TEST_F(ModelBackendPipelineTest, ValidatorRejectsCapabilityMismatch) {
  nlohmann::json cfg = {
      {"biz_name", "cap_mismatch_test"},
      {"models",
       nlohmann::json::array({{
           {"model_id", "emb_model"},
           {"capability", "rerank"},              // declared rerank
           {"model_type", "mock_bge_embedding"},  // definition is embedding
           {"backend", "mock_test_backend"},
           {"model_path", "./model.onnx"},
       }})},
      {"pipeline", nlohmann::json::array({{
                       {"id", "node1"},
                       {"node_type", "MockEmbeddingConsumerNode"},
                       {"depends_on", nlohmann::json::array()},
                       {"config", {{"bind_model", "emb_model"}}},
                   }})},
  };

  auto report = PipelineValidator::Validate(
      cfg, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(report.ok);

  bool found_cap_mismatch = false;
  for (const auto& diag : report.diagnostics) {
    if (diag.code == DiagnosticCode::kModelCapabilityMismatch) {
      found_cap_mismatch = true;
    }
  }
  EXPECT_TRUE(found_cap_mismatch);
}

// 3. Pipeline Build and Atomic Materialization Tests
TEST_F(ModelBackendPipelineTest, PipelineBuildMaterializesAndRegistersModel) {
  nlohmann::json cfg = {
      {"biz_name", "pipeline_build_success"},
      {"models", nlohmann::json::array({{
                     {"model_id", "emb_model"},
                     {"capability", "embedding"},
                     {"model_type", "mock_bge_embedding"},
                     {"backend", "mock_test_backend"},
                     {"model_path", "./models/bge/model.onnx"},
                     {"model_config", {{"max_length", 128}}},
                 }})},
      {"pipeline", nlohmann::json::array({{
                       {"id", "node1"},
                       {"node_type", "MockEmbeddingConsumerNode"},
                       {"depends_on", nlohmann::json::array()},
                       {"config", {{"bind_model", "emb_model"}}},
                   }})},
  };

  Pipeline pipeline;
  PipelineDiagnostic diag;
  bool ok = pipeline.BuildFromJson(
      cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible);

  EXPECT_TRUE(ok) << diag.message;
  EXPECT_EQ(diag.code, PipelineErrorCode::kOk);

  // Verify backend and model creation
  EXPECT_EQ(g_backend_create_count.load(), 1);
  EXPECT_EQ(g_backend_load_count.load(), 1);
  EXPECT_EQ(g_model_create_count.load(), 1);

  // Verify model is accessible in session ModelManager
  auto model = pipeline.GetSessionContext().GetModelManager().GetModel<IModel>(
      "emb_model");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->ModelType(), "mock_bge_embedding");
  EXPECT_EQ(model->Capability(), "embedding");

  auto mock_model = std::dynamic_pointer_cast<MockEmbeddingModel>(model);
  ASSERT_NE(mock_model, nullptr);
  EXPECT_EQ(mock_model->ModelConfig()["max_length"], 128);
  EXPECT_EQ(mock_model->ModelConfig()["normalize"], true);  // default injected
}

TEST_F(ModelBackendPipelineTest,
       PipelineBuildAtomicRollbackOnSecondModelFailure) {
  // Register second backend that fails to load
  BackendDefinition bdef_fail;
  bdef_fail.backend_type = "failing_backend";
  bdef_fail.supported_protocols = {ExecutionProtocol::kTensorGraph};
  bdef_fail.concurrency = InferenceConcurrency::kConcurrent;
  BackendRegistry::Instance().Register(
      bdef_fail, []() -> std::unique_ptr<IInferenceBackend> {
        g_backend_create_count.fetch_add(1);
        return std::make_unique<MockInferenceBackend>(
            "failing_backend", ExecutionProtocol::kTensorGraph,
            InferenceConcurrency::kConcurrent,
            /*should_fail_load=*/true);
      });

  nlohmann::json cfg = {
      {"biz_name", "pipeline_atomic_rollback"},
      {"models", nlohmann::json::array({
                     {
                         {"model_id", "good_model"},
                         {"capability", "embedding"},
                         {"model_type", "mock_bge_embedding"},
                         {"backend", "mock_test_backend"},
                         {"model_path", "./models/good.onnx"},
                     },
                     {
                         {"model_id", "bad_model"},
                         {"capability", "embedding"},
                         {"model_type", "mock_bge_embedding"},
                         {"backend", "failing_backend"},
                         {"model_path", "./models/bad.onnx"},
                     },
                 })},
      {"pipeline", nlohmann::json::array({{
                       {"id", "node1"},
                       {"node_type", "MockEmbeddingConsumerNode"},
                       {"depends_on", nlohmann::json::array()},
                       {"config", {{"bind_model", "good_model"}}},
                   }})},
  };

  Pipeline pipeline;
  PipelineDiagnostic diag;
  bool ok = pipeline.BuildFromJson(
      cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible);

  EXPECT_FALSE(ok);
  EXPECT_EQ(diag.code, PipelineErrorCode::kModelMaterializationFailed);

  // Verify that failing_backend truly attempted to load (preventing false
  // positives)
  EXPECT_EQ(g_backend_create_count.load(), 2);
  EXPECT_EQ(g_backend_load_count.load(), 2);
  EXPECT_EQ(g_model_create_count.load(), 1);

  // Verify that good_model is NOT registered in ModelManager due to atomic
  // staging!
  EXPECT_EQ(pipeline.GetSessionContext().GetModelManager().GetModel<IModel>(
                "good_model"),
            nullptr);
  EXPECT_EQ(pipeline.GetSessionContext().GetModelManager().GetModel<IModel>(
                "bad_model"),
            nullptr);
}

TEST_F(ModelBackendPipelineTest, ValidatorRejectsModelPathEscapingRoot) {
  nlohmann::json cfg_escape = {
      {"biz_name", "path_escape_test"},
      {"models", nlohmann::json::array({{
                     {"model_id", "emb_model"},
                     {"capability", "embedding"},
                     {"model_type", "mock_bge_embedding"},
                     {"backend", "mock_test_backend"},
                     {"model_path", "../outside/secret.onnx"},
                 }})},
      {"pipeline", nlohmann::json::array({{
                       {"id", "node1"},
                       {"node_type", "MockEmbeddingConsumerNode"},
                       {"depends_on", nlohmann::json::array()},
                       {"config", {{"bind_model", "emb_model"}}},
                   }})},
  };

  auto report = PipelineValidator::Validate(
      cfg_escape, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(report.ok);
  ASSERT_FALSE(report.diagnostics.empty());

  bool found_escape_diag = false;
  for (const auto& d : report.diagnostics) {
    if (d.code == DiagnosticCode::kFieldRange &&
        d.path == "/models/0/model_path") {
      found_escape_diag = true;
      EXPECT_NE(d.message.find("cannot traverse outside"), std::string::npos);
    }
  }
  EXPECT_TRUE(found_escape_diag);
}

TEST_F(ModelBackendPipelineTest,
       ValidatorUnknownModelAndBackendConfigFieldDiagnostics) {
  nlohmann::json cfg = {
      {"biz_name", "diag_test"},
      {"models", nlohmann::json::array({{
                     {"model_id", "emb_model"},
                     {"capability", "embedding"},
                     {"model_type", "mock_bge_embedding"},
                     {"backend", "mock_test_backend"},
                     {"model_path", "./models/good.onnx"},
                     {"model_config", {{"invalid_model_param", 123}}},
                     {"backend_config", {{"invalid_backend_param", "foo"}}},
                 }})},
      {"pipeline", nlohmann::json::array({{
                       {"id", "node1"},
                       {"node_type", "MockEmbeddingConsumerNode"},
                       {"depends_on", nlohmann::json::array()},
                       {"config", {{"bind_model", "emb_model"}}},
                   }})},
  };

  auto report = PipelineValidator::Validate(
      cfg, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(report.ok);

  bool found_model_cfg_diag = false;
  bool found_backend_cfg_diag = false;
  for (const auto& d : report.diagnostics) {
    if (d.code == DiagnosticCode::kUnknownModelConfigField) {
      found_model_cfg_diag = true;
      EXPECT_EQ(d.path, "/models/0/model_config/invalid_model_param");
      EXPECT_FALSE(d.suggestions.empty());
    }
    if (d.code == DiagnosticCode::kUnknownBackendConfigField) {
      found_backend_cfg_diag = true;
      EXPECT_EQ(d.path, "/models/0/backend_config/invalid_backend_param");
      EXPECT_FALSE(d.suggestions.empty());
    }
  }
  EXPECT_TRUE(found_model_cfg_diag);
  EXPECT_TRUE(found_backend_cfg_diag);
}

TEST_F(ModelBackendPipelineTest, ValidatorResolvesPathWithModelRootDir) {
  nlohmann::json cfg = {
      {"biz_name", "model_root_dir_test"},
      {"models", nlohmann::json::array({{
                     {"model_id", "emb_model"},
                     {"capability", "embedding"},
                     {"model_type", "mock_bge_embedding"},
                     {"backend", "mock_test_backend"},
                     {"model_path", "./models/bge/model.onnx"},
                 }})},
      {"pipeline", nlohmann::json::array({{
                       {"id", "node1"},
                       {"node_type", "MockEmbeddingConsumerNode"},
                       {"depends_on", nlohmann::json::array()},
                       {"config", {{"bind_model", "emb_model"}}},
                   }})},
  };

  // 1. Without model_root_dir (Offline check): normalized relative path
  auto plan_offline = PipelineValidator::ValidateAndPlan(
      cfg, ValidationPolicy::kPrivateExtensionCompatible, "");
  EXPECT_TRUE(plan_offline.report.ok);
  ASSERT_EQ(plan_offline.models.size(), 1u);
  EXPECT_EQ(plan_offline.models[0].resolved_model_path,
            "models/bge/model.onnx");

  // 2. With model_root_dir (Runtime creation): resolved under model_root_dir
  auto plan_runtime = PipelineValidator::ValidateAndPlan(
      cfg, ValidationPolicy::kPrivateExtensionCompatible, "/opt/custom_models");
  EXPECT_TRUE(plan_runtime.report.ok);
  ASSERT_EQ(plan_runtime.models.size(), 1u);
  EXPECT_EQ(plan_runtime.models[0].resolved_model_path,
            "/opt/custom_models/models/bge/model.onnx");
}

TEST_F(ModelBackendPipelineTest, PipelineBuildPassesModelRootDirToBackend) {
  nlohmann::json cfg = {
      {"biz_name", "runtime_root_propagate_test"},
      {"models", nlohmann::json::array({{
                     {"model_id", "emb_model"},
                     {"capability", "embedding"},
                     {"model_type", "mock_bge_embedding"},
                     {"backend", "mock_test_backend"},
                     {"model_path", "weights/bge.onnx"},
                 }})},
      {"pipeline", nlohmann::json::array({{
                       {"id", "node1"},
                       {"node_type", "MockEmbeddingConsumerNode"},
                       {"depends_on", nlohmann::json::array()},
                       {"config", {{"bind_model", "emb_model"}}},
                   }})},
  };

  Pipeline pipeline;
  RuntimeOptions opts;
  opts.model_root_dir = "/deploy/edgeflow_root";
  pipeline.GetSessionContext().SetRuntimeOptions(opts);

  PipelineDiagnostic diag;
  bool ok = pipeline.BuildFromJson(
      cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_TRUE(ok);
  EXPECT_EQ(diag.code, PipelineErrorCode::kOk);

  // Model registration in session carries the root-resolved path
  auto reg =
      pipeline.GetSessionContext().GetModelManager().GetModelRegistration(
          "emb_model");
  ASSERT_TRUE(reg.has_value());
  EXPECT_EQ(reg->resolved_model_path, "/deploy/edgeflow_root/weights/bge.onnx");
}

}  // namespace test_mb
}  // namespace alg_framework
