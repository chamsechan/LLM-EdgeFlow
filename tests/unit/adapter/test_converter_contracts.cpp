#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "adapter/converter_authoring.h"
#include "adapter/operator/mock/operator_process_binding.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow {
namespace {

constexpr auto kRows = MakeBlackboardKey<TextBatch>("rows");

struct RowCarrier {
  std::string text;
  bool valid = true;
};

class ConverterRowsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    input_binding.external_c_type_name = "RowCarrier";
    input_binding.validate_external = [](const void* raw, const InputLimits&,
                                         std::string* error) {
      if (!static_cast<const RowCarrier*>(raw)->valid) {
        *error = "carrier rejected";
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      return 0;
    };
    SetInputValue<RowCarrier, TextInputValue>(
        &input_binding,
        [](const RowCarrier& row) { return TextInputValue{row.text}; });
    output_binding.external_c_type_name = "RowCarrier";
    SetOutputValue<RowCarrier, TextInputValue>(
        &output_binding, [](RowCarrier& row, const TextInputValue& value,
                            const ResolvedOutputPoolSpec&) {
          row.text = value.sentence_text;
          return AdapterStatus::Ok();
        });
  }
  ExternalInputBatchView Input(
      const std::vector<std::shared_ptr<RowCarrier>>& rows) const {
    ExternalInputBatchView view;
    view.count = rows.size();
    view.binding = &input_binding;
    view.slot_types["in"] = "RowCarrier";
    for (const auto& row : rows) view.slots["in"].push_back(row);
    return view;
  }
  ExternalOutputBatchView Output(std::vector<RowCarrier>* rows) const {
    ExternalOutputBatchView view;
    view.count = rows->size();
    view.binding = &output_binding;
    view.slot_types["out"] = "RowCarrier";
    view.pool_specs["out"] = &pool;
    for (auto& row : *rows) view.leased_slots["out"].push_back(&row);
    return view;
  }
  int Decode(const ExternalInputBatchView& source, AlgContext* context,
             AdapterStatus* status, int* calls) const {
    const InputDecodeOptions options{"in", "row_contract", nullptr, &ports};
    return DecodeRequestRows<TextInputValue>(
        source, options, context, status, "in", kRows,
        [context, calls](const TextInputValue& value, std::string* text) {
          ++*calls;
          EXPECT_FALSE(context->Has("bound.rows"));
          if (value.sentence_text == "reject")
            return AdapterStatus::InvalidInput("row conversion rejected",
                                               "text");
          *text = value.sentence_text;
          return AdapterStatus::Ok();
        });
  }
  int Encode(AlgContext* context, ExternalOutputBatchView* destination,
             size_t* written, AdapterStatus* status, int* calls) const {
    const OutputEncodeOptions options{"out", "row_contract", nullptr, &ports};
    return EncodeResultRows<TextInputValue>(
        context, options, destination, written, status, "out", kRows,
        [calls](const std::string& text, TextInputValue* value) {
          ++*calls;
          value->sentence_text = text;
          return AdapterStatus::Ok();
        });
  }
  IoPortBindings ports{{"rows", "bound.rows"}};
  OperatorValueTypeBinding input_binding;
  OperatorValueTypeBinding output_binding;
  ResolvedOutputPoolSpec pool;
};

TEST_F(ConverterRowsTest, DecodeRequiresContextAndAtLeastOneRequest) {
  AdapterStatus status;
  int calls = 0;
  auto source = Input({std::make_shared<RowCarrier>(RowCarrier{"A"})});
  EXPECT_EQ(Decode(source, nullptr, &status, &calls),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "context");
  EXPECT_EQ(calls, 0);
  AlgContext context;
  source.count = 0;
  EXPECT_EQ(Decode(source, &context, &status, &calls),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "slots");
  EXPECT_EQ(calls, 0);
  EXPECT_FALSE(context.Has("bound.rows"));
}

TEST_F(ConverterRowsTest, DecodePublishesOnlyAfterEveryRowAndOwnsValues) {
  auto first = std::make_shared<RowCarrier>(RowCarrier{"first"});
  auto second = std::make_shared<RowCarrier>(RowCarrier{"second"});
  auto source = Input({first, second});
  AlgContext context;
  AdapterStatus status;
  int calls = 0;
  ASSERT_EQ(Decode(source, &context, &status, &calls), 0);
  EXPECT_EQ(calls, 2);
  first->text = "changed";
  source.slots.clear();
  second.reset();
  const auto* rows = context.Read<TextBatch>("bound.rows");
  ASSERT_NE(rows, nullptr);
  ASSERT_EQ(rows->size(), 2U);
  EXPECT_EQ((*rows)[0].req_id, 0U);
  EXPECT_EQ((*rows)[0].sub_id, 0U);
  EXPECT_EQ((*rows)[0].data, "first");
  EXPECT_EQ((*rows)[1].req_id, 1U);
  EXPECT_EQ((*rows)[1].sub_id, 0U);
  EXPECT_EQ((*rows)[1].data, "second");
}

TEST_F(ConverterRowsTest, LaterNullInvalidOrRejectedRowPublishesNothing) {
  struct Case {
    const char* name;
    std::shared_ptr<RowCarrier> second;
    int calls;
    const char* detail;
  };
  const std::vector<Case> cases = {
      {"null", nullptr, 1, "Missing or mismatched input binding"},
      {"invalid carrier", std::make_shared<RowCarrier>(RowCarrier{"B", false}),
       1, "carrier rejected"},
      {"row conversion", std::make_shared<RowCarrier>(RowCarrier{"reject"}), 2,
       "row conversion rejected"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto source =
        Input({std::make_shared<RowCarrier>(RowCarrier{"A"}), test.second});
    AlgContext context;
    AdapterStatus status;
    int calls = 0;
    EXPECT_EQ(Decode(source, &context, &status, &calls),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(calls, test.calls);
    EXPECT_FALSE(context.Has("bound.rows"));
    EXPECT_EQ(status.SampleIndex(), 1);
    EXPECT_EQ(status.AdapterName(), "in/row_contract");
    EXPECT_EQ(status.Message(), test.detail);
  }
}

TEST_F(ConverterRowsTest, UnreferencedInputPortDoesNotPublish) {
  ports.clear();
  auto source = Input({std::make_shared<RowCarrier>(RowCarrier{"A"})});
  AlgContext context;
  AdapterStatus status;
  int calls = 0;
  ASSERT_EQ(Decode(source, &context, &status, &calls), 0);
  EXPECT_FALSE(context.Has("bound.rows"));
  EXPECT_FALSE(context.Has(""));
}

TEST_F(ConverterRowsTest, EncodeRequiresContextAndConnectedResult) {
  std::vector<RowCarrier> rows{{"untouched"}};
  auto destination = Output(&rows);
  AdapterStatus status;
  size_t written = 99;
  int calls = 0;
  EXPECT_EQ(Encode(nullptr, &destination, &written, &status, &calls),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "context");
  EXPECT_EQ(written, 0U);
  AlgContext context;
  EXPECT_EQ(Encode(&context, &destination, &written, &status, &calls),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "res");
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(rows[0].text, "untouched");
}

TEST_F(ConverterRowsTest, EncodeRestoresRequestOrderBeforeCallingRowEncoder) {
  AlgContext context;
  ASSERT_TRUE(
      context.Publish("bound.rows", TextBatch{{1, 0, "B"}, {0, 0, "A"}}));
  std::vector<RowCarrier> rows(2);
  auto destination = Output(&rows);
  AdapterStatus status;
  size_t written = 99;
  int calls = 0;
  ASSERT_EQ(Encode(&context, &destination, &written, &status, &calls), 0);
  EXPECT_EQ(written, 2U);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(rows[0].text, "A");
  EXPECT_EQ(rows[1].text, "B");
}

TEST_F(ConverterRowsTest, EncodeRejectsInvalidProvenanceBeforeAnyRowWrite) {
  struct Case {
    const char* name;
    TextBatch batch;
  };
  const std::vector<Case> cases = {
      {"missing request", {{0, 0, "A"}}},
      {"duplicate complete key", {{0, 0, "A"}, {0, 0, "B"}}},
      {"out of range request", {{0, 0, "A"}, {2, 0, "B"}}},
      {"normal result requires sub zero", {{0, 1, "A"}, {1, 0, "B"}}},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    AlgContext context;
    ASSERT_TRUE(context.Publish("bound.rows", test.batch));
    std::vector<RowCarrier> rows{{"untouched A"}, {"untouched B"}};
    auto destination = Output(&rows);
    AdapterStatus status;
    size_t written = 99;
    int calls = 0;
    EXPECT_EQ(Encode(&context, &destination, &written, &status, &calls),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(written, 0U);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(rows[0].text, "untouched A");
    EXPECT_EQ(rows[1].text, "untouched B");
    EXPECT_EQ(status.FieldPath(), "res");
    EXPECT_EQ(status.Message(),
              "Missing, duplicate or invalid result provenance");
  }
}

TEST_F(ConverterRowsTest, OptionalMissingOutputRowRetainsOtherRowPosition) {
  AlgContext context;
  ASSERT_TRUE(
      context.Publish("bound.rows", TextBatch{{1, 0, "B"}, {0, 0, "A"}}));
  std::vector<RowCarrier> rows{{"unused"}, {"before"}};
  auto destination = Output(&rows);
  destination.required = false;
  destination.leased_slots["out"][0] = nullptr;
  AdapterStatus status;
  size_t written = 0;
  int calls = 0;
  ASSERT_EQ(Encode(&context, &destination, &written, &status, &calls), 0);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(rows[0].text, "unused");
  EXPECT_EQ(rows[1].text, "B");
}

TEST(ConverterIndexTest, RankedResultsAllowUniqueChildrenAndRequireEachAnchor) {
  const TextBatch batch{
      {1, 7, "child"}, {0, 0, "A"}, {1, 0, "B"}, {0, 9, "other"}};
  std::vector<const TextBatch::value_type*> ordered;
  AdapterStatus status;
  ASSERT_TRUE(
      IndexResults(&batch, 2, &ordered, "ranked", "contract", &status, true));
  ASSERT_EQ(ordered.size(), 2U);
  EXPECT_EQ(ordered[0], &batch[1]);
  EXPECT_EQ(ordered[1], &batch[2]);
  EXPECT_EQ(ordered[0]->data, "A");
  EXPECT_EQ(ordered[1]->data, "B");
  const std::vector<TextBatch> invalid = {
      {{0, 0, "A"}, {1, 7, "no anchor"}},
      {{0, 0, "A"}, {1, 0, "B"}, {1, 7, "x"}, {1, 7, "y"}},
      {{0, 0, "A"}, {1, 0, "B"}, {2, 3, "outside"}}};
  for (const auto& input : invalid) {
    EXPECT_FALSE(
        IndexResults(&input, 2, &ordered, "ranked", "contract", &status, true));
    EXPECT_EQ(status.FieldPath(), "ranked");
  }
}

// Routing uses the current registered mock bindings. Business field conversion
// is covered by the real Converter suites; this carrier is only valid routing
// data.
struct EntityCarrier {
  explicit EntityCarrier(int service) : text{1, bytes}, value{service, &text} {}
  char bytes[2] = "x";
  CompanyString text;
  CompanyOperatorEntityInput value;
};

class ConverterRoutingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto& converters = IoConverterRegistry::Instance();
    const auto* entity =
        converters.FindInputConverter("entity_in", "entity_extract");
    const auto* translate =
        converters.FindInputConverter("entity_in", "translate");
    const auto* output =
        converters.FindOutputConverter("json_out", "translate");
    ASSERT_NE(entity, nullptr);
    ASSERT_NE(translate, nullptr);
    ASSERT_NE(output, nullptr);
    entity_definition = *entity;
    translate_definition = *translate;
    output_definition = *output;
    const auto input_binding =
        OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("entity_in");
    const auto output_binding =
        OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("json_out");
    ASSERT_TRUE(input_binding.has_value());
    ASSERT_TRUE(output_binding.has_value());
    entity_selection.converter = &entity_definition;
    entity_selection.host_binding = *input_binding;
    translate_selection.converter = &translate_definition;
    translate_selection.host_binding = *input_binding;
    output_selection.converter = &output_definition;
    output_selection.host_binding = *output_binding;
    entity_inputs = {entity_selection};
    translation_outputs = {output_selection};
  }
  InputConverterDefinition entity_definition;
  InputConverterDefinition translate_definition;
  OutputConverterDefinition output_definition;
  SelectedInput entity_selection;
  SelectedInput translate_selection;
  SelectedOutput output_selection;
  std::vector<SelectedInput> entity_inputs;
  std::vector<SelectedOutput> translation_outputs;
};

TEST(ConverterKeyTest, LastDotSeparatesSuffixFromNonemptyNamespace) {
  std::string prefix;
  std::string suffix;
  ASSERT_TRUE(OperatorValueTypeRegistry::ParseKey("tenant.camera.entity_in",
                                                  &prefix, &suffix));
  EXPECT_EQ(prefix, "tenant.camera");
  EXPECT_EQ(suffix, "entity_in");
  for (const std::string& key :
       {std::string(), std::string("entity_in"), std::string(".entity_in"),
        std::string("tenant.")}) {
    EXPECT_FALSE(OperatorValueTypeRegistry::ParseKey(key, &prefix, &suffix));
  }
}

TEST_F(ConverterRoutingTest, UniqueTypeAcceptsDifferentNamespacesPerRow) {
  EntityCarrier first(kMockServiceEntityExtract);
  EntityCarrier second(kMockServiceEntityExtract);
  const operator_api::NamedIoBatch input = {
      {{"tenant.camera.entity_in",
        operator_api::MakeBorrowedOperatorInput(&first.value)}},
      {{"other.entity_in",
        operator_api::MakeBorrowedOperatorInput(&second.value)}}};
  std::vector<ExternalInputBatchView> views;
  std::string error;
  ASSERT_EQ(ValidateAndExtractOperatorInputs(input, entity_inputs, {}, &views,
                                             &error),
            0)
      << error;
  ASSERT_EQ(views.size(), 1U);
  EXPECT_EQ(views[0].count, 2U);
  EXPECT_EQ(views[0].slots.at("entity_in")[0].get(), &first.value);
  EXPECT_EQ(views[0].slots.at("entity_in")[1].get(), &second.value);
}

TEST_F(ConverterRoutingTest,
       RepeatedTypeRequiresConverterNameAndPreservesSelections) {
  EntityCarrier entity(kMockServiceEntityExtract);
  EntityCarrier translate(kMockServiceTranslate);
  operator_api::NamedIoBatch input = {
      {{"entity_extract.entity_in",
        operator_api::MakeBorrowedOperatorInput(&entity.value)},
       {"translate.entity_in",
        operator_api::MakeBorrowedOperatorInput(&translate.value)}}};
  std::vector<ExternalInputBatchView> views;
  std::string error;
  const std::vector<SelectedInput> selected{entity_selection,
                                            translate_selection};
  ASSERT_EQ(
      ValidateAndExtractOperatorInputs(input, selected, {}, &views, &error), 0)
      << error;
  ASSERT_EQ(views.size(), 2U);
  EXPECT_EQ(views[0].slots.at("entity_in")[0].get(), &entity.value);
  EXPECT_EQ(views[1].slots.at("entity_in")[0].get(), &translate.value);
  input[0].erase("translate.entity_in");
  input[0]["arbitrary.entity_in"] =
      operator_api::MakeBorrowedOperatorInput(&translate.value);
  EXPECT_EQ(
      ValidateAndExtractOperatorInputs(input, selected, {}, &views, &error),
      -3);
  EXPECT_NE(error.find("Missing required input entity_in/translate"),
            std::string::npos);
}

TEST_F(ConverterRoutingTest,
       InputMissingNullWrongServiceExtraAndDuplicateAreRejected) {
  EntityCarrier valid(kMockServiceEntityExtract);
  EntityCarrier wrong_service(kMockServiceTranslate);
  struct Case {
    const char* name;
    operator_api::NamedIo row;
    const char* detail;
  };
  const auto value = operator_api::MakeBorrowedOperatorInput(&valid.value);
  const std::vector<Case> cases = {
      {"missing", {}, "Missing required input"},
      {"null", {{"x.entity_in", nullptr}}, "Null input shared_ptr"},
      {"service mismatch",
       {{"x.entity_in",
         operator_api::MakeBorrowedOperatorInput(&wrong_service.value)}},
       "expected 2"},
      {"extra",
       {{"x.entity_in", value}, {"x.string", value}},
       "Unknown extra input keys"},
      {"duplicate mapping",
       {{"x.entity_in", value}, {"y.entity_in", value}},
       "Duplicate slot mapping"},
      {"malformed key", {{"entity_in", value}}, "Invalid key format"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    std::vector<ExternalInputBatchView> views;
    std::string error;
    EXPECT_EQ(ValidateAndExtractOperatorInputs({test.row}, entity_inputs, {},
                                               &views, &error),
              -3);
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
  EXPECT_EQ(views[0].slots.at("entity_in").size(), 1U);
  EXPECT_EQ(views[0].slots.at("entity_in")[0], nullptr);
  EXPECT_EQ(ValidateAndExtractOperatorInputs({{{"x.entity_in", nullptr}}},
                                             entity_inputs, {}, &views, &error),
            -3);
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
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    std::vector<std::vector<FrameOutputBinding>> bindings;
    std::string error;
    EXPECT_EQ(ResolveOperatorOutputs({test.row}, translation_outputs, &bindings,
                                     &error),
              -4);
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

TEST_F(ConverterRoutingTest,
       RepeatedOutputTypeUsesNamesToChooseSelectionIndexes) {
  const auto* second_definition =
      IoConverterRegistry::Instance().FindOutputConverter("json_out",
                                                          "entity_extract");
  ASSERT_NE(second_definition, nullptr);
  auto second = output_selection;
  second.converter = second_definition;
  std::vector<std::vector<FrameOutputBinding>> bindings;
  std::string error;
  ASSERT_EQ(
      ResolveOperatorOutputs({{{"translate.json_out", nullptr},
                               {"entity_extract.json_out", nullptr}}},
                             {output_selection, second}, &bindings, &error),
      0);
  ASSERT_EQ(bindings[0].size(), 2U);
  EXPECT_EQ(bindings[0][0].key, "translate.json_out");
  EXPECT_EQ(bindings[0][0].output_index, 0U);
  EXPECT_EQ(bindings[0][1].key, "entity_extract.json_out");
  EXPECT_EQ(bindings[0][1].output_index, 1U);
  EXPECT_EQ(
      ResolveOperatorOutputs({{{"arbitrary.json_out", nullptr},
                               {"entity_extract.json_out", nullptr}}},
                             {output_selection, second}, &bindings, &error),
      -4);
  EXPECT_NE(error.find("Missing required output json_out/translate"),
            std::string::npos);
}

}  // namespace
}  // namespace llm_edgeflow
