#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "adapter/converter_authoring.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/error_codes.h"

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
        return error::kInvalidInput;
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
    view.slot = "in";
    view.external_c_type_name = "RowCarrier";
    for (const auto& row : rows) view.values.push_back(row);
    return view;
  }
  ExternalOutputBatchView Output(std::vector<RowCarrier>* rows) const {
    ExternalOutputBatchView view;
    view.count = rows->size();
    view.binding = &output_binding;
    view.slot = "out";
    view.external_c_type_name = "RowCarrier";
    view.pool_spec = &pool;
    for (auto& row : *rows) view.leased_values.push_back(&row);
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
  EXPECT_EQ(Decode(source, nullptr, &status, &calls), error::kInvalidInput);
  EXPECT_EQ(status.FieldPath(), "context");
  EXPECT_EQ(calls, 0);
  AlgContext context;
  source.count = 0;
  EXPECT_EQ(Decode(source, &context, &status, &calls), error::kInvalidInput);
  EXPECT_EQ(status.FieldPath(), "slots");
  EXPECT_EQ(calls, 0);
  EXPECT_FALSE(context.Has("bound.rows"));
}

TEST_F(ConverterRowsTest, InputViewRejectsWrongSlotTypeAndMissingRow) {
  auto source = Input({std::make_shared<RowCarrier>(RowCarrier{"A"})});
  AdapterStatus status;
  EXPECT_FALSE(source.Read<TextInputValue>("other", 0, &status));
  EXPECT_EQ(status.FieldPath(), "other");
  EXPECT_FALSE(source.Read<std::string>("in", 0, &status));
  EXPECT_FALSE(source.Read<TextInputValue>("in", 1, &status));
  source.external_c_type_name = "OtherCarrier";
  EXPECT_FALSE(source.Read<TextInputValue>("in", 0, &status));
  source.external_c_type_name = "RowCarrier";
  source.binding = nullptr;
  EXPECT_FALSE(source.Read<TextInputValue>("in", 0, &status));
  source.binding = &input_binding;
  auto value = source.Read<TextInputValue>("in", 0, &status);
  ASSERT_TRUE(value);
  EXPECT_EQ(value->sentence_text, "A");
}

TEST_F(ConverterRowsTest, OutputViewRequiresMatchingBindingAndPoolBeforeWrite) {
  std::vector<RowCarrier> rows{{"untouched"}};
  auto destination = Output(&rows);
  const TextInputValue value{"changed"};
  EXPECT_FALSE(destination.Write("other", 0, value).IsOk());
  EXPECT_FALSE(destination.Write("out", 0, std::string("wrong type")).IsOk());
  EXPECT_FALSE(destination.Write("out", 1, value).IsOk());
  destination.external_c_type_name = "OtherCarrier";
  EXPECT_FALSE(destination.Write("out", 0, value).IsOk());
  destination.external_c_type_name = "RowCarrier";
  destination.binding = nullptr;
  EXPECT_FALSE(destination.Write("out", 0, value).IsOk());
  destination.binding = &output_binding;
  destination.pool_spec = nullptr;
  const auto missing_pool = destination.Write("out", 0, value);
  EXPECT_EQ(missing_pool.Code(), error::kInvalidOutput);
  EXPECT_EQ(missing_pool.Message(), "Missing output capacity specification");
  EXPECT_EQ(rows[0].text, "untouched");
  destination.pool_spec = &pool;
  EXPECT_TRUE(destination.Write("out", 0, value).IsOk());
  EXPECT_EQ(rows[0].text, "changed");
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
  source.values.clear();
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
    EXPECT_EQ(Decode(source, &context, &status, &calls), error::kInvalidInput);
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
            error::kInvalidInput);
  EXPECT_EQ(status.FieldPath(), "context");
  EXPECT_EQ(written, 0U);
  AlgContext context;
  EXPECT_EQ(Encode(&context, &destination, &written, &status, &calls),
            error::kInvalidInput);
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
              error::kInvalidInput);
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
  destination.leased_values[0] = nullptr;
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

TEST(ConverterKeyTest, KeyIsExactlyNonemptyPrefixDotType) {
  std::string prefix;
  std::string suffix;
  ASSERT_TRUE(OperatorValueTypeRegistry::ParseKey("host_0.entity_in", &prefix,
                                                  &suffix));
  EXPECT_EQ(prefix, "host_0");
  EXPECT_EQ(suffix, "entity_in");
  for (const std::string& key :
       {std::string(), std::string("entity_in"), std::string(".entity_in"),
        std::string("tenant."), std::string("tenant.camera.entity_in"),
        std::string("host_0..entity_in")}) {
    EXPECT_FALSE(OperatorValueTypeRegistry::ParseKey(key, &prefix, &suffix));
  }
}

}  // namespace
}  // namespace llm_edgeflow
