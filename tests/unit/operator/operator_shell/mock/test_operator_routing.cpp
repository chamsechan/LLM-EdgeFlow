#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "adapter/io_converter_registry.h"
#include "adapter/operator/mock/operator_process_binding.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/error_codes.h"
#include "platform_mock/operator_data_types.h"
#include "tests/support/mock_text_carrier.h"

namespace llm_edgeflow {
namespace {

// Routing uses the current registered mock bindings. Business field conversion
// is covered by the real Converter suites; this carrier is only valid routing
// data.
using test_support::MockTextCarrier;

class ConverterRoutingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto& converters = IoConverterRegistry::Instance();
    const auto* entity =
        converters.FindInputConverter("entity_in", "entity_extract");
    const auto* output =
        converters.FindOutputConverter("json_out", "translate");
    ASSERT_NE(entity, nullptr);
    ASSERT_NE(output, nullptr);
    entity_definition = *entity;
    output_definition = *output;
    const auto input_binding =
        OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("entity_in");
    const auto output_binding =
        OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("json_out");
    ASSERT_TRUE(input_binding.has_value());
    ASSERT_TRUE(output_binding.has_value());
    entity_selection.converter = &entity_definition;
    entity_selection.host_binding = *input_binding;
    output_selection.converter = &output_definition;
    output_selection.host_binding = *output_binding;
    entity_inputs = {entity_selection};
    translation_outputs = {output_selection};
  }
  InputConverterDefinition entity_definition;
  OutputConverterDefinition output_definition;
  SelectedInput entity_selection;
  SelectedOutput output_selection;
  std::vector<SelectedInput> entity_inputs;
  std::vector<SelectedOutput> translation_outputs;
};

TEST_F(ConverterRoutingTest, OpaqueHostPrefixMayDifferPerRow) {
  MockTextCarrier first("x");
  MockTextCarrier second("x");
  const operator_api::NamedIoBatch input = {
      {{"host_0.entity_in",
        operator_api::MakeBorrowedOperatorInput(&first.root)}},
      {{"other.entity_in",
        operator_api::MakeBorrowedOperatorInput(&second.root)}}};
  std::vector<ExternalInputBatchView> views;
  std::string error;
  ASSERT_EQ(ValidateAndExtractOperatorInputs(input, entity_inputs, {}, &views,
                                             &error),
            0)
      << error;
  ASSERT_EQ(views.size(), 1U);
  EXPECT_EQ(views[0].count, 2U);
  EXPECT_EQ(views[0].values[0].get(), &first.root);
  EXPECT_EQ(views[0].values[1].get(), &second.root);
}

TEST_F(ConverterRoutingTest,
       InputMissingNullExtraDuplicateAndMalformedKeysAreRejected) {
  MockTextCarrier valid("x");
  struct Case {
    const char* name;
    operator_api::NamedIo row;
    const char* detail;
  };
  const auto value = operator_api::MakeBorrowedOperatorInput(&valid.root);
  const std::vector<Case> cases = {
      {"missing", {}, "Missing required input"},
      {"null", {{"x.entity_in", nullptr}}, "Null input shared_ptr"},
      {"extra",
       {{"x.entity_in", value}, {"x.string", value}},
       "Unknown extra input keys"},
      {"duplicate mapping",
       {{"x.entity_in", value}, {"y.entity_in", value}},
       "Duplicate slot mapping"},
      {"malformed key", {{"entity_in", value}}, "Invalid key format"},
      {"empty prefix", {{".entity_in", value}}, "Invalid key format"},
      {"dotted prefix", {{"a.b.entity_in", value}}, "Invalid key format"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    std::vector<ExternalInputBatchView> views;
    std::string error;
    EXPECT_EQ(ValidateAndExtractOperatorInputs({test.row}, entity_inputs, {},
                                               &views, &error),
              error::kInvalidInput);
    EXPECT_NE(error.find(test.detail), std::string::npos) << error;
  }
}

TEST_F(ConverterRoutingTest,
       OptionalInputMayBeAbsentButPresentValueMustBeValid) {
  entity_definition.slot.required = false;
  std::vector<ExternalInputBatchView> views;
  std::string error;
  ASSERT_EQ(
      ValidateAndExtractOperatorInputs({{}}, entity_inputs, {}, &views, &error),
      0);
  ASSERT_EQ(views.size(), 1U);
  EXPECT_EQ(views[0].count, 1U);
  EXPECT_EQ(views[0].values.size(), 1U);
  EXPECT_EQ(views[0].values[0], nullptr);
  EXPECT_EQ(ValidateAndExtractOperatorInputs({{{"x.entity_in", nullptr}}},
                                             entity_inputs, {}, &views, &error),
            error::kInvalidInput);
}

TEST_F(ConverterRoutingTest, OutputsRequireEmptyDeclaredSlotsAndRejectExtras) {
  const auto nonempty = std::make_shared<int>(1);
  struct Case {
    const char* name;
    operator_api::NamedIo row;
    const char* detail;
  };
  const std::vector<Case> cases = {
      {"missing required", {}, "Missing required output"},
      {"nonempty",
       {{"x.json_out", nonempty}},
       "Output shared_ptr must be null"},
      {"extra",
       {{"x.json_out", nullptr}, {"x.string", nullptr}},
       "Unknown extra output keys"},
      {"duplicate mapping",
       {{"x.json_out", nullptr}, {"y.json_out", nullptr}},
       "Duplicate slot mapping"},
      {"invalid key", {{"json_out", nullptr}}, "Invalid key format"},
      {"dotted prefix", {{"a.b.json_out", nullptr}}, "Invalid key format"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    std::vector<std::vector<FrameOutputBinding>> bindings;
    std::string error;
    EXPECT_EQ(ResolveOperatorOutputs({test.row}, translation_outputs, &bindings,
                                     &error),
              error::kInvalidOutput);
    EXPECT_NE(error.find(test.detail), std::string::npos) << error;
  }
}

TEST_F(ConverterRoutingTest,
       OptionalOutputOmissionRetainsRowsAndExistingSlotIndexes) {
  output_definition.slot.required = false;
  const operator_api::NamedIoBatch output = {
      {}, {{"arbitrary.json_out", nullptr}}, {}};
  std::vector<std::vector<FrameOutputBinding>> bindings;
  std::string error;
  ASSERT_EQ(
      ResolveOperatorOutputs(output, translation_outputs, &bindings, &error),
      0);
  ASSERT_EQ(bindings.size(), 3U);
  EXPECT_TRUE(bindings[0].empty());
  ASSERT_EQ(bindings[1].size(), 1U);
  EXPECT_EQ(bindings[1][0].key, "arbitrary.json_out");
  EXPECT_EQ(bindings[1][0].output_index, 0U);
  EXPECT_TRUE(bindings[2].empty());
}

}  // namespace
}  // namespace llm_edgeflow
