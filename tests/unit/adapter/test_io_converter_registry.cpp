#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "adapter/converter_authoring.h"
#include "adapter/deployment_preparation.h"
#include "adapter/io_catalog.h"
#include "adapter/io_converter_registry.h"
#include "adapter/io_values.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;
constexpr auto kValue = MakeBlackboardKey<TextBatch>("value");
constexpr auto kAnswer = MakeBlackboardKey<TextBatch>("answer");
struct InputParams {
  int gain;
};
struct OutputParams {
  int64_t result_json_max_bytes;
};
struct LayoutParams {
  int lane;
};

int IgnoreInput(const ExternalInputBatchView&, const InputDecodeOptions&,
                AlgContext*, AdapterStatus*) {
  return 0;
}
int IgnoreOutput(AlgContext*, const OutputEncodeOptions&,
                 ExternalOutputBatchView*, size_t*, AdapterStatus*) {
  return 0;
}

InputConverterDefinition Input(const std::string& name) {
  InputConverterDefinition def;
  def.type = "string";
  def.name = name;
  def.slot = ExternalInputSlot<std::string>();
  def.logical_ports = {OutputPort(kValue)};
  def.params =
      Parameters<InputParams>{Field("gain", &InputParams::gain).Default(3)};
  def.decode_fn = IgnoreInput;
  return def;
}
OutputConverterDefinition Output(const std::string& name) {
  OutputConverterDefinition def;
  def.type = "json_out";
  def.name = name;
  def.slot = ExternalOutputSlot<JsonOutputValue>();
  def.logical_ports = {RequiredInputPort(kAnswer)};
  def.params = Parameters<OutputParams>{
      MaxBytes("result_json", &OutputParams::result_json_max_bytes)
          .Default(31)};
  def.encode_fn = IgnoreOutput;
  return def;
}

TEST(IoConverterRegistryTest,
     CatalogTypesResolveBindingsAndJsonCapacityLimits) {
  const auto catalog = IoCatalog::ToJson();
  auto& registry = IoConverterRegistry::Instance();
  for (const auto* direction : {"input_converters", "output_converters"}) {
    ASSERT_FALSE(catalog.at(direction).empty());
    for (const auto& converter : catalog.at(direction)) {
      const auto type = converter.at("type").get<std::string>();
      const auto* binding =
          OperatorValueTypeRegistry::Instance().GetBindingBySuffix(type);
      ASSERT_NE(binding, nullptr) << type;
      EXPECT_EQ(converter.at("slot").at("type_id"),
                binding->external_c_type_name);
    }
  }
  for (const auto& converter : catalog.at("output_converters")) {
    if (converter.at("type") != "json_out") continue;
    SCOPED_TRACE(converter.at("name").get<std::string>());
    const auto* definition = registry.FindOutputConverter(
        "json_out", converter.at("name").get<std::string>());
    ASSERT_NE(definition, nullptr);
    std::shared_ptr<const ParameterValues> values;
    ASSERT_TRUE(definition->params.Parse(Json::object(), &values));
    EXPECT_EQ(values->Effective().at("result_json_max_bytes"), 8191);
    const auto fields = OutputConverterParameterFields(*definition);
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields.front().name, "result_json_max_bytes");
    EXPECT_EQ(fields.front().minimum, 1);
    EXPECT_EQ(fields.front().maximum, 65536);
  }
}

TEST(IoConverterRegistryTest,
     ExactPairsHaveNoCommonFallbackAndDeclarationsAreOwned) {
  EXPECT_EXIT(
      ([] {
        auto& registry = IoConverterRegistry::Instance();
        test_support::RegistryTestAccess::ResetConverters();
        auto input = Input("common");
        ASSERT_TRUE(registry.RegisterInputConverter(input));
        ASSERT_TRUE(registry.RegisterInputConverter(Input("special")));
        auto output = Output("common");
        ASSERT_TRUE(registry.RegisterOutputConverter(output));
        ASSERT_TRUE(registry.RegisterOutputConverter(Output("translate")));
        input.type = "changed";
        input.logical_ports.clear();
        input.params = {};
        output.slot.required = false;
        output.logical_ports.clear();
        ASSERT_NE(registry.FindInputConverter("string", "common"), nullptr);
        EXPECT_EQ(registry.FindInputConverter("string", "unknown"), nullptr);
        EXPECT_EQ(registry.FindInputConverter("other", "common"), nullptr);
        EXPECT_EQ(registry.FindOutputConverter("json_out", "unknown"), nullptr);
        const auto* owned = registry.FindInputConverter("string", "common");
        EXPECT_EQ(owned->type, "string");
        ASSERT_EQ(owned->logical_ports.size(), 1u);
        std::shared_ptr<const ParameterValues> values;
        ASSERT_TRUE(owned->params.Parse(Json::object(), &values));
        EXPECT_EQ(values->Effective(), (Json{{"gain", 3}}));
        EXPECT_TRUE(
            registry.FindOutputConverter("json_out", "common")->slot.required);
        auto snapshot = registry.AllInputConverters();
        ASSERT_EQ(snapshot.size(), 2u);
        EXPECT_EQ(snapshot[0].name, "common");
        EXPECT_EQ(snapshot[1].name, "special");
        snapshot[0].logical_ports.clear();
        EXPECT_EQ(owned->logical_ports.size(), 1u);
        _exit(::testing::Test::HasFailure() ? 1 : 0);
      }()),
      ::testing::ExitedWithCode(0), ".*");
}

TEST(IoConverterRegistryTest,
     InvalidAndDuplicateRegistrationsKeepOriginalEntriesAndStickyConflict) {
  EXPECT_EXIT(([] {
                auto& registry = IoConverterRegistry::Instance();
                test_support::RegistryTestAccess::ResetConverters();
                ASSERT_TRUE(registry.RegisterInputConverter(Input("common")));
                auto duplicate = Input("common");
                duplicate.logical_ports.clear();
                EXPECT_FALSE(registry.RegisterInputConverter(duplicate));
                auto invalid = Output("missing_callback");
                invalid.encode_fn = nullptr;
                EXPECT_FALSE(registry.RegisterOutputConverter(invalid));
                EXPECT_EQ(registry.AllInputConverters().size(), 1u);
                EXPECT_TRUE(registry.AllOutputConverters().empty());
                EXPECT_EQ(registry.FindInputConverter("string", "common")
                              ->logical_ports.size(),
                          1u);
                EXPECT_TRUE(registry.HasConflict());
                EXPECT_EQ(registry.GetConflictErrors().size(), 2u);
                std::vector<std::string> errors;
                EXPECT_FALSE(registry.Audit(&errors));
                EXPECT_GE(errors.size(), 2u);
                _exit(::testing::Test::HasFailure() ? 1 : 0);
              }()),
              ::testing::ExitedWithCode(0), ".*");
}

TEST(IoConverterRegistryTest,
     AuditRejectsUnusedOutputWithInvalidNativeCapacityDefault) {
  EXPECT_EXIT(
      ([] {
        auto& registry = IoConverterRegistry::Instance();
        test_support::RegistryTestAccess::ResetConverters();
        ASSERT_TRUE(registry.RegisterOutputConverter(Output("common")));
        auto unused = Output("translate");
        unused.params = Parameters<OutputParams>{
            MaxBytes("result_json", &OutputParams::result_json_max_bytes)
                .Default(70000)};
        ASSERT_TRUE(registry.RegisterOutputConverter(unused));
        std::vector<std::string> errors;
        EXPECT_FALSE(registry.Audit(&errors));
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find("json_out/translate"), std::string::npos);
        EXPECT_NE(
            errors.front().find(
                "Missing valid integer size default: result_json_max_bytes"),
            std::string::npos);
        _exit(::testing::Test::HasFailure() ? 1 : 0);
      }()),
      ::testing::ExitedWithCode(0), ".*");
}

TEST(IoConverterRegistryTest,
     AuditChecksSlotIdentityPortsAndNamedAllocatorEvenWhenUnused) {
  EXPECT_EXIT(
      ([] {
        struct InvalidAudit {
          const char* label;
          InputConverterDefinition input;
          OutputConverterDefinition output;
          const char* error;
        };
        std::vector<InvalidAudit> cases;
        auto missing = Input("common");
        missing.type = "unregistered";
        cases.push_back({"missing binding", missing, Output("common"),
                         "No registered platform binding for the slot"});
        auto value_type = Input("common");
        value_type.slot.value_type = typeid(int);
        cases.push_back({"value identity", value_type, Output("common"),
                         "Slot value type does not match"});
        auto direction = Input("common");
        direction.type = "json_out";
        cases.push_back({"direction", direction, Output("common"),
                         "No registered platform binding for the slot"});
        auto ports = Input("common");
        ports.logical_ports.push_back(ports.logical_ports.front());
        cases.push_back({"duplicate port", ports, Output("common"),
                         "Duplicate logical port: value"});
        auto qualified_port = Input("common");
        qualified_port.logical_ports.front().logical_name = "bad.name";
        cases.push_back({"qualified logical name", qualified_port,
                         Output("common"),
                         "Logical port name cannot contain '.'"});
        auto allocator = Output("common");
        allocator.slot.allocator = "absent_allocator";
        cases.push_back(
            {"allocator", Input("common"), allocator,
             "No registered platform binding for the output allocator"});
        auto metadata = Output("common");
        metadata.slot.metadata_count = 1;
        cases.push_back({"metadata", Input("common"), metadata,
                         "Invalid fixed metadata count or type"});
        auto& registry = IoConverterRegistry::Instance();
        for (const auto& invalid : cases) {
          SCOPED_TRACE(invalid.label);
          test_support::RegistryTestAccess::ResetConverters();
          ASSERT_TRUE(registry.RegisterInputConverter(invalid.input));
          ASSERT_TRUE(registry.RegisterOutputConverter(invalid.output));
          std::vector<std::string> errors;
          EXPECT_FALSE(registry.Audit(&errors));
          ASSERT_FALSE(errors.empty());
          EXPECT_NE(errors.front().find(invalid.error), std::string::npos)
              << errors.front();
        }
        _exit(::testing::Test::HasFailure() ? 1 : 0);
      }()),
      ::testing::ExitedWithCode(0), ".*");
}

TEST(IoConverterRegistryTest,
     AuditNormalizesAllocatorOnceAndPreparationKeepsHostAndParameterSnapshots) {
  EXPECT_EXIT(
      ([] {
        test_support::RegistryTestAccess::ScopedValueTypeState value_state;
        auto& registry = IoConverterRegistry::Instance();
        test_support::RegistryTestAccess::ResetConverters();
        int normalization_calls = 0;
        int allocation_calls = 0;
        auto binding = OperatorValueTypeRegistry::Instance().CopyOutputBinding(
            "json_out", "");
        ASSERT_TRUE(binding);
        binding->normalize_parameters = MakeOutputParameterParser<LayoutParams>(
            [&](const std::string& text, LayoutParams* params, std::string*) {
              ++normalization_calls;
              params->lane = Json::parse(text).at("lane").get<int>();
              return true;
            });
        binding->allocate_external = [&](const ResolvedOutputPoolSpec&,
                                         OwnedExternalBlock*, std::string*) {
          ++allocation_calls;
          return -1;
        };
        ASSERT_TRUE(
            RegisterOperatorOutputAllocator("registry_layout", *binding));
        ASSERT_TRUE(registry.RegisterInputConverter(Input("common")));
        auto output = Output("common");
        output.slot.allocator = "registry_layout";
        output.slot.allocator_params = R"({"lane":7})";
        ASSERT_TRUE(registry.RegisterOutputConverter(output));
        output.slot.allocator_params = R"({"lane":99})";
        std::vector<std::string> errors;
        ASSERT_TRUE(registry.Audit(&errors));
        const auto cached = registry.OutputParameters("json_out", "common");
        ASSERT_TRUE(registry.Audit(&errors));
        EXPECT_EQ(normalization_calls, 1);
        EXPECT_EQ(registry.OutputParameters("json_out", "common"), cached);
        Json document = {
            {"io",
             {{"input", Json::array({{{"type", "string"},
                                      {"name", "common"},
                                      {"params", {{"gain", 12}}}}})},
              {"output",
               Json::array({{{"type", "json_out"},
                             {"name", "common"},
                             {"params", {{"result_json_max_bytes", 17}}},
                             {"inputs", {{"answer", "render.text"}}}}})}}},
            {"pipeline",
             Json::array({{{"type", "text_template"},
                           {"name", "render"},
                           {"inputs", {{"primary", "input.value"}}}}})}};
        PreparedDeployment prepared;
        DeploymentDiagnostic diagnostic;
        ASSERT_TRUE(
            PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
            << diagnostic.message;
        EXPECT_EQ(normalization_calls, 1);
        EXPECT_EQ(allocation_calls, 0);
        ASSERT_EQ(prepared.inputs.size(), 1u);
        ASSERT_EQ(prepared.outputs.size(), 1u);
        EXPECT_EQ(prepared.inputs[0].params->Effective(), (Json{{"gain", 12}}));
        EXPECT_EQ(prepared.outputs[0].params->Effective(),
                  (Json{{"result_json_max_bytes", 17}}));
        EXPECT_EQ(prepared.outputs[0].pool_spec.GetCapacity("result_json"),
                  17u);
        EXPECT_EQ(prepared.outputs[0].pool_spec.params, cached);
        EXPECT_EQ(prepared.outputs[0].pool_spec.Parameters<LayoutParams>().lane,
                  7);
        document["io"]["output"][0]["params"]["result_json_max_bytes"] = 99;
        EXPECT_EQ(prepared.outputs[0].params->Integer("result_json_max_bytes"),
                  17);
        auto replacement =
            *OperatorValueTypeRegistry::Instance().CopyBindingBySuffix(
                "json_out");
        const auto translate = replacement.ServiceType("translate");
        ASSERT_TRUE(translate);
        replacement.services["translate"] = 1234;
        test_support::RegistryTestAccess::SetValueBinding(
            std::move(replacement));
        EXPECT_EQ(prepared.outputs[0].host_binding.ServiceType("translate"),
                  translate);
        EXPECT_EQ(
            prepared.outputs[0].allocator_binding.ServiceType("translate"),
            translate);
        _exit(::testing::Test::HasFailure() ? 1 : 0);
      }()),
      ::testing::ExitedWithCode(0), ".*");
}

}  // namespace
}  // namespace llm_edgeflow
