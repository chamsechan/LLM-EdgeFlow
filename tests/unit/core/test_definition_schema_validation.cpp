#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "contracts/control_payload.h"
#include "core/common_contracts.h"
#include "core/node_interface.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "engine/backend_interface.h"
#include "engine/backend_registry.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {

class SchemaProbeNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "SchemaProbeNode";
  static inline int s_init_count = 0;
  static inline int s_process_count = 0;

  static void ResetCounts() {
    s_init_count = 0;
    s_process_count = 0;
  }

  bool Init(const NodeInitContext&) override {
    ++s_init_count;
    return true;
  }
  int Process(AlgContext*) override {
    ++s_process_count;
    return 0;
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

NodeDefinition MakeSchemaProbeNodeDefinition() {
  NodeDefinition def;
  def.node_type = SchemaProbeNode::kNodeType;
  def.category = "test";
  def.description = "Schema probe test node";
  def.parallel_safe = true;
  def.config_fields = {
      ConfigFieldDefinition{"req_str", ConfigValueKind::kString,
                            /*required=*/true},
      ConfigFieldDefinition{
          "opt_int", ConfigValueKind::kInteger, /*required=*/false,
          /*default_value=*/10, /*minimum=*/1.0, /*maximum=*/100.0},
      ConfigFieldDefinition{"enum_mode", ConfigValueKind::kString,
                            /*required=*/false,
                            /*default_value=*/"fast", /*minimum=*/std::nullopt,
                            /*maximum=*/std::nullopt,
                            /*enum_values=*/{"fast", "accurate"}},
  };
  return def;
}

REGISTER_NODE_WITH_DEFINITION(SchemaProbeNode, MakeSchemaProbeNodeDefinition());

class ThrowingValidateConfigNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "ThrowingValidateConfigNode";
  static inline bool s_called = false;
  static inline int s_throw_mode = 0;

  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};

NodeDefinition MakeThrowingValidateConfigNodeDefinition() {
  NodeDefinition def;
  def.node_type = ThrowingValidateConfigNode::kNodeType;
  def.category = "test";
  def.parallel_safe = true;
  def.config_fields = {ConfigFieldDefinition{
      "req_num", ConfigValueKind::kInteger, true, 10, 1.0, 100.0}};
  def.validate_config = [](const nlohmann::json&, const auto&, std::string*) {
    ThrowingValidateConfigNode::s_called = true;
    if (ThrowingValidateConfigNode::s_throw_mode == 1) {
      throw std::runtime_error("simulated config validation crash");
    } else if (ThrowingValidateConfigNode::s_throw_mode == 2) {
      throw 42;
    }
    return true;
  };
  return def;
}

REGISTER_NODE_WITH_DEFINITION(ThrowingValidateConfigNode,
                              MakeThrowingValidateConfigNodeDefinition());

class SchemaProbeModel : public IModel {
 public:
  inline static constexpr char kModelType[] = "schema_probe_model";
  static inline int s_create_count = 0;

  static void ResetCounts() { s_create_count = 0; }

  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string*) {
    ++s_create_count;
    return std::make_shared<SchemaProbeModel>();
  }
  size_t GetMaxBatchSize() const noexcept override { return 4; }
  const std::string& ModelType() const noexcept override {
    static const std::string type = kModelType;
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "schema_probe";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
};

ModelDefinition MakeSchemaProbeModelDefinition() {
  ModelDefinition def;
  def.model_type = SchemaProbeModel::kModelType;
  def.capability = "schema_probe";
  def.description = "Schema probe test model";
  def.required_protocol = ExecutionProtocol::kTensorGraph;
  def.concurrency = InferenceConcurrency::kConcurrent;
  return def;
}

class SchemaProbeBackend : public IInferenceBackend {
 public:
  inline static constexpr char kBackendType[] = "schema_probe_backend";
  static inline int s_load_count = 0;
  static inline int s_validate_count = 0;
  static inline nlohmann::json s_validated_config;

  static void ResetCounts() {
    s_load_count = 0;
    s_validate_count = 0;
    s_validated_config = nullptr;
  }
  const std::string& BackendType() const noexcept override {
    static const std::string type = kBackendType;
    return type;
  }
  std::shared_ptr<IBackendSession> Load(const BackendLoadSpec&,
                                        std::string*) noexcept override {
    ++s_load_count;
    return nullptr;
  }
};

BackendDefinition MakeSchemaProbeBackendDefinition() {
  BackendDefinition def;
  def.backend_type = SchemaProbeBackend::kBackendType;
  def.description = "Schema probe test backend";
  def.supported_protocols = {ExecutionProtocol::kTensorGraph};
  def.concurrency = InferenceConcurrency::kConcurrent;
  def.config_fields = {
      ConfigFieldDefinition{
          "device_id", ConfigValueKind::kInteger, /*required=*/false,
          /*default_value=*/0, /*minimum=*/0.0, /*maximum=*/16.0},
      ConfigFieldDefinition{"precision", ConfigValueKind::kString,
                            /*required=*/false,
                            /*default_value=*/"fp16", /*minimum=*/std::nullopt,
                            /*maximum=*/std::nullopt,
                            /*enum_values=*/{"fp16", "fp32", "int8"}},
  };
  def.validate_config = [](const nlohmann::json& config,
                           std::string* diagnostic) {
    ++SchemaProbeBackend::s_validate_count;
    SchemaProbeBackend::s_validated_config = config;
    const int device_id = config.at("device_id").get<int>();
    // Exercise both exception barriers using otherwise valid field values.
    if (device_id == 15) throw std::runtime_error("probe validation exception");
    if (device_id == 16) throw 16;
    if (device_id == 0 && config.at("precision") == "int8") {
      if (diagnostic) *diagnostic = "int8 requires device_id greater than zero";
      return false;
    }
    return true;
  };
  return def;
}

nlohmann::json MakeSchemaProbePipeline(const nlohmann::json& backend_config) {
  return {{"biz_name", "unregistered_test_biz"},
          {"models",
           {{{"model_id", "probe_model"},
             {"capability", "schema_probe"},
             {"model_type", SchemaProbeModel::kModelType},
             {"backend", SchemaProbeBackend::kBackendType},
             {"model_path", "probe.bin"},
             {"model_config", nlohmann::json::object()},
             {"backend_config", backend_config}}}},
          {"pipeline",
           {{{"id", "node_0"},
             {"node_type", SchemaProbeNode::kNodeType},
             {"depends_on", nlohmann::json::array()},
             {"config", {{"req_str", "valid"}}}}}}};
}

REGISTER_MODEL_WITH_DEFINITION(SchemaProbeModel,
                               MakeSchemaProbeModelDefinition());
REGISTER_BACKEND_WITH_DEFINITION(SchemaProbeBackend,
                                 MakeSchemaProbeBackendDefinition());

}  // namespace

TEST(DefinitionSchemaValidationTest, EnforcesRequiredField) {
  nlohmann::json pipeline = {
      {"biz_name", "unregistered_test_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", SchemaProbeNode::kNodeType},
                               {"depends_on", nlohmann::json::array()},
                               {"config", nlohmann::json::object()}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(
      pipeline, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan.report.ok);
  ASSERT_FALSE(plan.report.diagnostics.empty());
  auto it =
      std::find_if(plan.report.diagnostics.begin(),
                   plan.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kMissingConfigField;
                   });
  ASSERT_NE(it, plan.report.diagnostics.end());
  EXPECT_EQ(it->path, "/pipeline/0/config/req_str");
  EXPECT_EQ(it->node_id, "node_0");
}

TEST(DefinitionSchemaValidationTest, EnforcesFieldTypeAndRange) {
  nlohmann::json pipeline = {
      {"biz_name", "unregistered_test_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array(
           {{{"id", "node_0"},
             {"node_type", SchemaProbeNode::kNodeType},
             {"depends_on", nlohmann::json::array()},
             {"config", {{"req_str", "hello"}, {"opt_int", 200}}}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(
      pipeline, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan.report.ok);
  auto it =
      std::find_if(plan.report.diagnostics.begin(),
                   plan.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kConfigFieldRange;
                   });
  ASSERT_NE(it, plan.report.diagnostics.end());
  EXPECT_EQ(it->path, "/pipeline/0/config/opt_int");
}

TEST(DefinitionSchemaValidationTest, EnforcesStringEnumValues) {
  nlohmann::json pipeline = {
      {"biz_name", "unregistered_test_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array(
           {{{"id", "node_0"},
             {"node_type", SchemaProbeNode::kNodeType},
             {"depends_on", nlohmann::json::array()},
             {"config",
              {{"req_str", "hello"}, {"enum_mode", "invalid_choice"}}}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(
      pipeline, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan.report.ok);
  auto it = std::find_if(plan.report.diagnostics.begin(),
                         plan.report.diagnostics.end(), [](const auto& item) {
                           return item.code == DiagnosticCode::kConfigFieldEnum;
                         });
  ASSERT_NE(it, plan.report.diagnostics.end());
  EXPECT_EQ(it->path, "/pipeline/0/config/enum_mode");
}

TEST(DefinitionSchemaValidationTest, EnforcesBackendConfigConstraints) {
  nlohmann::json pipeline = {
      {"biz_name", "unregistered_test_biz"},
      {"models",
       nlohmann::json::array(
           {{{"model_id", "probe_model"},
             {"capability", "schema_probe"},
             {"model_type", SchemaProbeModel::kModelType},
             {"backend", SchemaProbeBackend::kBackendType},
             {"model_path", "probe.bin"},
             {"model_config", nlohmann::json::object()},
             {"backend_config",
              {{"device_id", 999}, {"precision", "invalid_prec"}}}}})},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", SchemaProbeNode::kNodeType},
                               {"depends_on", nlohmann::json::array()},
                               {"config", {{"req_str", "valid"}}}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(
      pipeline, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan.report.ok);

  bool has_range = false;
  bool has_enum = false;
  for (const auto& diag : plan.report.diagnostics) {
    if (diag.code == DiagnosticCode::kConfigFieldRange &&
        diag.path == "/models/0/backend_config/device_id") {
      has_range = true;
    }
    if (diag.code == DiagnosticCode::kConfigFieldEnum &&
        diag.path == "/models/0/backend_config/precision") {
      has_enum = true;
    }
  }
  EXPECT_TRUE(has_range);
  EXPECT_TRUE(has_enum);
}

TEST(DefinitionSchemaValidationTest,
     BackendCallbackReceivesNormalizedDefaults) {
  for (const auto& config :
       {nlohmann::json::object(),
        nlohmann::json{{"device_id", 1}, {"precision", "int8"}}}) {
    SchemaProbeBackend::ResetCounts();
    SchemaProbeModel::ResetCounts();
    SchemaProbeNode::ResetCounts();
    const auto plan = PipelineValidator::ValidateAndPlan(
        MakeSchemaProbePipeline(config),
        ValidationPolicy::kPrivateExtensionCompatible);
    EXPECT_TRUE(plan.report.ok);
    EXPECT_EQ(SchemaProbeBackend::s_validate_count, 1);
    EXPECT_EQ(SchemaProbeBackend::s_validated_config.at("device_id"),
              config.value<int>("device_id", 0));
    EXPECT_EQ(SchemaProbeBackend::s_validated_config.at("precision"),
              config.value<std::string>("precision", "fp16"));
    EXPECT_EQ(SchemaProbeBackend::s_load_count, 0);
    EXPECT_EQ(SchemaProbeModel::s_create_count, 0);
    EXPECT_EQ(SchemaProbeNode::s_init_count, 0);
    EXPECT_EQ(SchemaProbeNode::s_process_count, 0);
  }
}

TEST(DefinitionSchemaValidationTest,
     BackendCombinationFailurePreventsMaterialization) {
  SchemaProbeBackend::ResetCounts();
  SchemaProbeModel::ResetCounts();
  SchemaProbeNode::ResetCounts();
  const auto input = MakeSchemaProbePipeline({{"precision", "int8"}});
  const auto plan = PipelineValidator::ValidateAndPlan(
      input, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan.report.ok);
  const auto diagnostic =
      std::find_if(plan.report.diagnostics.begin(),
                   plan.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kInvalidCombination;
                   });
  ASSERT_NE(diagnostic, plan.report.diagnostics.end());
  EXPECT_EQ(diagnostic->path, "/models/0/backend_config");
  EXPECT_EQ(diagnostic->message, "int8 requires device_id greater than zero");
  EXPECT_EQ(SchemaProbeBackend::s_validate_count, 1);
  EXPECT_EQ(SchemaProbeBackend::s_validated_config.at("device_id"), 0);

  Pipeline pipeline;
  PipelineDiagnostic build_diagnostic;
  EXPECT_FALSE(pipeline.BuildFromJson(
      input, &build_diagnostic, ValidationPolicy::kPrivateExtensionCompatible));
  EXPECT_EQ(pipeline.GetState(), Pipeline::State::kFailed);
  EXPECT_EQ(SchemaProbeBackend::s_load_count, 0);
  EXPECT_EQ(SchemaProbeModel::s_create_count, 0);
  EXPECT_EQ(SchemaProbeNode::s_init_count, 0);
  EXPECT_EQ(SchemaProbeNode::s_process_count, 0);
}

TEST(DefinitionSchemaValidationTest, InvalidBackendFieldsSkipSemanticCallback) {
  for (const auto& config : {nlohmann::json{{"device_id", "wrong"}},
                             nlohmann::json{{"device_id", 17}},
                             nlohmann::json{{"precision", "unknown"}},
                             nlohmann::json{{"undeclared", 1}}}) {
    SCOPED_TRACE(config.dump());
    SchemaProbeBackend::ResetCounts();
    const auto plan = PipelineValidator::ValidateAndPlan(
        MakeSchemaProbePipeline(config),
        ValidationPolicy::kPrivateExtensionCompatible);
    EXPECT_FALSE(plan.report.ok);
    EXPECT_EQ(SchemaProbeBackend::s_validate_count, 0);
    EXPECT_EQ(SchemaProbeBackend::s_load_count, 0);
  }
}

TEST(DefinitionSchemaValidationTest,
     BackendCallbackExceptionsBecomeDiagnostics) {
  for (const int device_id : {15, 16}) {
    SchemaProbeBackend::ResetCounts();
    const auto plan = PipelineValidator::ValidateAndPlan(
        MakeSchemaProbePipeline({{"device_id", device_id}}),
        ValidationPolicy::kPrivateExtensionCompatible);
    EXPECT_FALSE(plan.report.ok);
    EXPECT_EQ(SchemaProbeBackend::s_validate_count, 1);
    const auto diagnostic =
        std::find_if(plan.report.diagnostics.begin(),
                     plan.report.diagnostics.end(), [](const auto& item) {
                       return item.code == DiagnosticCode::kInvalidCombination;
                     });
    ASSERT_NE(diagnostic, plan.report.diagnostics.end());
    EXPECT_EQ(diagnostic->path, "/models/0/backend_config");
    EXPECT_EQ(
        diagnostic->message,
        device_id == 15
            ? "probe validation exception"
            : "Backend configuration validator threw an unknown exception");
    EXPECT_EQ(SchemaProbeBackend::s_load_count, 0);
  }
}

TEST(DefinitionSchemaValidationTest, ValidationFailureHasZeroSideEffects) {
  SchemaProbeNode::ResetCounts();
  SchemaProbeModel::ResetCounts();
  SchemaProbeBackend::ResetCounts();

  nlohmann::json invalid_pipeline = {
      {"biz_name", "unregistered_test_biz"},
      {"models",
       nlohmann::json::array({{{"model_id", "probe_model"},
                               {"capability", "schema_probe"},
                               {"model_type", SchemaProbeModel::kModelType},
                               {"backend", SchemaProbeBackend::kBackendType},
                               {"model_path", "probe.bin"},
                               {"model_config", nlohmann::json::object()},
                               {"backend_config", {{"device_id", -10}}}}})},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", SchemaProbeNode::kNodeType},
                               {"depends_on", nlohmann::json::array()},
                               {"config", {{"req_str", "test"}}}}})}};

  Pipeline pipeline;
  PipelineDiagnostic diag;
  bool built = pipeline.BuildFromJson(
      invalid_pipeline, &diag, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(built);
  EXPECT_EQ(pipeline.GetState(), Pipeline::State::kFailed);

  // 严格零模型加载、零节点初始化、零推理副作用
  EXPECT_EQ(SchemaProbeBackend::s_load_count, 0);
  EXPECT_EQ(SchemaProbeModel::s_create_count, 0);
  EXPECT_EQ(SchemaProbeNode::s_init_count, 0);
  EXPECT_EQ(SchemaProbeNode::s_process_count, 0);
}

TEST(DefinitionSchemaValidationTest, RejectsInvalidDefinitionAtRegistration) {
  // 1. Duplicate field names
  NodeDefinition dup_field_def;
  dup_field_def.node_type = "InvalidDupFieldNode";
  dup_field_def.config_fields = {
      ConfigFieldDefinition{"field_a", ConfigValueKind::kString},
      ConfigFieldDefinition{"field_a", ConfigValueKind::kInteger},
  };
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(dup_field_def));

  // 2. Minimum > Maximum
  NodeDefinition invalid_range_def;
  invalid_range_def.node_type = "InvalidRangeNode";
  invalid_range_def.config_fields = {
      ConfigFieldDefinition{"num", ConfigValueKind::kNumber, false, 5.0, 10.0,
                            1.0},
  };
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(invalid_range_def));

  // 3. Default value kind mismatch
  NodeDefinition default_mismatch_def;
  default_mismatch_def.node_type = "DefaultMismatchNode";
  default_mismatch_def.config_fields = {
      ConfigFieldDefinition{"flag", ConfigValueKind::kBoolean, false,
                            "not_a_bool"},
  };
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(default_mismatch_def));

  // 4. Default value not in enum
  NodeDefinition enum_mismatch_def;
  enum_mismatch_def.node_type = "EnumMismatchNode";
  enum_mismatch_def.config_fields = {
      ConfigFieldDefinition{"mode",
                            ConfigValueKind::kString,
                            false,
                            "unknown_mode",
                            std::nullopt,
                            std::nullopt,
                            {"mode_a", "mode_b"}},
  };
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(enum_mismatch_def));

  // 5. Duplicate enum values
  NodeDefinition dup_enum_def;
  dup_enum_def.node_type = "DupEnumNode";
  dup_enum_def.config_fields = {
      ConfigFieldDefinition{"mode",
                            ConfigValueKind::kString,
                            false,
                            "mode_a",
                            std::nullopt,
                            std::nullopt,
                            {"mode_a", "mode_a"}},
  };
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(dup_enum_def));

  // 6. Non-numeric field carrying minimum/maximum (CR-005)
  NodeDefinition string_range_def;
  string_range_def.node_type = "StringRangeNode";
  string_range_def.config_fields = {
      ConfigFieldDefinition{"str_fld", ConfigValueKind::kString, false, "hello",
                            0.0, 10.0},
  };
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(string_range_def));

  NodeDefinition bool_range_def;
  bool_range_def.node_type = "BoolRangeNode";
  bool_range_def.config_fields = {
      ConfigFieldDefinition{"bool_fld", ConfigValueKind::kBoolean, false, true,
                            0.0, 1.0},
  };
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(bool_range_def));

  // 7. Node declares model_capability without model_config_field (CR-005)
  NodeDefinition missing_model_field_def;
  missing_model_field_def.node_type = "MissingModelFieldNode";
  missing_model_field_def.model_capability = "llm";
  missing_model_field_def.model_config_field = "";
  missing_model_field_def.config_fields = {
      ConfigFieldDefinition{"some_param", ConfigValueKind::kString},
  };
  EXPECT_FALSE(
      PipelineCatalog::RegisterNodeDefinition(missing_model_field_def));

  // 8. Node declares model_capability but field not in config_fields (CR-005)
  NodeDefinition unlisted_model_field_def;
  unlisted_model_field_def.node_type = "UnlistedModelFieldNode";
  unlisted_model_field_def.model_capability = "llm";
  unlisted_model_field_def.model_config_field = "bind_model";
  unlisted_model_field_def.config_fields = {
      ConfigFieldDefinition{"other_param", ConfigValueKind::kString},
  };
  EXPECT_FALSE(
      PipelineCatalog::RegisterNodeDefinition(unlisted_model_field_def));

  // 9. Node declares model_capability but model_config_field is not string
  // (CR-005)
  NodeDefinition nonstring_model_field_def;
  nonstring_model_field_def.node_type = "NonStringModelFieldNode";
  nonstring_model_field_def.model_capability = "llm";
  nonstring_model_field_def.model_config_field = "bind_model";
  nonstring_model_field_def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kInteger},
  };
  EXPECT_FALSE(
      PipelineCatalog::RegisterNodeDefinition(nonstring_model_field_def));

  // 10. Port constraints referencing undeclared ports
  NodeDefinition invalid_constraint_def;
  invalid_constraint_def.node_type = "InvalidConstraintNode";
  invalid_constraint_def.inputs = {
      RequiredInputPort("text", BlackboardKey<TextBatch>{"", "TextBatch"})};
  invalid_constraint_def.port_constraints = {
      PortGroupConstraint(PortConstraintKind::kAtLeastOneOf,
                          std::vector<std::string>{"text", "unknown_port"})};
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(invalid_constraint_def));

  // 11. Invalid control command definition
  NodeDefinition invalid_cmd_def;
  invalid_cmd_def.node_type = "InvalidCmdNode";
  invalid_cmd_def.control_commands = {
      ControlCommandDefinition(0, "invalid_cmd")};  // id <= 0
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(invalid_cmd_def));

  // 12. A dynamic lifetime must reference a declared string enum containing
  // only framework lifetimes.
  NodeDefinition invalid_lifetime_override;
  invalid_lifetime_override.node_type = "InvalidLifetimeOverrideNode";
  invalid_lifetime_override.inputs = {NodePortDefinition{
      "text", "TextBatch", true, "1:1", "preserve", "request", "lifetime"}};
  invalid_lifetime_override.config_fields = {
      ConfigFieldDefinition{"lifetime",
                            ConfigValueKind::kString,
                            false,
                            "forever",
                            std::nullopt,
                            std::nullopt,
                            {"request", "forever"}}};
  EXPECT_FALSE(
      PipelineCatalog::RegisterNodeDefinition(invalid_lifetime_override));
}

TEST(DefinitionSchemaValidationTest,
     ControlIdsRequireExplicitIdenticalSharing) {
  NodeDefinition first;
  first.node_type = "PrivateControlOwner";
  first.control_commands = {
      ControlCommandDefinition(2000000101, "private_update")};
  ASSERT_TRUE(PipelineCatalog::RegisterNodeDefinition(first));
  auto duplicate = first;
  duplicate.node_type = "PrivateControlDuplicate";
  std::string error;
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(duplicate, &error));
  EXPECT_NE(error.find("2000000101"), std::string::npos);
  EXPECT_NE(error.find("PrivateControlOwner"), std::string::npos);

  first.node_type = "SharedControlOwner";
  first.control_commands.front().cmd_id = 2000000102;
  first.control_commands.front().shared_id = true;
  ASSERT_TRUE(PipelineCatalog::RegisterNodeDefinition(first));
  duplicate = first;
  duplicate.node_type = "SharedControlPeer";
  EXPECT_TRUE(PipelineCatalog::RegisterNodeDefinition(duplicate));
  duplicate.node_type = "SharedControlNameMismatch";
  duplicate.control_commands.front().name = "different_semantics";
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(duplicate));
  duplicate.control_commands = first.control_commands;
  duplicate.node_type = "SharedControlSchemaMismatch";
  duplicate.control_commands.front().payload_schema = {{"type", "string"}};
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(duplicate));
  duplicate.control_commands = first.control_commands;
  duplicate.node_type = "SharedControlMissingOptIn";
  duplicate.control_commands.front().shared_id = false;
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(duplicate));

  NodeDefinition invalid;
  invalid.node_type = "NonObjectControlSchema";
  invalid.control_commands = {
      ControlCommandDefinition(2000000103, "invalid", "", false)};
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(invalid));
}

TEST(DefinitionSchemaValidationTest,
     ControlPayloadParsingPreservesOutputOnFailure) {
  const nlohmann::json schema = {
      {"type", "object"},
      {"required", {"values"}},
      {"additionalProperties", false},
      {"properties",
       {{"values", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}};
  nlohmann::json output = {{"old", true}};
  const auto before = output;
  std::string error;
  EXPECT_FALSE(ParseControlPayload("{", schema, &output, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(output, before);
  EXPECT_FALSE(
      ParseControlPayload(R"({"values":[1]})", schema, &output, &error));
  EXPECT_EQ(output, before);
  EXPECT_FALSE(ParseControlPayload(R"({"values":[],"extra":1})", schema,
                                   &output, &error));
  EXPECT_EQ(output, before);
  ASSERT_TRUE(
      ParseControlPayload(R"({"values":["a"]})", schema, &output, &error));
  EXPECT_EQ(output["values"][0], "a");
  EXPECT_TRUE(error.empty());
}

TEST(DefinitionSchemaValidationTest, ControlSchemaRejectsInvalidDeclarations) {
  const std::vector<std::pair<nlohmann::json, std::string>> invalid = {
      {{{"type", "int"}}, "type"},
      {{{"type", {"number", "null"}}}, "type"},
      {{{"minimum", "0"}}, "minimum"},
      {{{"maximum", std::numeric_limits<double>::infinity()}}, "maximum"},
      {{{"minimum", 2}, {"maximum", 1}}, "minimum"},
      {{{"minProperties", -1}}, "minProperties"},
      {{{"minProperties", 1.5}}, "minProperties"},
      {{{"enum", nlohmann::json::array()}}, "enum"},
      {{{"enum", {1, 1}}}, "enum"},
      {{{"required", "name"}}, "required"},
      {{{"required", {1}}}, "required"},
      {{{"required", {"name", "name"}}}, "required"},
      {{{"properties", nlohmann::json::array()}}, "properties"},
      {{{"properties", {{"score", {{"exclusiveMaximum", 1}}}}}}, "score"},
      {{{"items", nlohmann::json::array()}}, "items"},
      {{{"additionalProperties", "false"}}, "additionalProperties"},
      {{{"items", {{"type", "invalid"}}}}, "items"},
      {{{"additionalProperties", {{"pattern", ".*"}}}}, "additionalProperties"},
      {{{"description", false}}, "description"},
      {{{"examples", 1}}, "examples"},
      {{{"readOnly", "true"}}, "readOnly"},
      {{{"oneOf", nlohmann::json::array()}}, "oneOf"}};
  for (const auto& [schema, field] : invalid) {
    SCOPED_TRACE(schema.dump());
    NodeDefinition node;
    node.node_type = "InvalidControlSchemaProbe";
    node.control_commands = {
        ControlCommandDefinition(2000000110, "schema_probe", "", schema)};
    std::string error;
    EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(node, &error));
    EXPECT_NE(error.find(node.node_type), std::string::npos) << error;
    EXPECT_NE(error.find("2000000110"), std::string::npos) << error;
    EXPECT_NE(error.find(field), std::string::npos) << error;
    EXPECT_FALSE(
        ValidateControlPayload(nlohmann::json::object(), schema, &error));
    EXPECT_FALSE(error.empty());
  }
}

TEST(DefinitionSchemaValidationTest,
     ControlSchemaAllowsDocumentaryAnnotations) {
  const nlohmann::json schema = {
      {"type", "object"},
      {"title", "Control parameters"},
      {"description", "Runtime update"},
      {"$comment", "Only fields supplied by the caller are updated"},
      {"default", {{"score", 0.5}}},
      {"examples", {{{"score", 0.8}}}},
      {"deprecated", false},
      {"readOnly", false},
      {"writeOnly", true},
      {"properties",
       {{"score", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}}};
  NodeDefinition node;
  node.node_type = "AnnotatedControlSchemaProbe";
  node.control_commands = {
      ControlCommandDefinition(2000000111, "annotated_update", "", schema)};
  std::string error;
  ASSERT_TRUE(PipelineCatalog::RegisterNodeDefinition(node, &error)) << error;
  nlohmann::json payload;
  ASSERT_TRUE(ParseControlPayload("{}", schema, &payload, &error)) << error;
  EXPECT_EQ(payload, nlohmann::json::object());
}

TEST(DefinitionSchemaValidationTest,
     ControlPayloadEnforcesPublishedNumberBounds) {
  const auto definition = PipelineCatalog::FindNode("TextRuleMatchNode");
  ASSERT_TRUE(definition.has_value());
  ASSERT_FALSE(definition->control_commands.empty());
  const auto& schema = definition->control_commands.front().payload_schema;
  for (const double score : {0.0, 0.5, 1.0}) {
    const nlohmann::json payload = {
        {"rules", {{{"pattern", "VIP"}, {"score", score}}}}};
    std::string error;
    EXPECT_TRUE(ValidateControlPayload(payload, schema, &error)) << error;
  }
  for (const double score : {-0.01, 1.01}) {
    const nlohmann::json payload = {
        {"rules", {{{"pattern", "VIP"}, {"score", score}}}}};
    std::string error;
    EXPECT_FALSE(ValidateControlPayload(payload, schema, &error));
    EXPECT_NE(error.find("rules"), std::string::npos) << error;
    EXPECT_NE(error.find("Array item 0"), std::string::npos) << error;
    EXPECT_NE(error.find("score"), std::string::npos) << error;
    EXPECT_NE(error.find(score < 0 ? "minimum" : "maximum"), std::string::npos)
        << error;
  }
  EXPECT_FALSE(ValidateControlPayload(std::numeric_limits<double>::quiet_NaN(),
                                      {{"type", "number"}}));
  EXPECT_FALSE(ValidateControlPayload(std::numeric_limits<double>::infinity(),
                                      {{"type", "number"}}));
  EXPECT_FALSE(ValidateControlPayload(
      nlohmann::json::object(),
      {{"minProperties", std::numeric_limits<uint64_t>::max()}}));
  EXPECT_TRUE(ValidateControlPayload(
      3, {{"type", "integer"}, {"minimum", 3}, {"maximum", 3}}));
  EXPECT_FALSE(
      ValidateControlPayload(2, {{"type", "integer"}, {"minimum", 3}}));
  EXPECT_FALSE(
      ValidateControlPayload(4, {{"type", "integer"}, {"maximum", 3}}));
}

TEST(DefinitionSchemaValidationTest, NodeToJsonExportsConstraintsAndCommands) {
  const auto rerank_def = PipelineCatalog::FindNode("TextRerankNode");
  ASSERT_TRUE(rerank_def.has_value());
  auto json = PipelineCatalog::NodeToJson(*rerank_def);
  EXPECT_TRUE(json.contains("port_constraints"));
  EXPECT_TRUE(json["port_constraints"].is_array());
  EXPECT_FALSE(json["port_constraints"].empty());

  const auto rule_def = PipelineCatalog::FindNode("TextRuleMatchNode");
  ASSERT_TRUE(rule_def.has_value());
  auto rule_json = PipelineCatalog::NodeToJson(*rule_def);
  EXPECT_TRUE(rule_json.contains("control_commands"));
  EXPECT_TRUE(rule_json["control_commands"].is_array());
  ASSERT_FALSE(rule_json["control_commands"].empty());
  const auto& rule_payload_schema =
      rule_json["control_commands"][0]["payload_schema"];
  EXPECT_EQ(rule_payload_schema["minProperties"], 1);
  EXPECT_EQ(rule_payload_schema["additionalProperties"], false);

  const auto embedding_def = PipelineCatalog::FindNode("TextEmbeddingNode");
  ASSERT_TRUE(embedding_def.has_value());
  auto embedding_json = PipelineCatalog::NodeToJson(*embedding_def);
  ASSERT_FALSE(embedding_json["inputs"].empty());
  EXPECT_EQ(embedding_json["inputs"][0]["lifetime_config_field"], "lifetime");
}

TEST(DefinitionSchemaValidationTest, ProductionCatalogSelfCheck) {
  const auto& nodes = PipelineCatalog::Nodes();
  EXPECT_FALSE(nodes.empty());
  for (const auto& node : nodes) {
    EXPECT_FALSE(node.node_type.empty());
    std::unordered_set<std::string> seen_names;
    for (const auto& field : node.config_fields) {
      EXPECT_FALSE(field.name.empty());
      EXPECT_TRUE(seen_names.insert(field.name).second)
          << "Duplicate config field '" << field.name << "' in node "
          << node.node_type;
      if (field.minimum && field.maximum) {
        EXPECT_LE(*field.minimum, *field.maximum)
            << "Inverted range in node " << node.node_type << "." << field.name;
      }
    }
  }

  const auto backends = PipelineCatalog::Backends();
  EXPECT_FALSE(backends.empty());
  for (const auto& backend : backends) {
    EXPECT_FALSE(backend.backend_type.empty());
    std::unordered_set<std::string> seen_names;
    for (const auto& field : backend.config_fields) {
      EXPECT_FALSE(field.name.empty());
      EXPECT_TRUE(seen_names.insert(field.name).second)
          << "Duplicate config field '" << field.name << "' in backend "
          << backend.backend_type;
      if (field.minimum && field.maximum) {
        EXPECT_LE(*field.minimum, *field.maximum)
            << "Inverted range in backend " << backend.backend_type << "."
            << field.name;
      }
    }
  }
}

TEST(DefinitionSchemaValidationTest, RejectsInvalidNodePortDefinitions) {
  // Empty key
  NodeDefinition empty_key_node;
  empty_key_node.node_type = "EmptyKeyPortNode";
  empty_key_node.inputs = {
      NodePortDefinition{"", "TextBatch", true, "1:1", "preserve", "request"}};
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(empty_key_node));

  // Empty type_id
  NodeDefinition empty_type_node;
  empty_type_node.node_type = "EmptyTypePortNode";
  empty_type_node.inputs = {
      NodePortDefinition{"text", "", true, "1:1", "preserve", "request"}};
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(empty_type_node));

  // Invalid cardinality
  NodeDefinition invalid_card_node;
  invalid_card_node.node_type = "InvalidCardPortNode";
  invalid_card_node.inputs = {NodePortDefinition{"text", "TextBatch", true,
                                                 "3:3", "preserve", "request"}};
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(invalid_card_node));

  // Invalid provenance
  NodeDefinition invalid_prov_node;
  invalid_prov_node.node_type = "InvalidProvPortNode";
  invalid_prov_node.inputs = {
      NodePortDefinition{"text", "TextBatch", true, "1:1", "magic", "request"}};
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(invalid_prov_node));

  // Invalid lifetime
  NodeDefinition invalid_life_node;
  invalid_life_node.node_type = "InvalidLifePortNode";
  invalid_life_node.inputs = {NodePortDefinition{"text", "TextBatch", true,
                                                 "1:1", "preserve", "eternal"}};
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(invalid_life_node));

  // Duplicate input port key
  NodeDefinition dup_key_node;
  dup_key_node.node_type = "DupKeyPortNode";
  dup_key_node.inputs = {NodePortDefinition{"text", "TextBatch", true, "1:1",
                                            "preserve", "request"},
                         NodePortDefinition{"text", "TextBatch", false, "1:1",
                                            "preserve", "request"}};
  EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(dup_key_node));

  // Biz definition with invalid port
  BizDefinition invalid_biz;
  invalid_biz.biz_name = "invalid_port_biz";
  invalid_biz.ingress = {
      BizPortDefinition{"", "TextBatch", true, "1:1", "preserve", "request"}};
  EXPECT_FALSE(PipelineCatalog::RegisterBizDefinition(invalid_biz));
}

TEST(DefinitionSchemaValidationTest, RejectsNonIntegerFloatsForIntegerField) {
  nlohmann::json pipeline = {
      {"biz_name", "unregistered_test_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array(
           {{{"id", "node_0"},
             {"node_type", SchemaProbeNode::kNodeType},
             {"depends_on", nlohmann::json::array()},
             {"config", {{"req_str", "hello"}, {"opt_int", 20.5}}}}})}};

  auto plan = PipelineValidator::ValidateAndPlan(
      pipeline, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan.report.ok);
  auto it = std::find_if(plan.report.diagnostics.begin(),
                         plan.report.diagnostics.end(), [](const auto& item) {
                           return item.code == DiagnosticCode::kConfigFieldType;
                         });
  ASSERT_NE(it, plan.report.diagnostics.end());
  EXPECT_EQ(it->path, "/pipeline/0/config/opt_int");
}

TEST(DefinitionSchemaValidationTest,
     ValidateConfigExceptionMappingAndShortCircuit) {
  // Case 1: Field validation fails -> validate_config must NOT be called
  ThrowingValidateConfigNode::s_called = false;
  ThrowingValidateConfigNode::s_throw_mode = 1;
  nlohmann::json pipeline_field_fail = {
      {"biz_name", "unregistered_test_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", "ThrowingValidateConfigNode"},
                               {"depends_on", nlohmann::json::array()},
                               {"config", {{"req_num", "not_an_int"}}}}})}};
  auto plan1 = PipelineValidator::ValidateAndPlan(
      pipeline_field_fail, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan1.report.ok);
  EXPECT_FALSE(ThrowingValidateConfigNode::s_called);

  // Case 2: Field validation passes, validate_config throws std::runtime_error
  // -> mapped to kInvalidCombination
  ThrowingValidateConfigNode::s_called = false;
  ThrowingValidateConfigNode::s_throw_mode = 1;
  nlohmann::json pipeline_std_throw = {
      {"biz_name", "unregistered_test_biz"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", "ThrowingValidateConfigNode"},
                               {"depends_on", nlohmann::json::array()},
                               {"config", {{"req_num", 50}}}}})}};
  auto plan2 = PipelineValidator::ValidateAndPlan(
      pipeline_std_throw, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan2.report.ok);
  EXPECT_TRUE(ThrowingValidateConfigNode::s_called);
  auto it2 =
      std::find_if(plan2.report.diagnostics.begin(),
                   plan2.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kInvalidCombination;
                   });
  ASSERT_NE(it2, plan2.report.diagnostics.end());
  EXPECT_NE(it2->message.find("simulated config validation crash"),
            std::string::npos);

  // Case 3: Field validation passes, validate_config throws non-std exception
  // -> mapped to kInvalidCombination
  ThrowingValidateConfigNode::s_called = false;
  ThrowingValidateConfigNode::s_throw_mode = 2;
  auto plan3 = PipelineValidator::ValidateAndPlan(
      pipeline_std_throw, ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_FALSE(plan3.report.ok);
  EXPECT_TRUE(ThrowingValidateConfigNode::s_called);
  auto it3 =
      std::find_if(plan3.report.diagnostics.begin(),
                   plan3.report.diagnostics.end(), [](const auto& item) {
                     return item.code == DiagnosticCode::kInvalidCombination;
                   });
  ASSERT_NE(it3, plan3.report.diagnostics.end());
  EXPECT_NE(it3->message.find("unknown exception"), std::string::npos);
}

TEST(DefinitionSchemaValidationTest, IntegerBoundsDoNotRoundThroughDouble) {
  const std::vector<ConfigFieldDefinition> fields = {
      {"value", ConfigValueKind::kInteger, true, nullptr, -9007199254740992.0,
       9007199254740992.0}};
  nlohmann::json normalized;
  for (const nlohmann::json& value :
       {nlohmann::json(int64_t{9007199254740993}),
        nlohmann::json(uint64_t{9007199254740993}),
        nlohmann::json(int64_t{-9007199254740993})}) {
    EXPECT_FALSE(ValidateAndNormalizeFields(fields, {{"value", value}},
                                            &normalized, nullptr));
    EXPECT_FALSE(ValidateAndNormalizeConfig(fields, {{"value", value}},
                                            &normalized, nullptr));
  }
  EXPECT_TRUE(ValidateAndNormalizeFields(
      fields, {{"value", int64_t{9007199254740992}}}, &normalized, nullptr));
}

TEST(DefinitionSchemaValidationTest,
     IntegerBoundsHandleLimitsAndFractionalBounds) {
  constexpr double kSignedLimit = 9223372036854775808.0;
  constexpr double kUnsignedLimit = 18446744073709551616.0;
  const auto signed_max = nlohmann::json(std::numeric_limits<int64_t>::max());
  const auto signed_min = nlohmann::json(std::numeric_limits<int64_t>::min());
  const auto unsigned_max =
      nlohmann::json(std::numeric_limits<uint64_t>::max());
  struct Case {
    nlohmann::json value;
    std::optional<double> minimum;
    std::optional<double> maximum;
    bool valid;
  };
  const std::vector<Case> cases = {
      {signed_max, kSignedLimit, std::nullopt, false},
      {unsigned_max, kUnsignedLimit, std::nullopt, false},
      {signed_max, std::nextafter(kSignedLimit, 0.0), kSignedLimit, true},
      {unsigned_max, std::nextafter(kUnsignedLimit, 0.0), kUnsignedLimit, true},
      {signed_max, std::nullopt, std::nextafter(kSignedLimit, 0.0), false},
      {unsigned_max, std::nullopt, std::nextafter(kUnsignedLimit, 0.0), false},
      {signed_min, -kSignedLimit, -kSignedLimit, true},
      {signed_min, std::nextafter(-kSignedLimit, 0.0), std::nullopt, false},
      {signed_min, std::nullopt,
       std::nextafter(-kSignedLimit, -std::numeric_limits<double>::infinity()),
       false},
      {uint64_t{0}, -0.5, 0.5, true},
      {uint64_t{0}, 0.5, std::nullopt, false},
      {uint64_t{0}, std::nullopt, -0.5, false},
      {int64_t{-2}, -1.5, std::nullopt, false},
      {int64_t{-1}, -1.5, std::nullopt, true},
      {int64_t{-1}, std::nullopt, -1.5, false},
      {int64_t{-2}, std::nullopt, -1.5, true},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(nlohmann::json({{"value", item.value},
                                 {"minimum", item.minimum.value_or(0.0)},
                                 {"maximum", item.maximum.value_or(0.0)}})
                     .dump());
    std::vector<ConfigFieldDefinition> fields = {
        {"value", ConfigValueKind::kInteger, true, nullptr, item.minimum,
         item.maximum}};
    nlohmann::json normalized;
    EXPECT_EQ(ValidateAndNormalizeFields(fields, {{"value", item.value}},
                                         &normalized, nullptr),
              item.valid);
    fields.front().default_value = item.value;
    std::string error;
    EXPECT_EQ(ValidateConfigFieldDefinitions(fields, &error), item.valid)
        << error;
  }
}

TEST(DefinitionSchemaValidationTest,
     PortNamesKeepTheirRolesAndCatalogSpelling) {
  static_assert(!std::is_convertible_v<NodePortDefinition, BizPortDefinition>);
  static_assert(!std::is_convertible_v<BizPortDefinition, NodePortDefinition>);
  const BlackboardKey<TextBatch> key{"request_text", "TextBatch"};
  const auto input = RequiredInputPort("text", key);
  const auto ingress = RequiredBizInput(key);
  EXPECT_EQ(input.logical_name, "text");
  EXPECT_EQ(ingress.blackboard_key, "request_text");
  NodeDefinition node;
  node.node_type = "PortNamingProbe";
  node.inputs = {input};
  const auto json = PipelineCatalog::NodeToJson(node);
  EXPECT_EQ(json["inputs"][0]["key"], "text");
  EXPECT_FALSE(json["inputs"][0].contains("logical_name"));
}

TEST(DefinitionSchemaValidationTest, NodeAndBizRejectEmptyFlowMetadata) {
  for (int field = 0; field < 3; ++field) {
    NodePortDefinition port{"value", "TextBatch", true,
                            "1:1",   "preserve",  "request"};
    if (field == 0) port.cardinality.clear();
    if (field == 1) port.provenance_policy.clear();
    if (field == 2) port.lifetime.clear();
    NodeDefinition node;
    node.node_type = "invalid_empty_flow_node";
    node.outputs = {port};
    BizDefinition biz;
    biz.biz_name = "invalid_empty_flow_biz";
    BizPortDefinition biz_port{"value", "TextBatch"};
    static_cast<PortContract&>(biz_port) = port;
    biz.egress = {biz_port};
    EXPECT_FALSE(PipelineCatalog::RegisterNodeDefinition(node));
    EXPECT_FALSE(PipelineCatalog::RegisterBizDefinition(biz));
  }
}

}  // namespace llm_edgeflow
