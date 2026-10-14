#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "contracts/parameters.h"
#include "core/node_registry.h"
#include "core/pipeline_validator.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

struct PlanningParameters {
  int limit = 0;
  std::string model;
};

NodeDefinition RelayDefinition(const std::string& type) {
  NodeDefinition definition;
  definition.node_type = type;
  definition.parallel_safe = true;
  definition.inputs = {{"text", "text"}};
  definition.outputs = {{"text", "text"}, {"unused", "text"}};
  definition.config_fields =
      Parameters<PlanningParameters>{
          Field("limit", &PlanningParameters::limit).Default(4).Minimum(1)}
          .Fields();
  return definition;
}

Json Node(const std::string& name, const std::string& type = "planning_relay",
          const std::string& source = "input.text") {
  return {{"name", name}, {"type", type}, {"inputs", {{"text", source}}}};
}

Json Pipeline(std::initializer_list<Json> nodes, size_t workers = 1) {
  auto pipeline = Json::array();
  for (const auto& node : nodes) pipeline.push_back(node);
  return {{"max_parallel_workers", workers}, {"pipeline", std::move(pipeline)}};
}

Json Model(const std::string& name = "generator",
           const std::string& backend = "planning_concurrent_backend",
           const std::string& category = "planning_generation") {
  return {{"type", category},
          {"name", name},
          {"file", "unresolved/model.bin"},
          {"backend", {{"type", backend}}}};
}

PipelineIoBoundary Boundary(const std::string& output = "relay.text") {
  PipelineIoBoundary boundary;
  boundary.input_published_ports = {{"input.text", "text"},
                                    {"input.unused", "text"}};
  IoPortDefinition result(output, "text");
  result.logical_name = "result";
  result.path = "/io/output/0/inputs";
  result.has_binding = !output.empty();
  boundary.output_consumed_ports.push_back(result);
  return boundary;
}

const ValidationDiagnostic* FindDiagnostic(const ValidationReport& report,
                                           int code, const std::string& path) {
  const auto found = std::find_if(
      report.diagnostics.begin(), report.diagnostics.end(),
      [&](const auto& item) { return item.code == code && item.path == path; });
  return found == report.diagnostics.end() ? nullptr : &*found;
}

void ExpectError(const ValidatedPipelinePlan& plan, int code,
                 const std::string& path) {
  EXPECT_FALSE(plan.report.ok);
  EXPECT_NE(FindDiagnostic(plan.report, code, path), nullptr)
      << plan.report.ToJson().dump(2);
}

class PipelinePlanningTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    for (const auto* type :
         {"planning_concurrent_backend", "planning_serial_backend",
          "planning_incompatible_backend"}) {
      BackendDefinition backend;
      backend.backend_type = type;
      backend.supported_protocols = {std::string(type) ==
                                             "planning_incompatible_backend"
                                         ? ExecutionProtocol::kTextGeneration
                                         : ExecutionProtocol::kFixture};
      backend.concurrency = std::string(type) == "planning_serial_backend"
                                ? InferenceConcurrency::kSerialized
                                : InferenceConcurrency::kConcurrent;
      backend.params = Parameters<PlanningParameters>{
          Field("limit", &PlanningParameters::limit).Default(8)};
      ASSERT_TRUE(BackendRegistry::Instance().Register(
          backend, []() -> std::unique_ptr<IInferenceBackend> {
            ADD_FAILURE() << "Preflight must not create a backend";
            return nullptr;
          }));
    }
    for (const auto* category :
         {"planning_generation", "planning_serial_model"}) {
      ModelDefinition model;
      model.model_type = category;
      model.impl_name = std::string(category) + "_impl";
      model.required_protocol = ExecutionProtocol::kFixture;
      model.fixture_backends = {"planning_concurrent_backend",
                                "planning_serial_backend"};
      model.concurrency = std::string(category) == "planning_serial_model"
                              ? InferenceConcurrency::kSerialized
                              : InferenceConcurrency::kConcurrent;
      model.params = Parameters<PlanningParameters>{
          Field("limit", &PlanningParameters::limit).Default(16)};
      ASSERT_TRUE(ModelRegistry::Instance().Register(
          model,
          [](const ModelCreateContext&,
             std::string*) -> std::shared_ptr<IModel> {
            ADD_FAILURE() << "Preflight must not create a model";
            return nullptr;
          }));
    }
  }

  void SetUp() override {
    test_support::RegistryTestAccess::ResetNodes();
    Register(RelayDefinition("planning_relay"));
    auto split = RelayDefinition("planning_split");
    split.outputs = {{"text", "text", true, "1:N", "generate_sub_id"}};
    Register(split);
    auto collect = RelayDefinition("planning_collect");
    collect.inputs[0].cardinality = "N:1";
    collect.outputs = {{"text", "text", true, "N:1", "aggregate"}};
    Register(collect);
    auto join = RelayDefinition("planning_join");
    join.inputs = {{"left", "text"}, {"right", "text"}};
    Register(join);
    auto model_user = RelayDefinition("planning_model_user");
    model_user.config_fields.push_back(Parameters<PlanningParameters>{
        Field("model", &PlanningParameters::model).Required()}
                                           .Fields()[0]);
    model_user.model_dependencies = {
        {"generator", "planning_generation", "model"}};
    Register(model_user);
  }

  void Register(const NodeDefinition& definition) {
    ASSERT_TRUE(NodeRegistry::Instance().Register(
        definition.node_type,
        []() -> std::unique_ptr<INode> {
          ADD_FAILURE() << "Preflight must not create a node";
          return nullptr;
        },
        definition));
  }

  Json ModelPipeline(size_t workers = 1) {
    auto root = Pipeline({Node("relay", "planning_model_user")}, workers);
    root["pipeline"][0]["params"] = {{"model", "generator"}};
    root["models"] = Json::array({Model()});
    return root;
  }

  // Every test restores the real catalog; probe creators never execute.
  test_support::RegistryTestAccess::ScopedNodeState nodes_;
};

TEST_F(PipelinePlanningTest, ProducesNormalizedBindingsAndReferencedPortsOnly) {
  const auto root = ModelPipeline();
  const auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  EXPECT_EQ(plan.report.topological_order, std::vector<std::string>{"relay"});
  EXPECT_EQ(plan.report.topological_layers,
            (std::vector<std::vector<std::string>>{{"relay"}}));
  ASSERT_EQ(plan.input_ports.size(), 1u);
  EXPECT_EQ(plan.input_ports[0].blackboard_key, "input.text");
  ASSERT_EQ(plan.models.size(), 1u);
  const auto& model = plan.models[0];
  EXPECT_EQ(model.model_name, "generator");
  EXPECT_EQ(model.impl_name, "planning_generation_impl");
  EXPECT_EQ(model.backend_type, "planning_concurrent_backend");
  EXPECT_EQ(model.protocol, ExecutionProtocol::kFixture);
  EXPECT_EQ(model.effective_concurrency, InferenceConcurrency::kConcurrent);
  EXPECT_EQ(model.model_file, "unresolved/model.bin");
  EXPECT_EQ(model.model_params, Json({{"limit", 16}}));
  EXPECT_EQ(model.backend_params, Json({{"limit", 8}}));
  const auto& node = plan.node_plans.at("relay");
  EXPECT_EQ(node.normalized_params,
            Json({{"model", "generator"}, {"limit", 4}}));
  const auto* binding = node.FindModelBinding("generator");
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->config_field, "model");
  EXPECT_EQ(binding->model_type, "planning_generation");
  EXPECT_EQ(binding->model_name, "generator");
  ASSERT_EQ(node.ports.size(), 2u);
  ASSERT_NE(node.FindPort("text"), nullptr);
  EXPECT_EQ(node.FindPort("text")->blackboard_key, "input.text");
  ASSERT_NE(node.FindPort("text", PortDirection::kOutput), nullptr);
  EXPECT_EQ(node.FindPort("text", PortDirection::kOutput)->blackboard_key,
            "relay.text");
  EXPECT_EQ(node.FindPort("unused", PortDirection::kOutput), nullptr);
  EXPECT_FALSE(root["pipeline"][0]["params"].contains("limit"));
}

TEST_F(PipelinePlanningTest,
       ParsedEntryPointPreservesSourceLocationsAndValues) {
  auto root = Pipeline(
      {Node("producer"), Node("relay", "planning_relay", "producer.text")});
  root["pipeline"][0]["params"] = {{"limit", 9}};
  ParsedPipelineConfig parsed;
  ASSERT_TRUE(ParsePipelineConfig(root, &parsed));
  const auto plan =
      PipelineValidator::ValidateParsedAndPlan(std::move(parsed), Boundary());
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  EXPECT_EQ(plan.node_plans.at("producer").normalized_params["limit"], 9);
  EXPECT_EQ(plan.node_plans.at("relay").node.source_index, 1u);
  EXPECT_EQ(plan.config.nodes[1].depends_on,
            std::vector<std::string>{"producer"});
}

TEST_F(PipelinePlanningTest, PropagatesScalarFieldFailureAtNodeParameterPath) {
  auto root = Pipeline({Node("relay")});
  root["pipeline"][0]["params"] = {{"limit", 0}};
  const auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
  ExpectError(plan, error::kConfigFieldRange, "/pipeline/0/params/limit");
  const auto* error = FindDiagnostic(plan.report, error::kConfigFieldRange,
                                     "/pipeline/0/params/limit");
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->node_name, "relay");
}

TEST_F(PipelinePlanningTest, PropagatesParserFailureBeforeBuildingAnyPlan) {
  const auto plan = PipelineValidator::ValidateAndPlan(
      {{"pipeline", {{{"type", "planning_relay"}}}}}, Boundary());
  ExpectError(plan, error::kMissingField, "/pipeline/0/name");
  EXPECT_TRUE(plan.node_plans.empty());
  EXPECT_TRUE(plan.models.empty());
  EXPECT_TRUE(plan.report.topological_order.empty());
}

TEST_F(PipelinePlanningTest,
       SelectsModelAndBackendBeforeValidatingTheirParameters) {
  struct Case {
    const char* name;
    const char* path;
    Json value;
    int code;
  };
  const std::vector<Case> cases = {
      {"unknown category", "/models/0/type", "absent_category",
       error::kUnknownModelType},
      {"unknown backend", "/models/0/backend/type", "absent_backend",
       error::kUnknownBackend},
      {"incompatible protocol", "/models/0/backend/type",
       "planning_incompatible_backend", error::kBackendProtocolMismatch},
      {"unknown reference", "/pipeline/0/params/model", "absent_instance",
       error::kUnknownModelReference},
      {"wrong capability", "/models/0/type", "planning_serial_model",
       error::kModelTypeMismatch},
  };
  for (const auto& example : cases) {
    SCOPED_TRACE(example.name);
    auto root = ModelPipeline();
    root[Json::json_pointer(example.path)] = example.value;
    const auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
    ExpectError(plan, example.code,
                example.code == error::kModelTypeMismatch
                    ? "/pipeline/0/params/model"
                    : example.path);
  }
  auto root = ModelPipeline();
  root["models"][0]["backend"]["type"] = "absent_backend";
  root["models"][0]["params"] = {{"unknown", 1}};
  const auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
  ExpectError(plan, error::kUnknownBackend, "/models/0/backend/type");
  EXPECT_EQ(FindDiagnostic(plan.report, error::kUnknownConfigField,
                           "/models/0/params/unknown"),
            nullptr);
}

TEST_F(PipelinePlanningTest, ModelReferenceIsRequiredAtItsDeclaredConfigField) {
  auto root = ModelPipeline();
  root["pipeline"][0]["params"].erase("model");
  ExpectError(PipelineValidator::ValidateAndPlan(root, Boundary()),
              error::kMissingConfigField, "/pipeline/0/params/model");
}

TEST_F(PipelinePlanningTest, NormalizesExplicitModelAndBackendValues) {
  auto root = ModelPipeline();
  root["models"][0]["params"] = {{"limit", 23}};
  root["models"][0]["backend"]["params"] = {{"limit", 11}};
  auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  ASSERT_EQ(plan.models.size(), 1u);
  EXPECT_EQ(plan.models[0].model_params["limit"], 23);
  EXPECT_EQ(plan.models[0].backend_params["limit"], 11);
  root["models"][0]["backend"]["params"] = {{"limit", "eleven"}};
  plan = PipelineValidator::ValidateAndPlan(root, Boundary());
  ExpectError(plan, error::kConfigFieldType, "/models/0/backend/params/limit");
}

TEST_F(PipelinePlanningTest,
       RejectsUnusedModelsButSuppressesUnreliableBindingAnalysis) {
  auto root = Pipeline({Node("relay")});
  root["models"] = Json::array({Model()});
  ExpectError(PipelineValidator::ValidateAndPlan(root, Boundary()),
              error::kUnusedModel, "/models/0/name");
  root["pipeline"][0]["type"] = "absent_node";
  const auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
  ExpectError(plan, error::kUnknownNodeType, "/pipeline/0/type");
  EXPECT_EQ(FindDiagnostic(plan.report, error::kUnusedModel, "/models/0/name"),
            nullptr);
}

TEST_F(PipelinePlanningTest,
       RejectsAmbiguousCompatibleImplementationsInIsolatedProcess) {
  // ModelRegistry is process-lived. Keep this deliberately invalid catalog in a
  // child.
  EXPECT_EXIT(
      {
        ModelDefinition duplicate =
            *ModelRegistry::Instance().Find("planning_generation_impl");
        duplicate.impl_name = "planning_duplicate_impl";
        if (!ModelRegistry::Instance().Register(
                duplicate,
                [](const ModelCreateContext&, std::string*)
                    -> std::shared_ptr<IModel> { return nullptr; }))
          std::_Exit(2);
        const auto plan =
            PipelineValidator::ValidateAndPlan(ModelPipeline(), Boundary());
        std::_Exit(!plan.report.ok &&
                           FindDiagnostic(plan.report, error::kRegistryConflict,
                                          "/models/0/backend/type")
                       ? 0
                       : 1);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST_F(PipelinePlanningTest, DataAndOrderOnlyDependenciesFormOneUnion) {
  auto root = Pipeline({Node("relay", "planning_relay", "producer.text"),
                        Node("producer"), Node("order_only")});
  root["pipeline"][0]["depends_on"] = {"order_only", "producer"};
  const auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  EXPECT_EQ(plan.report.topological_layers,
            (std::vector<std::vector<std::string>>{{"producer", "order_only"},
                                                   {"relay"}}));
  EXPECT_EQ(plan.config.nodes[0].depends_on,
            (std::vector<std::string>{"order_only", "producer"}));
  EXPECT_EQ(plan.node_plans.at("relay").FindPort("text")->blackboard_key,
            "producer.text");
  EXPECT_EQ(
      plan.node_plans.at("order_only").FindPort("text", PortDirection::kOutput),
      nullptr);
}

TEST_F(PipelinePlanningTest,
       ArrayOrderCreatesNoDependencyAndBindingsDetermineOrder) {
  const auto independent = PipelineValidator::ValidateAndPlan(
      Pipeline({Node("later"), Node("earlier")}), Boundary("earlier.text"));
  ASSERT_TRUE(independent.report.ok);
  EXPECT_EQ(independent.report.topological_layers,
            (std::vector<std::vector<std::string>>{{"later", "earlier"}}));
  const auto linked = PipelineValidator::ValidateAndPlan(
      Pipeline(
          {Node("relay", "planning_relay", "producer.text"), Node("producer")}),
      Boundary());
  ASSERT_TRUE(linked.report.ok);
  EXPECT_EQ(linked.report.topological_order,
            (std::vector<std::string>{"producer", "relay"}));
  EXPECT_EQ(linked.config.nodes[0].depends_on,
            std::vector<std::string>{"producer"});
}

TEST_F(PipelinePlanningTest, RejectsInvalidOrderingAndDataCycles) {
  struct Case {
    const char* name;
    Json dependencies;
    int code;
    const char* path;
  };
  const std::vector<Case> cases = {
      {"unknown dependency",
       {"absent"},
       error::kInvalidDependency,
       "/pipeline/0/depends_on/0"},
      {"self dependency",
       {"relay"},
       error::kDagCycle,
       "/pipeline/0/depends_on/0"},
      {"duplicate dependency",
       {"producer", "producer"},
       error::kDuplicateDependency,
       "/pipeline/0/depends_on/1"},
  };
  for (const auto& example : cases) {
    SCOPED_TRACE(example.name);
    auto root = Pipeline({Node("relay"), Node("producer")});
    root["pipeline"][0]["depends_on"] = example.dependencies;
    ExpectError(PipelineValidator::ValidateAndPlan(root, Boundary()),
                example.code, example.path);
  }
  auto cycle = Pipeline({Node("relay", "planning_relay", "producer.text"),
                         Node("producer", "planning_relay", "relay.text")});
  ExpectError(PipelineValidator::ValidateAndPlan(cycle, Boundary()),
              error::kDagCycle, "/pipeline");
  auto self = Pipeline({Node("relay", "planning_relay", "relay.text")});
  ExpectError(PipelineValidator::ValidateAndPlan(self, Boundary()),
              error::kDagCycle, "/pipeline");
}

TEST_F(PipelinePlanningTest, RequiresExplicitInputAndStrictSourceReferences) {
  struct Case {
    const char* source;
    int code;
  };
  for (const auto& example :
       std::vector<Case>{{"", error::kFieldType},
                         {"text", error::kFieldType},
                         {".text", error::kFieldType},
                         {"input.", error::kFieldType},
                         {"input.text.more", error::kFieldType},
                         {"missing.text", error::kUnknownNodeReference},
                         {"input.missing", error::kUnknownPortReference}}) {
    SCOPED_TRACE(example.source);
    ExpectError(PipelineValidator::ValidateAndPlan(
                    Pipeline({Node("relay", "planning_relay", example.source)}),
                    Boundary()),
                example.code, "/pipeline/0/inputs/text");
  }
  auto root = Pipeline({Node("relay")});
  root["pipeline"][0].erase("inputs");
  ExpectError(PipelineValidator::ValidateAndPlan(root, Boundary()),
              error::kMissingInputProducer, "/pipeline/0/inputs");
  root["pipeline"][0]["inputs"] = {{"unknown", "input.text"}};
  ExpectError(PipelineValidator::ValidateAndPlan(root, Boundary()),
              error::kUnknownField, "/pipeline/0/inputs/unknown");
  root = Pipeline(
      {Node("producer"), Node("relay", "planning_relay", "producer.missing")});
  ExpectError(PipelineValidator::ValidateAndPlan(root, Boundary()),
              error::kUnknownPortReference, "/pipeline/1/inputs/text");
}

TEST_F(PipelinePlanningTest, TypeMismatchCarriesConsumerAndProducerFacts) {
  auto boundary = Boundary();
  boundary.input_published_ports[0].type_id = "tokens";
  const auto plan =
      PipelineValidator::ValidateAndPlan(Pipeline({Node("relay")}), boundary);
  ExpectError(plan, error::kPortTypeMismatch, "/pipeline/0/inputs/text");
  const auto* error = FindDiagnostic(plan.report, error::kPortTypeMismatch,
                                     "/pipeline/0/inputs/text");
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->node_name, "relay");
  EXPECT_EQ(error->port, "text");
  EXPECT_EQ(error->related_nodes, std::vector<std::string>{"input"});
  EXPECT_EQ(error->facts["bound_key"], "input.text");
  EXPECT_EQ(error->facts["actual"]["type_id"], "tokens");
  EXPECT_EQ(error->facts["expected"]["type_id"], "text");
}

TEST_F(PipelinePlanningTest, ChecksDuplicateIngressAndRequiredOutputClosure) {
  auto boundary = Boundary();
  boundary.input_published_ports.push_back(boundary.input_published_ports[0]);
  ExpectError(
      PipelineValidator::ValidateAndPlan(Pipeline({Node("relay")}), boundary),
      error::kDuplicatePortProducer, "/io/input");
  boundary = Boundary("");
  ExpectError(
      PipelineValidator::ValidateAndPlan(Pipeline({Node("relay")}), boundary),
      error::kMissingOutputProducer, "/io/output/0/inputs");
  boundary.output_consumed_ports[0].required = false;
  const auto optional =
      PipelineValidator::ValidateAndPlan(Pipeline({Node("relay")}), boundary);
  ASSERT_TRUE(optional.report.ok);
  EXPECT_EQ(optional.node_plans.at("relay").ports.size(), 1u);
  boundary = Boundary();
  boundary.output_consumed_ports[0].type_id = "tokens";
  ExpectError(
      PipelineValidator::ValidateAndPlan(Pipeline({Node("relay")}), boundary),
      error::kPortTypeMismatch, "/io/output/0/inputs/result");
}

TEST_F(PipelinePlanningTest, OptionalInputsRemainAbsentFromResolvedPorts) {
  auto optional = RelayDefinition("planning_optional");
  optional.inputs.push_back({"context", "text", false});
  Register(optional);
  const auto plan = PipelineValidator::ValidateAndPlan(
      Pipeline({Node("relay", "planning_optional")}), Boundary());
  ASSERT_TRUE(plan.report.ok);
  EXPECT_EQ(plan.node_plans.at("relay").FindPort("context"), nullptr);
  EXPECT_EQ(plan.input_ports.size(), 1u);
}

TEST_F(PipelinePlanningTest, EnforcesEachPortGroupAgainstConnectedInputs) {
  struct Case {
    const char* name;
    PortConstraintKind kind;
    std::vector<std::vector<std::string>> valid;
    std::vector<std::vector<std::string>> invalid;
  };
  const std::vector<Case> cases = {
      {"at least one",
       PortConstraintKind::kAtLeastOneOf,
       {{"a"}, {"a", "b"}},
       {{}}},
      {"exactly one",
       PortConstraintKind::kExactlyOneOf,
       {{"a"}, {"b"}},
       {{}, {"a", "b"}}},
      {"all or none",
       PortConstraintKind::kAllOrNone,
       {{}, {"a", "b"}},
       {{"a"}, {"b"}}},
      {"at most one",
       PortConstraintKind::kAtMostOneOf,
       {{}, {"a"}, {"b"}},
       {{"a", "b"}}},
      {"one complete group",
       PortConstraintKind::kExactOneGroupOf,
       {{"a", "b"}, {"c"}},
       {{}, {"a"}, {"a", "c"}, {"a", "b", "c"}}},
  };
  for (size_t i = 0; i < cases.size(); ++i) {
    const auto& example = cases[i];
    SCOPED_TRACE(example.name);
    auto definition = RelayDefinition("planning_group_" + std::to_string(i));
    definition.inputs = {
        {"a", "text", false}, {"b", "text", false}, {"c", "text", false}};
    definition.port_constraints =
        example.kind == PortConstraintKind::kExactOneGroupOf
            ? std::vector<PortGroupConstraint>{PortGroupConstraint::Groups(
                  example.kind, {{"a", "b"}, {"c"}})}
            : std::vector<PortGroupConstraint>{{example.kind, {"a", "b"}}};
    Register(definition);
    for (bool valid : {true, false}) {
      for (const auto& connected : valid ? example.valid : example.invalid) {
        SCOPED_TRACE(Json(connected).dump());
        auto node = Node("relay", definition.node_type);
        node["inputs"] = Json::object();
        for (const auto& port : connected) node["inputs"][port] = "input.text";
        const auto plan =
            PipelineValidator::ValidateAndPlan(Pipeline({node}), Boundary());
        if (valid)
          EXPECT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
        else
          ExpectError(plan, error::kInvalidCombination, "/pipeline/0");
      }
    }
  }
}

TEST_F(PipelinePlanningTest,
       OneToOnePropagatesFanoutAndAggregationClosesSingleOutput) {
  auto root = Pipeline({Node("split", "planning_split"),
                        Node("relay", "planning_relay", "split.text")});
  auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
  ExpectError(plan, error::kPortCardinalityMismatch,
              "/io/output/0/inputs/result");
  const auto* error =
      FindDiagnostic(plan.report, error::kPortCardinalityMismatch,
                     "/io/output/0/inputs/result");
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->facts["actual_shape"],
            Json({{"kind", "multi"}, {"origin", "split.text"}}));
  EXPECT_EQ(error->facts["expected_shape"], Json({{"kind", "per_request"}}));
  root["pipeline"].push_back(Node("collect", "planning_collect", "relay.text"));
  plan = PipelineValidator::ValidateAndPlan(root, Boundary("collect.text"));
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  EXPECT_EQ(plan.report.topological_order,
            (std::vector<std::string>{"split", "relay", "collect"}));
  EXPECT_EQ(plan.node_plans.at("collect")
                .FindPort("text", PortDirection::kOutput)
                ->cardinality,
            "N:1");
}

TEST_F(PipelinePlanningTest, ItemWiseInputsMustShareTheSameFanoutOrigin) {
  auto root = Pipeline(
      {Node("split", "planning_split"),
       Node("relay", "planning_relay", "split.text"),
       {{"name", "join"},
        {"type", "planning_join"},
        {"inputs", {{"left", "split.text"}, {"right", "relay.text"}}}}});
  auto boundary = Boundary("join.text");
  boundary.output_consumed_ports[0].cardinality = "N:M";
  ASSERT_TRUE(PipelineValidator::ValidateAndPlan(root, boundary).report.ok);
  root["pipeline"][1] = Node("relay", "planning_split", "split.text");
  const auto plan = PipelineValidator::ValidateAndPlan(root, boundary);
  ExpectError(plan, error::kPortCardinalityMismatch,
              "/pipeline/2/inputs/right");
  const auto* error = FindDiagnostic(
      plan.report, error::kPortCardinalityMismatch, "/pipeline/2/inputs/right");
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->facts["actual_shape"]["origin"], "relay.text");
  EXPECT_EQ(error->facts["expected_shape"]["origin"], "split.text");
  EXPECT_EQ(error->facts["anchor_port"], "left");
}

TEST_F(PipelinePlanningTest,
       SessionSourceFlowsToRequestAndFollowLifetimeUsesResolvedInput) {
  auto follow = RelayDefinition("planning_follow");
  follow.outputs[0].lifetime_from_input = "text";
  follow.validate_config = [](const Json&, const BindingFacts& facts,
                              std::string*, std::string*) {
    return facts.has_bindings && facts.connected_inputs.count("text") &&
           facts.InputLifetime("text") == "session";
  };
  Register(follow);
  auto boundary = Boundary("relay.text");
  boundary.input_published_ports[0].lifetime = "session";
  boundary.output_consumed_ports[0].lifetime = "session";
  const auto plan = PipelineValidator::ValidateAndPlan(
      Pipeline({Node("relay", "planning_follow"),
                Node("consumer", "planning_relay", "relay.text")}),
      boundary);
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  const auto& relay = plan.node_plans.at("relay");
  EXPECT_EQ(relay.FindPort("text")->lifetime, "session");
  EXPECT_EQ(relay.FindPort("text", PortDirection::kOutput)->lifetime,
            "session");
  EXPECT_EQ(plan.node_plans.at("consumer").FindPort("text")->lifetime,
            "session");
}

TEST_F(PipelinePlanningTest,
       RejectsRequestSourceForSessionConsumerAndStrictProvenance) {
  auto session = RelayDefinition("planning_session_consumer");
  session.inputs[0].lifetime = "session";
  Register(session);
  ExpectError(PipelineValidator::ValidateAndPlan(
                  Pipeline({Node("relay", session.node_type)}), Boundary()),
              error::kPortLifetimeMismatch, "/pipeline/0/inputs/text");
  auto boundary = Boundary();
  boundary.output_consumed_ports[0].lifetime = "session";
  ExpectError(
      PipelineValidator::ValidateAndPlan(Pipeline({Node("relay")}), boundary),
      error::kPortLifetimeMismatch, "/io/output/0/inputs/result");
  auto provenance = RelayDefinition("planning_provenance");
  provenance.inputs[0].provenance_policy = "generate_sub_id";
  Register(provenance);
  ExpectError(PipelineValidator::ValidateAndPlan(
                  Pipeline({Node("relay", provenance.node_type)}), Boundary()),
              error::kPortProvenanceMismatch, "/pipeline/0/inputs/text");
}

TEST_F(PipelinePlanningTest,
       ParallelLayersSerializeUnsafeNodesWithoutRejectingPlan) {
  auto unsafe = RelayDefinition("planning_unsafe");
  unsafe.parallel_safe = false;
  Register(unsafe);
  const auto root =
      Pipeline({Node("unsafe_a", unsafe.node_type), Node("safe_a"),
                Node("unsafe_b", unsafe.node_type), Node("safe_b")},
               4);
  const auto plan =
      PipelineValidator::ValidateAndPlan(root, Boundary("safe_b.text"));
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  EXPECT_EQ(plan.report.topological_layers,
            (std::vector<std::vector<std::string>>{
                {"safe_a", "safe_b"}, {"unsafe_a"}, {"unsafe_b"}}));
  EXPECT_EQ(
      plan.report.topological_order,
      (std::vector<std::string>{"safe_a", "safe_b", "unsafe_a", "unsafe_b"}));
}

TEST_F(PipelinePlanningTest, SingleWorkerRetainsTheOriginalWavefront) {
  auto unsafe = RelayDefinition("planning_single_worker_unsafe");
  unsafe.parallel_safe = false;
  Register(unsafe);
  const auto plan = PipelineValidator::ValidateAndPlan(
      Pipeline({Node("unsafe", unsafe.node_type), Node("relay")}), Boundary());
  ASSERT_TRUE(plan.report.ok);
  EXPECT_EQ(plan.config.max_parallel_workers, 1u);
  EXPECT_EQ(plan.report.topological_layers,
            (std::vector<std::vector<std::string>>{{"unsafe", "relay"}}));
}

TEST_F(PipelinePlanningTest, DistinctSerializedModelInstancesCanShareALayer) {
  auto root = ModelPipeline(4);
  root["models"][0]["backend"]["type"] = "planning_serial_backend";
  root["models"].push_back(Model("other", "planning_serial_backend"));
  auto second = root["pipeline"][0];
  second["name"] = "second";
  second["params"]["model"] = "other";
  root["pipeline"].push_back(second);
  const auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
  ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
  EXPECT_EQ(plan.report.topological_layers,
            (std::vector<std::vector<std::string>>{{"relay", "second"}}));
}

TEST_F(PipelinePlanningTest,
       SharedModelSerializationUsesBothModelAndBackendDeclaration) {
  for (const auto* backend :
       {"planning_serial_backend", "planning_concurrent_backend"}) {
    SCOPED_TRACE(backend);
    auto root = ModelPipeline(4);
    auto second = root["pipeline"][0];
    second["name"] = "second";
    root["pipeline"].push_back(second);
    root["models"][0]["backend"]["type"] = backend;
    const auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
    ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
    const bool serial = std::string(backend) == "planning_serial_backend";
    EXPECT_EQ(plan.models[0].effective_concurrency,
              serial ? InferenceConcurrency::kSerialized
                     : InferenceConcurrency::kConcurrent);
    EXPECT_EQ(
        plan.report.topological_layers,
        serial ? (std::vector<std::vector<std::string>>{{"relay"}, {"second"}})
               : (std::vector<std::vector<std::string>>{{"relay", "second"}}));
  }
  auto user = RelayDefinition("planning_serial_user");
  user.config_fields =
      NodeRegistry::Instance().Find("planning_model_user")->config_fields;
  user.model_dependencies = {{"generator", "planning_serial_model", "model"}};
  Register(user);
  auto root = ModelPipeline(4);
  root["models"][0]["type"] = "planning_serial_model";
  root["pipeline"][0]["type"] = user.node_type;
  auto second = root["pipeline"][0];
  second["name"] = "second";
  root["pipeline"].push_back(second);
  const auto plan = PipelineValidator::ValidateAndPlan(root, Boundary());
  ASSERT_TRUE(plan.report.ok);
  EXPECT_EQ(plan.models[0].effective_concurrency,
            InferenceConcurrency::kSerialized);
  EXPECT_EQ(plan.report.topological_layers,
            (std::vector<std::vector<std::string>>{{"relay"}, {"second"}}));
}
}  // namespace
}  // namespace llm_edgeflow
