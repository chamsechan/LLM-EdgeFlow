#include <gtest/gtest.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/common_contracts.h"
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

  static void ResetCounts() { s_load_count = 0; }
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
  return def;
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
  invalid_lifetime_override.inputs = {PortDefinition{
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

TEST(DefinitionSchemaValidationTest, NodeToJsonExportsConstraintsAndCommands) {
  const auto* rerank_def = PipelineCatalog::FindNode("TextRerankNode");
  ASSERT_NE(rerank_def, nullptr);
  auto json = PipelineCatalog::NodeToJson(*rerank_def);
  EXPECT_TRUE(json.contains("port_constraints"));
  EXPECT_TRUE(json["port_constraints"].is_array());
  EXPECT_FALSE(json["port_constraints"].empty());

  const auto* rule_def = PipelineCatalog::FindNode("TextRuleMatchNode");
  ASSERT_NE(rule_def, nullptr);
  auto rule_json = PipelineCatalog::NodeToJson(*rule_def);
  EXPECT_TRUE(rule_json.contains("control_commands"));
  EXPECT_TRUE(rule_json["control_commands"].is_array());
  ASSERT_FALSE(rule_json["control_commands"].empty());
  const auto& rule_payload_schema =
      rule_json["control_commands"][0]["payload_schema"];
  EXPECT_EQ(rule_payload_schema["minProperties"], 1);
  EXPECT_EQ(rule_payload_schema["additionalProperties"], false);

  const auto* embedding_def = PipelineCatalog::FindNode("TextEmbeddingNode");
  ASSERT_NE(embedding_def, nullptr);
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

}  // namespace alg_framework
