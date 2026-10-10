#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "adapter/converter_authoring.h"
#include "adapter/deployment_diagnostic.h"
#include "adapter/deployment_io_config.h"
#include "adapter/deployment_preparation.h"
#include "adapter/io_catalog.h"
#include "adapter/io_converter_registry.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/io_values.h"
#include "adapter/operator/mock/platform_value_binding.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/pipeline_document.h"
#include "adapter/shared_algorithm_runtime.h"
#include "contracts/registry_conflicts.h"
#include "core/pipeline_config.h"
#include "edgeflow/operator/interface.h"
#include "tests/support/operator_test_fixture.h"
#include "tests/support/registry_test_access.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {
namespace {
namespace fs = std::filesystem;

int dummy_decode_calls = 0;
int dummy_encode_calls = 0;
std::atomic<int> allocator_parse_calls{0};
struct LayoutParams {
  int marker = 0;
};
bool RegisterCountingAllocator() {
  const auto* builtin =
      OperatorValueTypeRegistry::Instance().GetOutputBinding("entity_out", "");
  if (!builtin) return false;
  auto binding = *builtin;
  binding.normalize_parameters = MakeOutputParameterParser<LayoutParams>(
      [](const std::string& text, LayoutParams* params, std::string* error) {
        ++allocator_parse_calls;
        const auto json = nlohmann::json::parse(text);
        if (!json.is_object() || !json.contains("marker") ||
            !json["marker"].is_number_integer()) {
          if (error) *error = "Layout requires integer marker";
          return false;
        }
        params->marker = json["marker"].get<int>();
        return true;
      });
  return RegisterOperatorOutputAllocator("unit_counting_allocator", binding);
}
int DummyDecode(const ExternalInputBatchView&, const InputDecodeOptions&,
                AlgContext*, AdapterStatus*) {
  ++dummy_decode_calls;
  return 0;
}
int DummyEncode(AlgContext*, const OutputEncodeOptions&,
                ExternalOutputBatchView*, size_t* written, AdapterStatus*) {
  ++dummy_encode_calls;
  if (written) *written = 1;
  return 0;
}
struct SizeParams {
  int64_t entities_json_max_bytes = 0;
};
Parameters<SizeParams> SizeParameters(int64_t default_size = 2047) {
  return Parameters<SizeParams>{
      MaxBytes("entities_json", &SizeParams::entities_json_max_bytes)
          .Default(default_size)};
}
InputConverterDefinition TestInput() {
  InputConverterDefinition def;
  def.type = "entity_in";
  def.name = "test_service";
  def.slot = ExternalInputSlot<TextInputValue>(def.type);
  def.logical_ports = {
      NodePortDefinition("sentence_text", "TextBatch", true, "1:1")};
  def.decode_fn = DummyDecode;
  return def;
}
OutputConverterDefinition TestOutput() {
  OutputConverterDefinition def;
  def.type = "entity_out";
  def.name = "test_service";
  def.slot = ExternalOutputSlot<EntityOutputValue>(def.type);
  def.logical_ports = {
      NodePortDefinition("answer_text", "TextBatch", true, "1:1")};
  def.params = SizeParameters();
  def.encode_fn = DummyEncode;
  return def;
}
nlohmann::json TestIo() {
  return {{"input", {{{"type", "entity_in"}, {"name", "test_service"}}}},
          {"output",
           {{{"type", "entity_out"},
             {"name", "test_service"},
             {"inputs", {{"answer_text", "copy.text"}}}}}}};
}
nlohmann::json DefaultPipelineNodes() {
  return {{{"name", "copy"},
           {"type", "text_template"},
           {"inputs", {{"primary", "input.sentence_text"}}},
           {"params", {{"template", "{{primary}}"}}}}};
}
nlohmann::json TestDocument() {
  return {{"io", TestIo()},
          {"models", nlohmann::json::array()},
          {"pipeline", DefaultPipelineNodes()}};
}
}  // namespace

class IoConverterRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(RegisterCountingAllocator());
    test_support::RegistryTestAccess::SetService("entity_in", "test_service",
                                                 1001);
    test_support::RegistryTestAccess::SetService("entity_out", "test_service",
                                                 1001);
    auto& registry = IoConverterRegistry::Instance();
    saved_inputs_ = registry.AllInputConverters();
    saved_outputs_ = registry.AllOutputConverters();
    registry.ClearForTesting();
    ASSERT_TRUE(registry.RegisterInputConverter(TestInput()));
    ASSERT_TRUE(registry.RegisterOutputConverter(TestOutput()));
  }
  void TearDown() override {
    auto& registry = IoConverterRegistry::Instance();
    registry.ClearForTesting();
    for (const auto& def : saved_inputs_)
      EXPECT_TRUE(registry.RegisterInputConverter(def));
    for (const auto& def : saved_outputs_)
      EXPECT_TRUE(registry.RegisterOutputConverter(def));
  }
  void ExpectAuditFailure(const std::string& label, const std::string& reason) {
    std::vector<std::string> errors;
    EXPECT_FALSE(IoConverterRegistry::Instance().Audit(&errors));
    EXPECT_TRUE(std::any_of(errors.begin(), errors.end(),
                            [&](const auto& error) {
                              return error.find(label) != std::string::npos &&
                                     error.find(reason) != std::string::npos;
                            }))
        << ::testing::PrintToString(errors);
  }
  test_support::RegistryTestAccess::ScopedValueTypeState value_types_;
  std::vector<InputConverterDefinition> saved_inputs_;
  std::vector<OutputConverterDefinition> saved_outputs_;
};

TEST_F(IoConverterRegistryTest, ValidDefinitionsAreFoundByPairAndAuditCleanly) {
  auto& registry = IoConverterRegistry::Instance();
  const auto* input = registry.FindInputConverter("entity_in", "test_service");
  const auto* output =
      registry.FindOutputConverter("entity_out", "test_service");
  ASSERT_NE(input, nullptr);
  ASSERT_NE(output, nullptr);
  EXPECT_EQ(input->Label(), "entity_in/test_service");
  EXPECT_EQ(output->Label(), "entity_out/test_service");
  EXPECT_EQ(input->slot.type_suffix, input->type);
  EXPECT_EQ(output->slot.type_suffix, output->type);
  EXPECT_EQ(registry.FindInputConverter("entity_in", "unknown"), nullptr);
  EXPECT_EQ(registry.FindInputConverter("entity_out", "test_service"), nullptr);
  std::vector<std::string> errors;
  EXPECT_TRUE(registry.Audit(&errors)) << ::testing::PrintToString(errors);
  EXPECT_TRUE(errors.empty());
}

TEST_F(IoConverterRegistryTest, DuplicatePairsAndServicesRemainConflicts) {
  auto& registry = IoConverterRegistry::Instance();
  EXPECT_FALSE(registry.RegisterInputConverter(TestInput()));
  EXPECT_TRUE(registry.HasConflict());
  ExpectAuditFailure("entity_in/test_service", "Duplicate converter");
  registry.ResetConflictForTesting();
  auto input = TestInput();
  input.name = "another_service";
  test_support::RegistryTestAccess::SetService(input.type, input.name, 1001);
  EXPECT_TRUE(registry.RegisterInputConverter(input));
  ExpectAuditFailure("entity_in", "Duplicate service_type");
  test_support::RegistryTestAccess::SetService(input.type, input.name, 1002);
  auto output = TestOutput();
  output.name = "another_service";
  test_support::RegistryTestAccess::SetService(output.type, output.name, 1001);
  EXPECT_TRUE(registry.RegisterOutputConverter(output));
  ExpectAuditFailure("entity_out", "Duplicate service_type");
  test_support::RegistryTestAccess::SetService(output.type, output.name, 1002);
  input.type = "keyword_in";
  input.slot = ExternalInputSlot<TextInputValue>(input.type);
  test_support::RegistryTestAccess::SetService(input.type, input.name, 1001);
  EXPECT_TRUE(registry.RegisterInputConverter(input));
  EXPECT_TRUE(registry.Audit());
}

TEST_F(IoConverterRegistryTest, RegistrationRequiresTypeNameAndCallback) {
  auto& registry = IoConverterRegistry::Instance();
  for (int variant = 0; variant < 3; ++variant) {
    SCOPED_TRACE(variant);
    auto input = TestInput();
    auto output = TestOutput();
    if (variant == 0) {
      input.type.clear();
      output.type.clear();
    } else if (variant == 1) {
      input.name.clear();
      output.name.clear();
    } else {
      input.decode_fn = nullptr;
      output.encode_fn = nullptr;
    }
    EXPECT_FALSE(registry.RegisterInputConverter(input));
    EXPECT_FALSE(registry.RegisterOutputConverter(output));
    EXPECT_TRUE(registry.HasConflict());
    EXPECT_FALSE(registry.Audit());
    registry.ResetConflictForTesting();
  }
  EXPECT_TRUE(registry.Audit());
}

TEST_F(IoConverterRegistryTest, SlotMustMatchRegisteredPlatformTypeAndSuffix) {
  auto& registry = IoConverterRegistry::Instance();
  for (int variant = 0; variant < 3; ++variant) {
    auto input = TestInput();
    input.name = "bad_input_" + std::to_string(variant);
    test_support::RegistryTestAccess::SetService(input.type, input.name,
                                                 1100 + variant);
    if (variant == 0) input.slot.type_suffix = "different";
    if (variant == 1) input.slot.value_type = typeid(std::string);
    if (variant == 2) {
      input.type = "unknown_platform_type";
      input.slot.type_suffix = input.type;
    }
    ASSERT_TRUE(registry.RegisterInputConverter(input));
    ExpectAuditFailure(input.Label(), variant == 0 ? "type_suffix"
                                      : variant == 1
                                          ? "value type"
                                          : "No registered platform");
  }
  auto output = TestOutput();
  output.name = "bad_output_struct";
  test_support::RegistryTestAccess::SetService(output.type, output.name, 1200);
  output.slot.value_type = typeid(KeywordOutputValue);
  ASSERT_TRUE(registry.RegisterOutputConverter(output));
  ExpectAuditFailure(output.Label(), "value type");
}

TEST_F(IoConverterRegistryTest, AuditRequiresBindingValueReadersAndWriters) {
  auto& values = OperatorValueTypeRegistry::Instance();
  const auto input = *values.GetBindingBySuffix("entity_in");
  for (const bool missing_reader : {false, true}) {
    SCOPED_TRACE(missing_reader);
    auto invalid = input;
    if (missing_reader)
      invalid.read_value = nullptr;
    else
      invalid.validate_external = nullptr;
    test_support::RegistryTestAccess::SetValueBinding(std::move(invalid));
    ExpectAuditFailure("entity_in/test_service", "value reader");
  }
  test_support::RegistryTestAccess::SetValueBinding(input);
  auto output = *values.GetBindingBySuffix("entity_out");
  output.write_value = nullptr;
  test_support::RegistryTestAccess::SetValueBinding(std::move(output));
  ExpectAuditFailure("entity_out/test_service", "value writer");
}

TEST_F(IoConverterRegistryTest, NamedAllocatorMustMatchNeutralValueAndWriter) {
  const auto* builtin =
      OperatorValueTypeRegistry::Instance().GetOutputBinding("entity_out", "");
  ASSERT_NE(builtin, nullptr);
  for (const bool wrong_type : {false, true}) {
    SCOPED_TRACE(wrong_type);
    auto binding = *builtin;
    if (wrong_type)
      binding.value_type = typeid(KeywordOutputValue);
    else
      binding.write_value = nullptr;
    const std::string allocator = wrong_type ? "unit_wrong_value_allocator"
                                             : "unit_missing_writer_allocator";
    ASSERT_TRUE(RegisterOperatorOutputAllocator(allocator, binding));
    auto output = TestOutput();
    output.name = allocator;
    output.slot.allocator = allocator;
    test_support::RegistryTestAccess::SetService(output.type, output.name,
                                                 wrong_type ? 1251 : 1250);
    ASSERT_TRUE(
        IoConverterRegistry::Instance().RegisterOutputConverter(output));
    ExpectAuditFailure(output.Label(), "Allocator value type or writer");
  }
}

TEST_F(IoConverterRegistryTest, ServiceDeclarationMatchesHostMembersAndCommon) {
  auto& registry = IoConverterRegistry::Instance();
  auto missing = TestInput();
  missing.name = "missing_service";
  ASSERT_TRUE(registry.RegisterInputConverter(missing));
  ExpectAuditFailure(missing.Label(), "service_type");
  auto common = TestOutput();
  common.name = kCommonIoName;
  test_support::RegistryTestAccess::SetService(common.type, common.name, 1200);
  ASSERT_TRUE(registry.RegisterOutputConverter(common));
  ExpectAuditFailure(common.Label(), "service_type");
  auto string = TestInput();
  string.type = "string";
  string.name = "string_with_service";
  test_support::RegistryTestAccess::SetService(string.type, string.name, 1200);
  string.slot = ExternalInputSlot<std::string>(string.type);
  ASSERT_TRUE(registry.RegisterInputConverter(string));
  ExpectAuditFailure(string.Label(), "service_type");
  registry.ClearForTesting();
  string.name = "string_service";
  auto string_binding =
      *OperatorValueTypeRegistry::Instance().GetBindingBySuffix("string");
  string_binding.services.clear();
  test_support::RegistryTestAccess::SetValueBinding(std::move(string_binding));
  ASSERT_TRUE(registry.RegisterInputConverter(string));
  auto output_binding =
      *OperatorValueTypeRegistry::Instance().GetBindingBySuffix("entity_out");
  output_binding.services.erase(kCommonIoName);
  test_support::RegistryTestAccess::SetValueBinding(std::move(output_binding));
  ASSERT_TRUE(registry.RegisterOutputConverter(common));
  EXPECT_TRUE(registry.Audit());
}

TEST_F(IoConverterRegistryTest,
       NamedAllocatorUsesHostServiceDeclarationForAudit) {
  ASSERT_NE(OperatorValueTypeRegistry::Instance().GetOutputBinding(
                "entity_out", "unit_counting_allocator"),
            nullptr);
  auto& registry = IoConverterRegistry::Instance();
  for (const bool common : {false, true}) {
    for (const bool declared_service : {false, true}) {
      SCOPED_TRACE(common);
      SCOPED_TRACE(declared_service);
      registry.ClearForTesting();
      ASSERT_TRUE(registry.RegisterInputConverter(TestInput()));
      auto output = TestOutput();
      output.name = common ? kCommonIoName : "named_layout_service";
      output.slot.allocator = "unit_counting_allocator";
      output.slot.allocator_params = R"({"marker":7})";
      auto binding = *OperatorValueTypeRegistry::Instance().GetBindingBySuffix(
          output.type);
      binding.services.erase(kCommonIoName);
      binding.services.erase("named_layout_service");
      if (declared_service) binding.services[output.name] = 1200;
      test_support::RegistryTestAccess::SetValueBinding(std::move(binding));
      ASSERT_TRUE(registry.RegisterOutputConverter(output));
      if (common != declared_service) {
        std::vector<std::string> errors;
        EXPECT_TRUE(registry.Audit(&errors))
            << ::testing::PrintToString(errors);
      } else {
        ExpectAuditFailure(output.Label(), "service_type");
      }
    }
  }
}

TEST_F(IoConverterRegistryTest, NamedAllocatorProcessWritesTheHostServiceType) {
  ASSERT_NE(OperatorValueTypeRegistry::Instance().GetOutputBinding(
                "entity_out", "unit_counting_allocator"),
            nullptr);
  const auto input = std::find_if(
      saved_inputs_.begin(), saved_inputs_.end(), [](const auto& def) {
        return def.type == "entity_in" && def.name == "entity_extract";
      });
  const auto output = std::find_if(
      saved_outputs_.begin(), saved_outputs_.end(), [](const auto& def) {
        return def.type == "entity_out" && def.name == "entity_extract";
      });
  ASSERT_NE(input, saved_inputs_.end());
  ASSERT_NE(output, saved_outputs_.end());
  auto selected_output = *output;
  selected_output.slot.allocator = "unit_counting_allocator";
  selected_output.slot.allocator_params = R"({"marker":7})";
  auto& registry = IoConverterRegistry::Instance();
  registry.ClearForTesting();
  ASSERT_TRUE(registry.RegisterInputConverter(*input));
  ASSERT_TRUE(registry.RegisterOutputConverter(selected_output));
  const nlohmann::json document = {
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "entity_extract"}}}},
        {"output",
         {{{"type", "entity_out"},
           {"name", "entity_extract"},
           {"inputs", {{"entities", "parse.document"}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"name", "parse"},
         {"type", "structured_json_parse"},
         {"inputs", {{"text", "input.sentence_text"}}},
         {"params", {{"failure_policy", "fail"}}}}}}};
  const auto directory =
      fs::temp_directory_path() /
      ("edgeflow_named_allocator_process_" + std::to_string(getpid()));
  fs::create_directories(directory);
  std::ofstream(directory / "pipeline.json") << document;
  std::ofstream(directory / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  const auto ops = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0) << operator_api::GetOperatorLastError();
  test_support::ScopedTestOperator instance(ops);
  ASSERT_EQ(instance.Create("pipeline.conf", directory.string()), 0)
      << instance.create_diagnostic();
  const std::string text = R"([{"name":"Alice","type":"PERSON"}])";
  CompanyString string{static_cast<int32_t>(text.size()),
                       const_cast<char*>(text.data())};
  CompanyOperatorEntityInput request{};
  request.service_type = kMockServiceEntityExtract;
  request.sentence_text = &string;
  operator_api::NamedIoBatch inputs(1);
  inputs[0]["channel.entity_in"] =
      operator_api::MakeBorrowedOperatorInput(&request);
  operator_api::NamedIoBatch outputs(1);
  outputs[0]["channel.entity_out"] = nullptr;
  ASSERT_EQ(ops.Process(instance.get(), inputs, outputs), 0)
      << operator_api::GetOperatorLastError();
  const auto* result = static_cast<const CompanyOperatorEntityOutput*>(
      outputs[0].at("channel.entity_out").get());
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->service_type, kMockServiceEntityExtract);
  EXPECT_EQ(result->status_code, 0);
  ASSERT_NE(result->entities_json, nullptr);
  EXPECT_EQ(nlohmann::json::parse(std::string(result->entities_json->data,
                                              result->entities_json->length)),
            nlohmann::json::parse(text));
  outputs.clear();
  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
  EXPECT_EQ(ops.DeInit(), 0) << operator_api::GetOperatorLastError();
  fs::remove_all(directory);
}

TEST_F(IoConverterRegistryTest, LogicalPortsMustBeNonemptyTypedAndUnique) {
  for (int variant = 0; variant < 4; ++variant) {
    auto input = TestInput();
    input.name = "bad_ports_" + std::to_string(variant);
    test_support::RegistryTestAccess::SetService(input.type, input.name,
                                                 1300 + variant);
    if (variant == 0) input.logical_ports.clear();
    if (variant == 1)
      input.logical_ports.push_back(input.logical_ports.front());
    if (variant == 2) input.logical_ports.front().logical_name.clear();
    if (variant == 3) input.logical_ports.front().type_id.clear();
    ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(input));
    ExpectAuditFailure(input.Label(), variant == 0   ? "Logical ports"
                                      : variant == 1 ? "Duplicate logical port"
                                                     : "nonempty");
  }
}

TEST_F(IoConverterRegistryTest, AuditRejectsMissingOrInvalidSizeDeclarations) {
  for (int variant = 0; variant < 3; ++variant) {
    auto output = TestOutput();
    output.name = "bad_size_" + std::to_string(variant);
    test_support::RegistryTestAccess::SetService(output.type, output.name,
                                                 1400 + variant);
    if (variant == 0) output.params = ParameterSet{};
    if (variant == 1) {
      output.params = Parameters<SizeParams>{
          Field("entities_json_max_bytes", &SizeParams::entities_json_max_bytes)
              .Default(0)};
    }
    if (variant == 2) output.params = SizeParameters(65537);
    ASSERT_TRUE(
        IoConverterRegistry::Instance().RegisterOutputConverter(output));
    ExpectAuditFailure(output.Label(), "size default");
  }
}

TEST_F(IoConverterRegistryTest,
       AuditRejectsUnknownAllocatorParametersAndMetadata) {
  auto& registry = IoConverterRegistry::Instance();
  auto unknown = TestOutput();
  unknown.name = "unknown_layout";
  test_support::RegistryTestAccess::SetService(unknown.type, unknown.name,
                                               1500);
  unknown.slot.allocator = "unknown_allocator";
  ASSERT_TRUE(registry.RegisterOutputConverter(unknown));
  ExpectAuditFailure(unknown.Label(), "No registered platform");
  auto invalid = TestOutput();
  invalid.name = "invalid_layout_params";
  test_support::RegistryTestAccess::SetService(invalid.type, invalid.name,
                                               1501);
  invalid.slot.allocator_params = R"({"unexpected":1})";
  ASSERT_TRUE(registry.RegisterOutputConverter(invalid));
  ExpectAuditFailure(invalid.Label(), "does not accept params");
  auto metadata = TestOutput();
  metadata.name = "no_metadata_member";
  test_support::RegistryTestAccess::SetService(metadata.type, metadata.name,
                                               1502);
  metadata.slot.metadata_count = 1;
  metadata.slot.metadata_type_id = 1;
  ASSERT_TRUE(registry.RegisterOutputConverter(metadata));
  ExpectAuditFailure(metadata.Label(), "metadata");
}

TEST_F(IoConverterRegistryTest,
       UnselectedBadConverterBlocksGlobalInitialization) {
  ASSERT_TRUE(IoConverterRegistry::Instance().Audit());
  auto bad = TestInput();
  bad.name = "unselected_struct_mismatch";
  test_support::RegistryTestAccess::SetService(bad.type, bad.name, 1600);
  bad.slot.value_type = typeid(DocumentInputValue);
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(bad));
  std::string diagnostic;
  EXPECT_EQ(SharedAlgorithmRuntime::GlobalInit(&diagnostic),
            COMPANY_ALG_ERR_REGISTRY_CONFLICT);
  EXPECT_NE(diagnostic.find("Converter audit"), std::string::npos);
  EXPECT_NE(diagnostic.find(bad.Label()), std::string::npos);
  EXPECT_NE(diagnostic.find("value type"), std::string::npos);
}

TEST_F(IoConverterRegistryTest, StandardLayoutRequiresExplicitStringSizes) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetOutputBinding("entity_out", "");
  ASSERT_NE(binding, nullptr);
  ResolvedOutputPoolSpec requested;
  requested.type = "entity_out";
  ResolvedOutputPoolSpec resolved;
  std::string error;
  EXPECT_FALSE(ResolveOutputPoolSpec(*binding, requested, &resolved, &error));
  EXPECT_NE(error.find("entities_json"), std::string::npos);
  requested.capacities["entities_json"] = 17;
  ASSERT_TRUE(ResolveOutputPoolSpec(*binding, requested, &resolved, &error))
      << error;
  EXPECT_EQ(resolved.GetCapacity("entities_json"), 17u);
}

TEST_F(IoConverterRegistryTest,
       AllocatorNormalizationIsCachedAcrossAuditAndCreate) {
  ASSERT_NE(OperatorValueTypeRegistry::Instance().GetOutputBinding(
                "entity_out", "unit_counting_allocator"),
            nullptr);
  auto output = TestOutput();
  output.name = "counted_layout";
  test_support::RegistryTestAccess::SetService(output.type, output.name, 1700);
  output.slot.allocator = "unit_counting_allocator";
  output.slot.allocator_params = R"({"marker":7})";
  auto& registry = IoConverterRegistry::Instance();
  ASSERT_TRUE(registry.RegisterOutputConverter(output));
  const int before = allocator_parse_calls.load();
  ASSERT_TRUE(registry.Audit());
  const auto normalized = registry.OutputParameters(output.type, output.name);
  ASSERT_NE(normalized, nullptr);
  EXPECT_EQ(allocator_parse_calls.load(), before + 1);
  ASSERT_TRUE(registry.Audit());
  std::atomic<int> audit_failures{0};
  std::thread first([&] {
    if (!registry.Audit()) ++audit_failures;
  });
  std::thread second([&] {
    if (!registry.Audit()) ++audit_failures;
  });
  first.join();
  second.join();
  EXPECT_EQ(audit_failures.load(), 0);
  auto document = TestDocument();
  document["io"]["output"][0]["name"] = output.name;
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string error;
  ASSERT_EQ(
      IoPlanResolver::ResolveFromPipelineJson(document, "", &plan, &error), 0)
      << error;
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(plan->outputs.front().pool_spec.params, normalized);
  EXPECT_EQ(plan->outputs.front().pool_spec.Parameters<LayoutParams>().marker,
            7);
  std::unique_ptr<SharedAlgorithmRuntime> runtime;
  const auto status = SharedAlgorithmRuntime::CreateFromIoPlan(
      std::move(plan), RuntimeCreateOptions{}, &runtime, &error);
  ASSERT_EQ(status, 0) << error;
  ASSERT_NE(runtime, nullptr);
  EXPECT_EQ(allocator_parse_calls.load(), before + 1);
}

TEST_F(IoConverterRegistryTest, SplitKeepsCoreNeutralAndBoundaryExplicit) {
  const auto document = TestDocument();
  PipelineDocumentSplit split;
  std::string error;
  ASSERT_TRUE(SplitPipelineDocument(document, &split, &error)) << error;
  ASSERT_EQ(split.inputs.size(), 1u);
  ASSERT_EQ(split.outputs.size(), 1u);
  EXPECT_EQ(split.inputs.front().type, "entity_in");
  EXPECT_EQ(split.outputs.front().name, "test_service");
  EXPECT_FALSE(split.neutral_pipeline_json.contains("io"));
  EXPECT_EQ(split.neutral_pipeline_json["models"], document["models"]);
  EXPECT_EQ(split.neutral_pipeline_json["pipeline"], document["pipeline"]);
  ParsedPipelineConfig parsed;
  PipelineDiagnostic diagnostic;
  EXPECT_TRUE(
      ParsePipelineConfig(split.neutral_pipeline_json, &parsed, &diagnostic));
  PreparedDeployment prepared;
  DeploymentDiagnostic deployment_diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared,
                                        &deployment_diagnostic))
      << deployment_diagnostic.message;
  ASSERT_EQ(prepared.io_boundary.input_published_ports.size(), 1u);
  ASSERT_EQ(prepared.io_boundary.output_consumed_ports.size(), 1u);
  EXPECT_EQ(prepared.io_boundary.input_published_ports.front().Name(),
            "input.sentence_text");
  EXPECT_EQ(prepared.io_boundary.output_consumed_ports.front().Name(),
            "copy.text");
}

TEST_F(IoConverterRegistryTest, PlannedConvertersOwnExplicitPortBindings) {
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string error;
  ASSERT_EQ(IoPlanResolver::ResolveFromPipelineJson(TestDocument(), "", &plan,
                                                    &error),
            0)
      << error;
  ASSERT_NE(plan, nullptr);
  ASSERT_EQ(plan->inputs.size(), 1u);
  ASSERT_EQ(plan->outputs.size(), 1u);
  EXPECT_EQ(plan->inputs[0].ports,
            (IoPortBindings{{"sentence_text", "input.sentence_text"}}));
  EXPECT_EQ(plan->outputs[0].ports,
            (IoPortBindings{{"answer_text", "copy.text"}}));
  EXPECT_EQ(plan->resolved_pipeline_json["io"]["output"][0]["inputs"],
            nlohmann::json({{"answer_text", "copy.text"}}));
}

TEST_F(IoConverterRegistryTest,
       MissingOutputConnectionDiffersFromExplicitEmptySource) {
  for (bool explicit_empty : {false, true}) {
    auto document = TestDocument();
    if (explicit_empty)
      document["io"]["output"][0]["inputs"]["answer_text"] = "";
    else
      document["io"]["output"][0].erase("inputs");
    std::unique_ptr<ValidatedIoPlan> plan;
    std::string error;
    DeploymentDiagnostic diagnostic;
    EXPECT_NE(IoPlanResolver::ResolveFromPipelineJson(document, "", &plan,
                                                      &error, &diagnostic),
              0);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diagnostic.code,
              explicit_empty ? "FIELD_TYPE" : "MISSING_OUTPUT_PRODUCER");
    EXPECT_EQ(diagnostic.path, explicit_empty
                                   ? "/io/output/0/inputs/answer_text"
                                   : "/io/output/0/inputs");
  }
}

TEST_F(IoConverterRegistryTest,
       IoStructureRejectsMalformedEntriesWithExactPaths) {
  struct Invalid {
    nlohmann::json::json_pointer target;
    nlohmann::json value;
    std::string path;
  };
  const std::vector<Invalid> invalid = {
      {nlohmann::json::json_pointer("/io"), nullptr, "/io"},
      {nlohmann::json::json_pointer("/io/input"), nlohmann::json::object(),
       "/io/input"},
      {nlohmann::json::json_pointer("/io/input"), nlohmann::json::array(),
       "/io/input"},
      {nlohmann::json::json_pointer("/io/input/0"), 1, "/io/input/0"},
      {nlohmann::json::json_pointer("/io/input/0/type"), "",
       "/io/input/0/type"},
      {nlohmann::json::json_pointer("/io/input/0/inputs"),
       nlohmann::json::object(), "/io/input/0/inputs"},
      {nlohmann::json::json_pointer("/inputs"), nlohmann::json::object(),
       "/inputs"},
      {nlohmann::json::json_pointer("/io/output/0/inputs_extra"),
       nlohmann::json::object(), "/io/output/0/inputs_extra"},
      {nlohmann::json::json_pointer("/io/output/0/name"), false,
       "/io/output/0/name"},
      {nlohmann::json::json_pointer("/io/output/0/params"), 17,
       "/io/output/0/params"},
      {nlohmann::json::json_pointer("/io/output/0/unknown~0~1key"), 17,
       "/io/output/0/unknown~0~1key"}};
  for (const auto& entry : invalid) {
    SCOPED_TRACE(entry.path);
    auto document = TestDocument();
    document[entry.target] = entry.value;
    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(
        PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
    EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diagnostic.path, entry.path);
  }
  for (const char* field : {"io", "input", "output", "type", "name"}) {
    auto document = TestDocument();
    std::string path;
    if (std::string(field) == "io") {
      document.erase(field);
      path = "/io";
    } else if (std::string(field) == "input" ||
               std::string(field) == "output") {
      document["io"].erase(field);
      path = std::string("/io/") + field;
    } else {
      document["io"]["input"][0].erase(field);
      path = std::string("/io/input/0/") + field;
    }
    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(
        PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
    EXPECT_EQ(diagnostic.path, path);
  }
}

TEST_F(IoConverterRegistryTest,
       UnknownAndDuplicatePairsHaveActionableDiagnostics) {
  for (const char* side : {"input", "output"}) {
    for (const char* name : {"unknown_service", kCommonIoName}) {
      auto document = TestDocument();
      document["io"][side][0]["name"] = name;
      PreparedDeployment prepared;
      DeploymentDiagnostic diagnostic;
      EXPECT_FALSE(
          PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
      EXPECT_EQ(diagnostic.code, "UNKNOWN_CONVERTER");
      EXPECT_EQ(diagnostic.path, std::string("/io/") + side + "/0");
      EXPECT_NE(diagnostic.message.find("test_service"), std::string::npos);
    }
    auto typo = TestDocument();
    const auto registered_type = typo["io"][side][0]["type"].get<std::string>();
    typo["io"][side][0]["type"] = registered_type + "x";
    PreparedDeployment typo_prepared;
    DeploymentDiagnostic typo_diagnostic;
    EXPECT_FALSE(
        PrepareDeploymentDocument(typo, {}, &typo_prepared, &typo_diagnostic));
    EXPECT_EQ(typo_diagnostic.code, "UNKNOWN_CONVERTER");
    EXPECT_EQ(typo_diagnostic.path, std::string("/io/") + side + "/0");
    EXPECT_NE(
        typo_diagnostic.message.find("suggested type: " + registered_type),
        std::string::npos);
    auto document = TestDocument();
    document["io"][side].push_back(document["io"][side][0]);
    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(
        PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
    EXPECT_EQ(diagnostic.code, "DUPLICATE_IO_ENTRY");
    EXPECT_EQ(diagnostic.path, std::string("/io/") + side + "/1");
  }
}

TEST_F(IoConverterRegistryTest, SelectedInputsRejectDuplicateLogicalProducers) {
  auto second = TestInput();
  second.name = "second_service";
  test_support::RegistryTestAccess::SetService(second.type, second.name, 1900);
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(second));
  auto document = TestDocument();
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  document["io"]["input"].push_back(
      {{"type", second.type}, {"name", second.name}});
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "DUPLICATE_PORT_PRODUCER");
  EXPECT_EQ(diagnostic.path, "/io/input/1");
  EXPECT_NE(diagnostic.message.find("sentence_text"), std::string::npos);
  EXPECT_TRUE(prepared.inputs.empty());
  EXPECT_TRUE(prepared.outputs.empty());
  EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
  EXPECT_TRUE(prepared.io_boundary.input_published_ports.empty());
  EXPECT_TRUE(prepared.io_boundary.output_consumed_ports.empty());
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string error;
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(document, "", &plan, &error,
                                                    &diagnostic),
            -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diagnostic.code, "DUPLICATE_PORT_PRODUCER");
  EXPECT_EQ(diagnostic.path, "/io/input/1");
}

TEST_F(IoConverterRegistryTest, SelectedStringInputsNeedNoRequestIdSource) {
  auto first = TestInput();
  first.type = "string";
  first.name = "first_string";
  first.slot = ExternalInputSlot<std::string>(first.type);
  first.logical_ports = {
      NodePortDefinition("first_text", "TextBatch", true, "1:1")};
  auto second = first;
  second.name = "second_string";
  second.logical_ports = {
      NodePortDefinition("second_text", "TextBatch", true, "1:1")};
  auto& registry = IoConverterRegistry::Instance();
  ASSERT_TRUE(registry.RegisterInputConverter(first));
  ASSERT_TRUE(registry.RegisterInputConverter(second));
  auto document = TestDocument();
  document["io"]["input"] = {{{"type", first.type}, {"name", first.name}},
                             {{"type", second.type}, {"name", second.name}}};
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  ASSERT_EQ(prepared.inputs.size(), 2U);
  EXPECT_EQ(prepared.io_boundary.input_published_ports.size(), 2U);
}

TEST_F(IoConverterRegistryTest, ParameterDefaultsOverridesAndEscapedErrors) {
  auto document = TestDocument();
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  EXPECT_EQ(prepared.outputs.front().pool_spec.GetCapacity("entities_json"),
            2047u);
  document["io"]["output"][0]["params"] = {{"entities_json_max_bytes", 17}};
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  EXPECT_EQ(prepared.outputs.front()
                .params->Get<SizeParams>()
                .entities_json_max_bytes,
            17);
  EXPECT_EQ(prepared.outputs.front().pool_spec.GetCapacity("entities_json"),
            17u);
  for (int64_t value : {0, 65537}) {
    document["io"]["output"][0]["params"]["entities_json_max_bytes"] = value;
    EXPECT_FALSE(
        PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
    EXPECT_EQ(diagnostic.code, "CONFIG_FIELD_RANGE");
    EXPECT_EQ(diagnostic.path, "/io/output/0/params/entities_json_max_bytes");
  }
  document["io"]["output"][0]["params"] = {{"bad~/field", 1}};
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "UNKNOWN_CONFIG_FIELD");
  EXPECT_EQ(diagnostic.path, "/io/output/0/params/bad~0~1field");
}

TEST_F(IoConverterRegistryTest,
       PrepareDerivedSizesReachPoolAndEffectiveSnapshotOnce) {
  int prepare_calls = 0;
  auto output = TestOutput();
  output.name = "derived_size";
  test_support::RegistryTestAccess::SetService(output.type, output.name, 1800);
  output.params = SizeParameters(17).Prepare(
      [&](SizeParams* params, std::string* diagnostic) {
        ++prepare_calls;
        if (params->entities_json_max_bytes == 13) {
          if (diagnostic) *diagnostic = "unlucky size";
          return false;
        }
        params->entities_json_max_bytes *= 2;
        return true;
      });
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterOutputConverter(output));
  auto document = TestDocument();
  document["io"]["output"][0]["name"] = output.name;
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string error;
  DeploymentDiagnostic diagnostic;
  ASSERT_EQ(IoPlanResolver::ResolveFromPipelineJson(document, "", &plan, &error,
                                                    &diagnostic),
            0)
      << error;
  EXPECT_EQ(prepare_calls, 1);
  EXPECT_EQ(plan->outputs.front().pool_spec.GetCapacity("entities_json"), 34u);
  EXPECT_EQ(plan->resolved_pipeline_json["io"]["output"][0]["params"],
            nlohmann::json({{"entities_json_max_bytes", 34}}));
  EXPECT_EQ(plan->resolved_pipeline_json["io"]["input"][0]["params"],
            nlohmann::json::object());
  EXPECT_FALSE(document["io"]["output"][0].contains("params"));
  document["io"]["output"][0]["params"] = {{"entities_json_max_bytes", 13}};
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(document, "", &plan, &error,
                                                    &diagnostic),
            -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diagnostic.code, "INVALID_COMBINATION");
  EXPECT_EQ(diagnostic.path, "/io/output/0/params");
  EXPECT_EQ(prepare_calls, 2);
}

TEST_F(IoConverterRegistryTest,
       PrepareFailureClearsAllSelectionAndBoundaryState) {
  const auto valid = TestDocument();
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(valid, {}, &prepared, &diagnostic));
  ASSERT_FALSE(prepared.inputs.empty());
  ASSERT_FALSE(prepared.outputs.empty());
  ASSERT_FALSE(prepared.parsed_pipeline.nodes.empty());
  auto invalid = valid;
  invalid["io"]["output"][0]["name"] = "unknown_service";
  const auto original = invalid;
  EXPECT_FALSE(PrepareDeploymentDocument(invalid, {}, &prepared, &diagnostic));
  EXPECT_TRUE(prepared.inputs.empty());
  EXPECT_TRUE(prepared.outputs.empty());
  EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
  EXPECT_TRUE(prepared.parsed_pipeline.nodes.empty());
  EXPECT_TRUE(prepared.parsed_pipeline.models.empty());
  EXPECT_TRUE(prepared.io_boundary.input_published_ports.empty());
  EXPECT_TRUE(prepared.io_boundary.output_consumed_ports.empty());
  EXPECT_EQ(invalid, original);
  auto models = valid;
  models["models"] = {{{"name", "first"},
                       {"type", "embedding"},
                       {"backend", {{"type", "test_tensor_backend"}}},
                       {"file", "first.bin"}},
                      {{"name", "second"},
                       {"type", "embedding"},
                       {"backend", {{"type", "test_tensor_backend"}}},
                       {"file", "second.bin"}}};
  DeploymentPrepareOptions options;
  options.pipeline_dir = fs::temp_directory_path().string();
  ASSERT_TRUE(
      PrepareDeploymentDocument(models, options, &prepared, &diagnostic))
      << diagnostic.message;
  models["models"][1]["file"] = "../../escaped_second.bin";
  EXPECT_FALSE(
      PrepareDeploymentDocument(models, options, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.path, "/models/1/file");
  EXPECT_TRUE(prepared.inputs.empty());
  EXPECT_TRUE(prepared.outputs.empty());
  EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
  EXPECT_TRUE(prepared.io_boundary.input_published_ports.empty());
  EXPECT_TRUE(prepared.io_boundary.output_consumed_ports.empty());
}

TEST_F(IoConverterRegistryTest,
       OutputPoolBudgetAndDepthRejectOversizedHandles) {
  auto document = TestDocument();
  document["io"]["output"][0]["params"] = {{"entities_json_max_bytes", 65536}};
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string error;
  DeploymentDiagnostic diagnostic;
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(document, "", &plan, &error,
                                                    &diagnostic, 1024),
            -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diagnostic.code, "INVALID_OUTPUT_ALLOCATION");
  EXPECT_EQ(diagnostic.path, "/io/output/0");
  EXPECT_NE(error.find("budget"), std::string::npos);
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(document, "", &plan, &error,
                                                    &diagnostic, 1025),
            -2);
  EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diagnostic.path, "/");
}

TEST_F(IoConverterRegistryTest,
       FileResolverPreservesParseAndParameterDiagnostics) {
  const auto directory =
      fs::temp_directory_path() /
      ("edgeflow_converter_files_" + std::to_string(getpid()));
  fs::create_directories(directory);
  const auto pipeline = directory / "pipeline.json";
  const auto conf = directory / "pipeline.conf";
  std::ofstream(conf) << nlohmann::json{{"pipe_path", "pipeline.json"}};
  std::ofstream(pipeline) << TestDocument();
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string error;
  DeploymentDiagnostic diagnostic;
  ASSERT_EQ(IoPlanResolver::ResolveFromFile(conf.string(), &plan, &error,
                                            &diagnostic),
            0)
      << error;
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(plan->resolved_pipeline_json["io"]["output"][0]["params"],
            nlohmann::json({{"entities_json_max_bytes", 2047}}));
  auto invalid = TestDocument();
  invalid["io"]["output"][0]["params"] = {{"unknown~/size", 17}};
  std::ofstream(pipeline) << invalid;
  EXPECT_EQ(IoPlanResolver::ResolveFromFile(conf.string(), &plan, &error,
                                            &diagnostic),
            -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diagnostic.code, "UNKNOWN_CONFIG_FIELD");
  EXPECT_EQ(diagnostic.path, "/io/output/0/params/unknown~0~1size");
  std::ofstream(pipeline) << "{ malformed JSON";
  EXPECT_EQ(IoPlanResolver::ResolveFromFile(conf.string(), &plan, &error,
                                            &diagnostic),
            -2);
  EXPECT_EQ(diagnostic.code, "JSON_PARSE");
  EXPECT_EQ(diagnostic.path, "/");
  EXPECT_EQ(
      IoPlanResolver::ResolveFromFile((directory / "missing.conf").string(),
                                      &plan, &error, &diagnostic),
      -2);
  EXPECT_EQ(diagnostic.code, "CONFIG_FILE_OPEN");
  std::ofstream(conf) << "{ invalid config";
  EXPECT_EQ(IoPlanResolver::ResolveFromFile(conf.string(), &plan, &error,
                                            &diagnostic),
            -2);
  EXPECT_EQ(diagnostic.code, "JSON_PARSE");
  std::ofstream(conf) << nlohmann::json{{"pipe_path", "missing.json"}};
  EXPECT_EQ(IoPlanResolver::ResolveFromFile(conf.string(), &plan, &error,
                                            &diagnostic),
            -2);
  EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diagnostic.path, "/pipe_path");
  fs::remove_all(directory);
}

TEST_F(IoConverterRegistryTest, PublicIoQueryDoesNotDecodeAndClearsFailures) {
  const auto directory =
      fs::temp_directory_path() /
      ("edgeflow_converter_query_" + std::to_string(getpid()));
  fs::create_directories(directory);
  auto document = TestDocument();
  std::ofstream(directory / "pipeline.json") << document;
  std::ofstream(directory / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  operator_api::OperatorIoContract io;
  char error[512]{};
  const auto root = directory.string();
  const int decode_before = dummy_decode_calls;
  const int encode_before = dummy_encode_calls;
  ASSERT_EQ(operator_api::ResolveOperatorConfigIo(root.c_str(), "pipeline.conf",
                                                  &io, error, sizeof(error)),
            0)
      << error;
  ASSERT_EQ(io.inputs.size(), 1u);
  ASSERT_EQ(io.outputs.size(), 1u);
  EXPECT_EQ(io.inputs.front().type, "entity_in");
  EXPECT_EQ(io.outputs.front().name, "test_service");
  EXPECT_EQ(io.inputs.front().service_type, std::optional<int32_t>{1001});
  EXPECT_EQ(io.outputs.front().service_type, std::optional<int32_t>{1001});
  EXPECT_EQ(dummy_decode_calls, decode_before);
  EXPECT_EQ(dummy_encode_calls, encode_before);
  document["io"]["input"][0]["name"] = "unknown_service";
  std::ofstream(directory / "pipeline.json") << document;
  EXPECT_EQ(operator_api::ResolveOperatorConfigIo(root.c_str(), "pipeline.conf",
                                                  &io, error, sizeof(error)),
            -2);
  EXPECT_TRUE(io.inputs.empty());
  EXPECT_TRUE(io.outputs.empty());
  EXPECT_NE(std::string(error).find("unknown_service"), std::string::npos);
  fs::remove_all(directory);
}

TEST_F(IoConverterRegistryTest, RegistryConflictsStayRecordedWithoutMessage) {
  RegistryConflicts conflicts;
  EXPECT_FALSE(conflicts.HasConflict());
  EXPECT_TRUE(conflicts.Messages().empty());
  conflicts.Record("duplicate");
  EXPECT_TRUE(conflicts.HasConflict());
  EXPECT_EQ(conflicts.Messages(), std::vector<std::string>{"duplicate"});
  conflicts.Clear();
  EXPECT_FALSE(conflicts.HasConflict());

  // 因分配失败丢失消息时，注册表不得重新开放。
  RegistryConflicts lossy;
  std::string message = "lost";
  bool allocation_failed = false;
  {
    test_support::ScopedAllocationFailure failure(0);
    lossy.Record(std::move(message));
    allocation_failed = failure.Triggered();
  }
  EXPECT_TRUE(allocation_failed);
  EXPECT_TRUE(lossy.HasConflict());
  ASSERT_EQ(lossy.Messages().size(), 1u);
  EXPECT_NE(lossy.Messages()[0].find("without a stored message"),
            std::string::npos);
}

TEST_F(IoConverterRegistryTest, DeploymentIoConfigValidation) {
  // 1. 合法 Operator 定位配置 (仅包含 pipe_path)
  nlohmann::json valid_cfg = {{"pipe_path", "test.json"}};

  // 写入临时测试 pipeline 文件
  std::string tmp_dir = "/tmp/edgeflow_test_config_" + std::to_string(getpid());
  fs::create_directories(tmp_dir);
  std::string pipe_path = tmp_dir + "/test.json";
  {
    std::ofstream ofs(pipe_path);
    ofs << "{}";
  }

  DeploymentIoConfig parsed;
  std::string err;
  EXPECT_TRUE(DeploymentIoConfig::Parse(valid_cfg, tmp_dir, &parsed, &err));
  EXPECT_EQ(parsed.pipe_path, "test.json");

  // 3. 拒绝顶层未知字段
  nlohmann::json bad_field = valid_cfg;
  bad_field["extra_field"] = "foo";
  EXPECT_FALSE(DeploymentIoConfig::Parse(bad_field, tmp_dir, &parsed, &err));
  EXPECT_NE(err.find("Unknown field"), std::string::npos);

  // 5. 路径逃逸拒绝
  nlohmann::json escape_cfg = {{"pipe_path", "../../../etc/passwd"}};
  EXPECT_FALSE(DeploymentIoConfig::Parse(escape_cfg, tmp_dir, &parsed, &err));

  // 6. JSON Pointer 转义未知键 (例如 "bad~/field" -> "/bad~0~1field")
  nlohmann::json escaped_key_cfg = valid_cfg;
  escaped_key_cfg["bad~/field"] = 1;
  DeploymentDiagnostic diag;
  EXPECT_FALSE(DeploymentIoConfig::Parse(escaped_key_cfg, tmp_dir, &parsed,
                                         &err, &diag));
  EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diag.path, "/bad~0~1field");

  // 7. ReadFromFile 在解析错误时携带配置文件路径上下文
  const std::string bad_conf_path = tmp_dir + "/bad_config.conf";
  {
    std::ofstream ofs(bad_conf_path);
    ofs << escaped_key_cfg.dump();
  }
  std::string read_err;
  DeploymentDiagnostic read_diag;
  EXPECT_FALSE(DeploymentIoConfig::ReadFromFile(bad_conf_path, &parsed,
                                                &read_err, &read_diag));
  EXPECT_EQ(read_diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(read_diag.path, "/bad~0~1field");
  EXPECT_EQ(read_err.rfind("Error in config file " + bad_conf_path + ": ", 0),
            0);
  EXPECT_EQ(read_diag.message.rfind(
                "Error in config file " + bad_conf_path + ": ", 0),
            0);

  // 8. ReadFromFile 针对未知字段报错同样携带配置文件路径上下文
  const std::string dep_conf_path = tmp_dir + "/unknown_field.conf";
  {
    std::ofstream ofs(dep_conf_path);
    ofs << bad_field.dump();
  }
  EXPECT_FALSE(DeploymentIoConfig::ReadFromFile(dep_conf_path, &parsed,
                                                &read_err, &read_diag));
  EXPECT_EQ(read_diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(read_err.rfind("Error in config file " + dep_conf_path + ": ", 0),
            0);
  EXPECT_EQ(read_diag.message.rfind(
                "Error in config file " + dep_conf_path + ": ", 0),
            0);

  // 9. ReadFromFile 文件路径包含关键保留词 (如 pipe_path.conf)
  // 时，仍必须正确携带文件路径前缀
  const std::string keyword_conf_path = tmp_dir + "/pipe_path.conf";
  {
    std::ofstream ofs(keyword_conf_path);
    ofs << escaped_key_cfg.dump();
  }
  EXPECT_FALSE(DeploymentIoConfig::ReadFromFile(keyword_conf_path, &parsed,
                                                &read_err, &read_diag));
  EXPECT_EQ(read_diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(read_diag.path, "/bad~0~1field");
  EXPECT_EQ(
      read_err.rfind("Error in config file " + keyword_conf_path + ": ", 0), 0);
  EXPECT_EQ(read_diag.message.rfind(
                "Error in config file " + keyword_conf_path + ": ", 0),
            0);

  // 10. 多个 ~ 与 / 字符的转义校验
  nlohmann::json multi_escape_cfg = valid_cfg;
  multi_escape_cfg["a~b/c~0/d~1"] = 42;
  EXPECT_FALSE(DeploymentIoConfig::Parse(multi_escape_cfg, tmp_dir, &parsed,
                                         &err, &diag));
  EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diag.path, "/a~0b~1c~00~1d~01");

  fs::remove_all(tmp_dir);
}

TEST_F(IoConverterRegistryTest,
       StrictConfigDirectoryIsolationAndCwdInvariance) {
  const std::string root_dir =
      "/tmp/edgeflow_test_isolation_" + std::to_string(getpid());
  fs::remove_all(root_dir);

  const fs::path base_dir = fs::path(root_dir) / "service_configs";
  const fs::path outside_dir = fs::path(root_dir) / "outside";
  const fs::path sibling_dir = fs::path(root_dir) / "sibling";
  const fs::path sub_dir = base_dir / "subdir";

  fs::create_directories(base_dir);
  fs::create_directories(outside_dir);
  fs::create_directories(sibling_dir);
  fs::create_directories(sub_dir);

  // 准备各个目标文件
  {
    std::ofstream(base_dir / "pipeline.json") << "{}";
    std::ofstream(sub_dir / "sub_pipeline.json") << "{}";
    std::ofstream(outside_dir / "outside_pipeline.json") << "{}";
    std::ofstream(sibling_dir / "sibling_pipeline.json") << "{}";
  }

  // 创建指向根外文件的符号链接
  std::error_code ec;
  fs::create_symlink(outside_dir / "outside_pipeline.json",
                     base_dir / "symlink_escape.json", ec);
  ASSERT_FALSE(ec) << ec.message();

  DeploymentIoConfig parsed;
  std::string err;

  auto make_conf = [](const std::string& pipe) {
    nlohmann::json cfg = {{"pipe_path", pipe}};
    return cfg;
  };

  // 1. 同级文件 -> 成功
  EXPECT_TRUE(DeploymentIoConfig::Parse(make_conf("pipeline.json"),
                                        base_dir.string(), &parsed, &err));
  EXPECT_EQ(parsed.resolved_pipe_path,
            fs::canonical(base_dir / "pipeline.json").string());

  // 2. 子目录文件 -> 成功
  EXPECT_TRUE(DeploymentIoConfig::Parse(make_conf("subdir/sub_pipeline.json"),
                                        base_dir.string(), &parsed, &err));
  EXPECT_EQ(parsed.resolved_pipe_path,
            fs::canonical(sub_dir / "sub_pipeline.json").string());

  // 3. 父目录逃逸 (../outside/outside_pipeline.json) -> 严格拒绝
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(make_conf("../outside/outside_pipeline.json"),
                                base_dir.string(), &parsed, &err));
  EXPECT_NE(err.find("escapes config directory"), std::string::npos);

  // 4. 兄弟目录逃逸 (../sibling/sibling_pipeline.json) -> 严格拒绝
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(make_conf("../sibling/sibling_pipeline.json"),
                                base_dir.string(), &parsed, &err));
  EXPECT_NE(err.find("escapes config directory"), std::string::npos);

  // 5. 符号链接逃逸 (位于 base_dir 内但指向根外) -> 严格拒绝
  EXPECT_FALSE(DeploymentIoConfig::Parse(make_conf("symlink_escape.json"),
                                         base_dir.string(), &parsed, &err));
  EXPECT_NE(err.find("escapes config directory"), std::string::npos);

  // 6. 切换工作目录不改变解析结果 (Cwd Invariance)
  const fs::path conf_file = base_dir / "deploy.conf";
  {
    std::ofstream ofs(conf_file);
    ofs << make_conf("pipeline.json").dump();
  }

  const fs::path orig_cwd = fs::current_path();
  // 切换工作目录到 outside_dir
  fs::current_path(outside_dir, ec);
  ASSERT_FALSE(ec);

  DeploymentIoConfig cwd_parsed;
  std::string cwd_err;
  bool read_ok = DeploymentIoConfig::ReadFromFile(conf_file.string(),
                                                  &cwd_parsed, &cwd_err);

  // 恢复原工作目录
  fs::current_path(orig_cwd, ec);
  ASSERT_FALSE(ec);

  EXPECT_TRUE(read_ok) << cwd_err;
  EXPECT_EQ(cwd_parsed.resolved_pipe_path,
            fs::canonical(base_dir / "pipeline.json").string());

  fs::remove_all(root_dir);
}

TEST_F(IoConverterRegistryTest, ModelPathResolutionFailurePointsToModelEntry) {
  nlohmann::json doc = {{"io", TestIo()},
                        {"models",
                         {{{"name", "mid~test/path"},
                           {"type", "embedding"},
                           {"backend",
                            {{"type", "test_tensor_backend"},
                             {"params", {{"fixed_batch_size", 4}}}}},
                           {"params", {{"embedding_dim", 128}}},
                           {"file", "../../escaped_model.bin"}}}},
                        {"pipeline", DefaultPipelineNodes()}};
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string err;
  DeploymentDiagnostic diagnostic;
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(doc, "", &plan, &err,
                                                    &diagnostic),
            -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diagnostic.code, "INVALID_FILE_PATH");
  EXPECT_EQ(diagnostic.path, "/models/0/file");
  EXPECT_NE(err.find(diagnostic.path), std::string::npos) << err;
}

TEST_F(IoConverterRegistryTest, ModelPathMissingEmptyOrWrongTypeRejected_T03) {
  const nlohmann::json base_doc = {{"io", TestIo()},
                                   {"models",
                                    {{{"name", "mid_1"},
                                      {"type", "embedding"},
                                      {"backend",
                                       {{"type", "test_tensor_backend"},
                                        {"params", {{"fixed_batch_size", 4}}}}},
                                      {"params", {{"embedding_dim", 128}}}}}},
                                   {"pipeline", DefaultPipelineNodes()}};
  DeploymentPrepareOptions options;

  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  EXPECT_FALSE(PrepareDeploymentDocument(base_doc, options, &prepared, &diag));
  EXPECT_EQ(diag.code, "MISSING_FIELD");
  EXPECT_EQ(diag.path, "/models/0/file");
  for (const auto& value :
       nlohmann::json::array({nullptr, 12345, true, nlohmann::json::object(),
                              nlohmann::json::array(), ""})) {
    SCOPED_TRACE(value.dump());
    auto doc = base_doc;
    doc["models"][0]["file"] = value;
    const auto original = doc;
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code,
              value.is_string() ? "INVALID_FILE_PATH" : "FIELD_TYPE");
    EXPECT_EQ(diag.path, "/models/0/file");
    EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
    EXPECT_EQ(doc, original);
  }
}

TEST_F(IoConverterRegistryTest, ModelStructureInvalidRejected_T04) {
  DeploymentPrepareOptions options;

  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  // 情形 1：models 不是数组 (如对象)
  {
    nlohmann::json doc = {{"io", TestIo()},
                          {"models", {{"mid_1", "not_an_array"}}},
                          {"pipeline", DefaultPipelineNodes()}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "FIELD_TYPE");
    EXPECT_EQ(diag.path, "/models");
  }

  // 情形 2：每个模型条目都必须有 file。
  {
    nlohmann::json doc = {{"io", TestIo()},
                          {"models",
                           {{{"name", "mid_1"},
                             {"type", "embedding"},
                             {"backend", {{"type", "test_tensor_backend"}}}}}},
                          {"pipeline", DefaultPipelineNodes()}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "MISSING_FIELD");
    EXPECT_EQ(diag.path, "/models/0/file");
  }

  // 情形 3：name 重复
  {
    nlohmann::json doc = {{"io", TestIo()},
                          {"models",
                           {{{"name", "mid_1"},
                             {"type", "embedding"},
                             {"backend", {{"type", "test_tensor_backend"}}},
                             {"file", "orig1.bin"}},
                            {{"name", "mid_1"},
                             {"type", "embedding"},
                             {"backend", {{"type", "test_tensor_backend"}}},
                             {"file", "orig2.bin"}}}},
                          {"pipeline", DefaultPipelineNodes()}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "DUPLICATE_MODEL_NAME");
    EXPECT_EQ(diag.path, "/models/1/name");
  }
}

TEST_F(IoConverterRegistryTest, ModelPathNonexistentOnDiskIsAllowed_T05) {
  const auto temp_dir = fs::temp_directory_path() / "edgeflow_test_t05";
  fs::create_directories(temp_dir);
  const auto missing_path = temp_dir / "missing_dir/model.bin";
  ASSERT_FALSE(fs::exists(missing_path));
  const nlohmann::json doc = {{"io", TestIo()},
                              {"models",
                               {{{"name", "mid_1"},
                                 {"type", "embedding"},
                                 {"backend",
                                  {{"type", "test_tensor_backend"},
                                   {"params", {{"fixed_batch_size", 4}}}}},
                                 {"params", {{"embedding_dim", 128}}},
                                 {"file", "missing_dir/model.bin"}}}},
                              {"pipeline", DefaultPipelineNodes()}};
  const auto original = doc;
  DeploymentPrepareOptions options;

  options.pipeline_dir = temp_dir.string();
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;
  ASSERT_TRUE(PrepareDeploymentDocument(doc, options, &prepared, &diag))
      << diag.message;
  EXPECT_EQ(prepared.neutral_pipeline_json["models"][0]["file"],
            fs::weakly_canonical(missing_path).string());
  EXPECT_EQ(doc, original);
  EXPECT_FALSE(fs::exists(missing_path));
  fs::remove_all(temp_dir);
}

}  // namespace llm_edgeflow
