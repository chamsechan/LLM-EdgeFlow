#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_process_binding.h"
#include "adapter/platform_value_binding.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"
#include "tests/support/operator_test_fixture.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {
struct InputRowParams {
  std::string prefix;
};
struct OutputRowParams {
  std::string suffix;
};

TEST(IoConverterTest, HarnessParsesOnceAndRowsReadSharedTypedParameters) {
  int input_prepare_calls = 0;
  int output_prepare_calls = 0;
  InputConverterDefinition input;
  input.type = "keyword_in";
  input.name = "test_parameters";
  input.slot = ExternalInputSlot<TextInputValue>(input.type);
  input.logical_ports = {
      OutputPort(MakeBlackboardKey<TextBatch>("parameter_texts"))};
  input.params =
      Parameters<InputRowParams>{
          Field("prefix", &InputRowParams::prefix).Default("IN:")}
          .Prepare([&](InputRowParams*, std::string*) {
            ++input_prepare_calls;
            return true;
          });
  input.decode_fn = [](const ExternalInputBatchView& source,
                       const InputDecodeOptions& options, AlgContext* context,
                       AdapterStatus* status) {
    return DecodeRequestRows<TextInputValue>(
        source, options, context, status, "keyword_in",
        MakeBlackboardKey<TextBatch>("parameter_texts"),
        [&options](const TextInputValue& row, std::string* payload) {
          *payload =
              options.Params<InputRowParams>().prefix + row.sentence_text;
          return AdapterStatus::Ok();
        });
  };
  OutputConverterDefinition output;
  output.type = "entity_out";
  output.name = "test_parameters";
  output.slot = ExternalOutputSlot<EntityOutputValue>(output.type);
  output.logical_ports = {
      RequiredInputPort(MakeBlackboardKey<TextBatch>("parameter_texts"))};
  output.params =
      Parameters<OutputRowParams>{
          Field("suffix", &OutputRowParams::suffix).Default(":OUT")}
          .Prepare([&](OutputRowParams*, std::string*) {
            ++output_prepare_calls;
            return true;
          });
  output.encode_fn = [](AlgContext* context, const OutputEncodeOptions& options,
                        ExternalOutputBatchView* view, size_t* written,
                        AdapterStatus* status) {
    return EncodeResultRows<EntityOutputValue>(
        context, options, view, written, status, "entity_out",
        MakeBlackboardKey<TextBatch>("parameter_texts"),
        [&options](const std::string& text, EntityOutputValue* row) {
          row->entities_json = text + options.Params<OutputRowParams>().suffix;
          return AdapterStatus::Ok();
        });
  };
  test::AdapterHarness harness(&input, &output);
  EXPECT_EQ(input_prepare_calls, 1);
  EXPECT_EQ(output_prepare_calls, 1);
  char first[] = "one", second[] = "two";
  CompanyString first_text{3, first}, second_text{3, second};
  CompanyOperatorKeywordInput first_input{42, kMockServiceKeywordMatch,
                                          &first_text},
      second_input{84, kMockServiceKeywordMatch, &second_text};
  ASSERT_EQ(harness.DecodeOperator({&first_input, &second_input}), 0);
  EXPECT_EQ(harness.RequestIds(), (std::vector<uint64_t>{42, 84}));
  first[0] = 'x';
  const auto* texts = harness.Context().Read<TextBatch>("parameter_texts");
  ASSERT_NE(texts, nullptr);
  ASSERT_EQ(texts->size(), 2u);
  EXPECT_EQ(texts->front().data, "IN:one");
  EXPECT_EQ(texts->back().data, "IN:two");
  for (int call = 0; call < 2; ++call) {
    char first_bytes[32]{}, second_bytes[32]{};
    CompanyString first_output{0, first_bytes}, second_output{0, second_bytes};
    std::vector<CompanyOperatorEntityOutput> outputs(2);
    outputs[0].service_type = kMockServiceEntityExtract;
    outputs[1].service_type = kMockServiceEntityExtract;
    outputs[0].entities_json = &first_output;
    outputs[1].entities_json = &second_output;
    ASSERT_EQ(harness.EncodeOperator(&outputs, {{"entities_json", 31}}), 0);
    EXPECT_EQ(outputs[0].request_id, 42u);
    EXPECT_EQ(outputs[1].request_id, 84u);
    EXPECT_STREQ(first_bytes, "IN:one:OUT");
    EXPECT_STREQ(second_bytes, "IN:two:OUT");
  }
  EXPECT_EQ(input_prepare_calls, 1);
  EXPECT_EQ(output_prepare_calls, 1);
}

TEST(IoConverterTest, OptionalInputPreservesEveryFramePosition) {
  InputConverterDefinition required;
  required.type = "entity_in";
  required.name = "required";
  required.slot = ExternalInputSlot<TextInputValue>(required.type);
  auto optional = required;
  optional.name = kCommonIoName;
  optional.slot.required = false;
  auto binding = OperatorValueTypeRegistry::Instance()
                     .CopyBindingBySuffix(required.type)
                     .value();
  binding.services[required.name] = kMockServiceEntityExtract;
  const std::vector<SelectedInput> selected = {{&required, {}, {}, binding},
                                               {&optional, {}, {}, binding}};
  char text[] = "hello";
  CompanyString sentence{5, text};
  operator_api::NamedIoBatch inputs(5);
  std::vector<std::shared_ptr<CompanyOperatorEntityInput>> payloads;
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto payload = std::make_shared<CompanyOperatorEntityInput>();
    payload->request_id = 100 + i;
    payload->service_type = kMockServiceEntityExtract;
    payload->sentence_text = &sentence;
    inputs[i]["required.entity_in"] = payload;
    if (i % 2 == 1) inputs[i]["common.entity_in"] = payload;
    payloads.push_back(std::move(payload));
  }
  std::vector<ExternalInputBatchView> views;
  std::vector<uint64_t> request_ids;
  std::string error;
  ASSERT_EQ(ValidateAndExtractOperatorInputs(inputs, selected, {}, &views,
                                             &request_ids, &error),
            0)
      << error;
  ASSERT_EQ(views.size(), 2U);
  ASSERT_EQ(views[1].slots.at("entity_in").size(), inputs.size());
  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto required_value =
        views[0].Read<TextInputValue>("entity_in", i, nullptr);
    ASSERT_TRUE(required_value);
    EXPECT_EQ(required_value->sentence_text, "hello");
    const auto optional_value =
        views[1].Read<TextInputValue>("entity_in", i, nullptr);
    EXPECT_EQ(optional_value.has_value(), i % 2 == 1);
    if (optional_value) {
      EXPECT_EQ(optional_value->sentence_text, "hello");
    }
    EXPECT_EQ(request_ids[i], 100 + i);
  }
  inputs[2].erase("required.entity_in");
  EXPECT_EQ(ValidateAndExtractOperatorInputs(inputs, selected, {}, &views,
                                             &request_ids, &error),
            -3);
  EXPECT_NE(error.find("Missing required input"), std::string::npos);
  EXPECT_NE(error.find("frame 2"), std::string::npos);
}

TEST(IoConverterTest, InputUsesBoundKeyAndSkipsUnreferencedPublication) {
  const auto* converter = IoConverterRegistry::Instance().FindInputConverter(
      "keyword_in", "keyword_match");
  ASSERT_NE(converter, nullptr);
  char bytes[] = "hello";
  CompanyString sentence{5, bytes};
  CompanyOperatorKeywordInput input{42, kMockServiceKeywordMatch, &sentence};
  ExternalInputBatchView source;
  source.count = 1;
  source.slots["keyword_in"] = BorrowInputForTest({&input});
  source.slot_types["keyword_in"] = "CompanyOperatorKeywordInput";
  for (bool referenced : {true, false}) {
    const IoPortBindings ports =
        referenced ? IoPortBindings{{"sentence_text", "input.bound_text"}}
                   : IoPortBindings{};
    test::ParsedInputOptions options(*converter, nlohmann::json::object(),
                                     ports);
    const std::vector<uint64_t> request_ids{42};
    options.request_ids = &request_ids;
    AlgContext context;
    AdapterStatus status;
    ASSERT_EQ(
        test::DecodeForTest(*converter, source, options, &context, &status), 0);
    EXPECT_EQ(request_ids, (std::vector<uint64_t>{42}));
    EXPECT_FALSE(context.Has("sentence_text"));
    EXPECT_EQ(context.Has("input.bound_text"), referenced);
    if (referenced) {
      const auto* text = context.Read<TextBatch>("input.bound_text");
      ASSERT_NE(text, nullptr);
      ASSERT_EQ(text->size(), 1u);
      EXPECT_EQ(text->front().data, "hello");
      EXPECT_EQ(text->front().req_id, 0u);
    }
  }
}

namespace {
constexpr auto kRowTexts = MakeBlackboardKey<TextBatch>("texts");
}  // namespace

TEST(IoConverterTest, DecodeRowsOwnsPayloadsAndSeparatesDuplicateExternalIds) {
  char bytes[] = {'a', 'b', 'c', '!'};
  CompanyString text{3, bytes};
  CompanyOperatorKeywordInput first{42, kMockServiceKeywordMatch, &text},
      second{42, kMockServiceKeywordMatch, &text};
  ExternalInputBatchView source;
  source.count = 2;
  source.slots["input"] = BorrowInputForTest({&first, &second});
  source.slot_types["input"] = "CompanyOperatorKeywordInput";
  source.binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("keyword_in");
  InputDecodeOptions options;
  options.type = "input";
  options.name = "test_rows";
  const IoPortBindings ports{{"texts", "texts"}};
  options.ports = &ports;

  const std::vector<uint64_t> request_ids{42, 42};
  options.request_ids = &request_ids;
  AlgContext context;
  AdapterStatus status;
  ASSERT_EQ(DecodeRequestRows<TextInputValue>(
                source, options, &context, &status, "input", kRowTexts,
                [](const TextInputValue& row, std::string* value) {
                  *value = row.sentence_text;
                  return AdapterStatus::Ok();
                }),
            COMPANY_ALG_SUCCESS);
  bytes[0] = 'x';
  EXPECT_EQ(request_ids, (std::vector<uint64_t>{42, 42}));
  const auto* texts = context.Read(kRowTexts);
  ASSERT_NE(texts, nullptr);
  ASSERT_EQ(texts->size(), 2U);
  for (size_t i = 0; i < texts->size(); ++i) {
    EXPECT_EQ((*texts)[i].req_id, i);
    EXPECT_EQ((*texts)[i].sub_id, 0U);
    EXPECT_EQ((*texts)[i].data, "abc");
  }
}

TEST(IoConverterTest, DecodeRowsReportsCallbackFailureWithoutPublishingBatch) {
  char first_bytes[] = "good", second_bytes[] = "bad";
  CompanyString first_text{4, first_bytes}, second_text{3, second_bytes};
  CompanyOperatorKeywordInput first{1, kMockServiceKeywordMatch, &first_text},
      second{2, kMockServiceKeywordMatch, &second_text};
  ExternalInputBatchView source;
  source.count = 2;
  source.slots["input"] = BorrowInputForTest({&first, &second});
  source.slot_types["input"] = "CompanyOperatorKeywordInput";
  source.binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("keyword_in");
  InputDecodeOptions options;
  options.type = "input";
  options.name = "test_rows";
  const IoPortBindings ports{{"texts", "texts"}};
  options.ports = &ports;

  const std::vector<uint64_t> request_ids{1, 2};
  options.request_ids = &request_ids;
  AlgContext context;
  AdapterStatus status;
  EXPECT_EQ(DecodeRequestRows<TextInputValue>(
                source, options, &context, &status, "input", kRowTexts,
                [](const TextInputValue& row, std::string* value) {
                  if (row.sentence_text == "bad")
                    return AdapterStatus::InvalidInput("bad sentence",
                                                       "sentence_text");
                  *value = "accepted";
                  return AdapterStatus::Ok();
                }),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.AdapterName(), options.Label());
  EXPECT_EQ(status.SampleIndex(), 1);
  EXPECT_EQ(status.FieldPath(), "sentence_text");
  EXPECT_EQ(status.Message(), "bad sentence");
  EXPECT_EQ(request_ids, (std::vector<uint64_t>{1, 2}));
  EXPECT_FALSE(context.Has(kRowTexts.name));
}

TEST(IoConverterTest, EncodeRowsRestoresOrderAndIdsAndChecksWriterCapacity) {
  AlgContext context;
  const std::vector<uint64_t> request_ids{91, 17};
  context.Publish(kRowTexts,
                  TextBatch{{1, 0, "two"}, {0, 0, std::string("a\0b", 3)}});
  OutputEncodeOptions options;
  options.type = "output";
  options.name = "test_rows";
  const IoPortBindings ports{{"texts", "texts"}};
  options.ports = &ports;
  options.request_ids = &request_ids;
  char first_bytes[4] = {}, second_bytes[4] = {};
  CompanyString first_text{0, first_bytes}, second_text{0, second_bytes};
  CompanyOperatorEntityOutput first{0, kMockServiceEntityExtract, nullptr, 0},
      second{0, kMockServiceEntityExtract, nullptr, 0};
  first.entities_json = &first_text;
  second.entities_json = &second_text;
  TestOutputBatchView view;
  view.count = 2;
  view.leased_slots["output"] = {&first, &second};
  view.slot_types["output"] = "CompanyOperatorEntityOutput";
  view.binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("entity_out");
  view.SetCapacity("output", "entities_json", 3);
  auto encode = [](const std::string& text, EntityOutputValue* row) {
    row->status_code = 23;
    row->entities_json = text;
    return AdapterStatus::Ok();
  };
  AdapterStatus status;
  size_t written = 99;
  ASSERT_EQ(
      EncodeResultRows<EntityOutputValue>(&context, options, &view, &written,
                                          &status, "output", kRowTexts, encode),
      COMPANY_ALG_SUCCESS);
  EXPECT_EQ(written, 2U);
  EXPECT_EQ(first.request_id, 91U);
  EXPECT_EQ(second.request_id, 17U);
  EXPECT_EQ(first.status_code, 23);
  EXPECT_EQ(second.status_code, 23);
  ASSERT_EQ(first_text.length, 3);
  EXPECT_EQ(std::string(first_bytes, 3), std::string("a\0b", 3));
  EXPECT_EQ(first_bytes[3], '\0');
  EXPECT_EQ(std::string(second_bytes, second_text.length), "two");

  view.SetCapacity("output", "entities_json", 1);
  written = 99;
  EXPECT_EQ(
      EncodeResultRows<EntityOutputValue>(&context, options, &view, &written,
                                          &status, "output", kRowTexts, encode),
      COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(status.AdapterName(), options.Label());
  EXPECT_EQ(status.SampleIndex(), 0);
  EXPECT_EQ(status.FieldPath(), "entities_json");

  view.SetCapacity("output", "entities_json", 3);
  written = 99;
  EXPECT_EQ(
      EncodeResultRows<EntityOutputValue>(
          &context, options, &view, &written, &status, "output", kRowTexts,
          [](const std::string& text, EntityOutputValue*) {
            return text == "two" ? AdapterStatus::InvalidInput("cannot encode",
                                                               "business_field")
                                 : AdapterStatus::Ok();
          }),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(status.AdapterName(), options.Label());
  EXPECT_EQ(status.SampleIndex(), 1);
  EXPECT_EQ(status.FieldPath(), "business_field");
  EXPECT_EQ(status.Message(), "cannot encode");
}

TEST(IoConverterTest, TypedBindingRequiresExactSlotAndNeutralValueType) {
  const auto* input_binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("keyword_in");
  ASSERT_NE(input_binding, nullptr);
  char bytes[] = "hello";
  CompanyString text{5, bytes};
  CompanyOperatorKeywordInput row{42, kMockServiceKeywordMatch, &text};
  ExternalInputBatchView source;
  source.count = 1;
  source.binding = input_binding;
  source.slots["channel.keyword_in"] = BorrowInputForTest({&row});
  source.slot_types["channel.keyword_in"] = input_binding->external_c_type_name;
  AdapterStatus status;
  auto value = source.Read<TextInputValue>("channel.keyword_in", 0, &status);
  ASSERT_TRUE(value);
  bytes[0] = 'x';
  EXPECT_EQ(value->sentence_text, "hello");
  EXPECT_FALSE(
      source.Read<DocumentInputValue>("channel.keyword_in", 0, &status));
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_FALSE(source.Read<TextInputValue>("keyword_in", 0, &status));
  EXPECT_FALSE(source.Read<TextInputValue>("channel.keyword_in", 1, &status));
  source.slot_types["channel.keyword_in"] = "OtherCarrier";
  EXPECT_FALSE(source.Read<TextInputValue>("channel.keyword_in", 0, &status));
  source.slot_types.clear();
  EXPECT_FALSE(source.Read<TextInputValue>("channel.keyword_in", 0, &status));
  source.binding = nullptr;
  EXPECT_FALSE(source.Read<TextInputValue>("channel.keyword_in", 0, &status));

  const auto* output_binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("entity_out");
  ASSERT_NE(output_binding, nullptr);
  char output_bytes[] = "old";
  CompanyString output_text{3, output_bytes};
  CompanyOperatorEntityOutput output{7, kMockServiceEntityExtract, &output_text,
                                     9};
  TestOutputBatchView destination;
  destination.count = 1;
  destination.binding = output_binding;
  destination.leased_slots["channel.entity_out"] = {&output};
  destination.slot_types["channel.entity_out"] =
      output_binding->external_c_type_name;
  destination.SetCapacity("channel.entity_out", "entities_json", 3);
  EXPECT_FALSE(
      destination.Write("channel.entity_out", 0, 42, DocumentOutputValue{})
          .IsOk());
  EXPECT_FALSE(
      destination.Write("entity_out", 0, 42, EntityOutputValue{"new", 0})
          .IsOk());
  EXPECT_FALSE(
      destination
          .Write("channel.entity_out", 1, 42, EntityOutputValue{"new", 0})
          .IsOk());
  EXPECT_EQ(output.request_id, 7U);
  EXPECT_EQ(output.status_code, 9);
  EXPECT_STREQ(output_bytes, "old");
  ASSERT_TRUE(
      destination
          .Write("channel.entity_out", 0, 42, EntityOutputValue{"new", 23})
          .IsOk());
  EXPECT_EQ(output.request_id, 42U);
  EXPECT_EQ(output.status_code, 23);
  EXPECT_STREQ(output_bytes, "new");
  destination.slot_types["channel.entity_out"] = "OtherCarrier";
  EXPECT_FALSE(
      destination
          .Write("channel.entity_out", 0, 100, EntityOutputValue{"bad", 0})
          .IsOk());
  EXPECT_EQ(output.request_id, 42U);
  EXPECT_STREQ(output_bytes, "new");
}

TEST(IoConverterTest, InputBindingValidatesLengthsAndOwnsExplicitLengthCopies) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("keyword_in");
  ASSERT_NE(binding, nullptr);
  CompanyString empty{0, nullptr};
  CompanyOperatorKeywordInput row{42, kMockServiceKeywordMatch, &empty};
  ExternalInputBatchView source;
  source.count = 1;
  source.binding = binding;
  source.slots["keyword_in"] = BorrowInputForTest({&row});
  source.slot_types["keyword_in"] = binding->external_c_type_name;
  AdapterStatus status;
  const auto value = source.Read<TextInputValue>("keyword_in", 0, &status);
  ASSERT_TRUE(value);
  EXPECT_TRUE(value->sentence_text.empty());
  row.sentence_text = nullptr;
  EXPECT_FALSE(source.Read<TextInputValue>("keyword_in", 0, &status));
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  CompanyString missing{1, nullptr};
  row.sentence_text = &missing;
  EXPECT_FALSE(source.Read<TextInputValue>("keyword_in", 0, &status));
  char bytes[] = {'a', '\0', 'b'};
  CompanyString negative{-1, bytes};
  row.sentence_text = &negative;
  EXPECT_FALSE(source.Read<TextInputValue>("keyword_in", 0, &status));
  CompanyString binary{3, bytes};
  row.sentence_text = &binary;
  EXPECT_FALSE(source.Read<TextInputValue>("keyword_in", 0, &status));
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  bytes[1] = 'b';
  const auto copied = source.Read<TextInputValue>("keyword_in", 0, &status);
  ASSERT_TRUE(copied);
  bytes[0] = 'x';
  EXPECT_EQ(copied->sentence_text, "abb");
}

TEST(IoConverterTest,
     OutputBindingRequiresFieldCapacityAndPreservesFailedDestination) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("entity_out");
  ASSERT_NE(binding, nullptr);
  char bytes[] = "old";
  CompanyString text{3, bytes};
  CompanyOperatorEntityOutput row{7, kMockServiceEntityExtract, &text, 9};
  TestOutputBatchView view;
  view.count = 1;
  view.binding = binding;
  view.leased_slots["entity_out"] = {&row};
  view.slot_types["entity_out"] = binding->external_c_type_name;
  auto status = view.Write("entity_out", 0, 42, EntityOutputValue{"new", 0});
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  view.SetCapacity("entity_out", "other", 3);
  status = view.Write("entity_out", 0, 42, EntityOutputValue{"new", 0});
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.FieldPath(), "entities_json");
  EXPECT_STREQ(bytes, "old");
  EXPECT_EQ(text.length, 3);
  EXPECT_EQ(row.request_id, 7U);
  EXPECT_EQ(row.status_code, 9);
  view.SetCapacity("entity_out", "entities_json", 2);
  status = view.Write("entity_out", 0, 42, EntityOutputValue{"new", 0});
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_STREQ(bytes, "old");
  EXPECT_EQ(text.length, 3);
  EXPECT_EQ(row.request_id, 7U);
  EXPECT_EQ(row.status_code, 9);
}

TEST(IoConverterTest, OutputBindingHonorsEmbeddedNullLengthAndTerminator) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("entity_out");
  ASSERT_NE(binding, nullptr);
  char bytes[] = {'?', '?', '?', '?', '!'};
  CompanyString text{0, bytes};
  CompanyOperatorEntityOutput row{0, kMockServiceEntityExtract, &text, 0};
  TestOutputBatchView view;
  view.count = 1;
  view.binding = binding;
  view.leased_slots["entity_out"] = {&row};
  view.slot_types["entity_out"] = binding->external_c_type_name;
  view.SetCapacity("entity_out", "entities_json", 3);
  const std::string payload("a\0b", 3);
  ASSERT_TRUE(
      view.Write("entity_out", 0, 42, EntityOutputValue{payload, 23}).IsOk());
  EXPECT_EQ(text.length, 3);
  EXPECT_EQ(std::string(bytes, 3), payload);
  EXPECT_EQ(bytes[3], '\0');
  EXPECT_EQ(bytes[4], '!');
  EXPECT_FALSE(
      view.Write("entity_out", 0, 100, EntityOutputValue{"abcd", 0}).IsOk());
  EXPECT_EQ(text.length, 3);
  EXPECT_EQ(std::string(bytes, 3), payload);
  EXPECT_EQ(bytes[4], '!');
  EXPECT_EQ(row.request_id, 42U);
  EXPECT_EQ(row.status_code, 23);
  view.SetCapacity("entity_out", "entities_json", 0);
  ASSERT_TRUE(
      view.Write("entity_out", 0, 100, EntityOutputValue{"", 0}).IsOk());
  EXPECT_EQ(text.length, 0);
  EXPECT_EQ(bytes[0], '\0');
  EXPECT_EQ(bytes[2], 'b');
}

namespace {
// These carriers deliberately have different member order, names and string
// storage.
struct AlternateInputCarrier {
  std::string document;
  int32_t operation = 0;
  struct Routing {
    uint64_t correlation = 0;
  } routing;
};
struct AlternateOutputCarrier {
  int32_t result = -1;
  std::vector<uint8_t> document;
  struct Routing {
    int32_t operation = 0;
    uint64_t correlation = 0;
  } routing;
};

OperatorValueTypeBinding AlternateInputBinding() {
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = "entity_in";
  binding.external_c_type_name = "AlternateInputCarrier";
  binding.direction = IoDirection::kInput;
  binding.validate_external = [](const void* raw, const InputLimits& limits,
                                 std::string* error) {
    const auto& row = *static_cast<const AlternateInputCarrier*>(raw);
    if (row.document.size() > limits.max_text_bytes) {
      if (error) *error = "document exceeds text limit";
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    return COMPANY_ALG_SUCCESS;
  };
  binding.read_request_id = [](const void* raw) {
    return static_cast<const AlternateInputCarrier*>(raw)->routing.correlation;
  };
  SetServiceTypeMember(&binding, &AlternateInputCarrier::operation);
  binding.services = {{"translate", 501}, {"entity_extract", 503}};
  SetInputValue<AlternateInputCarrier, TextInputValue>(
      &binding, [](const AlternateInputCarrier& row) {
        return TextInputValue{row.document};
      });
  return binding;
}

OperatorValueTypeBinding AlternateOutputBinding() {
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = "entity_out";
  binding.external_c_type_name = "AlternateOutputCarrier";
  binding.direction = IoDirection::kOutput;
  binding.write_request_id = [](void* raw, uint64_t id) {
    static_cast<AlternateOutputCarrier*>(raw)->routing.correlation = id;
  };
  binding.read_service_type = [](const void* raw) {
    return static_cast<const AlternateOutputCarrier*>(raw)->routing.operation;
  };
  binding.write_service_type = [](void* raw, int32_t service) {
    static_cast<AlternateOutputCarrier*>(raw)->routing.operation = service;
  };
  binding.services = {{"translate", 502}, {"entity_extract", 504}};
  binding.read_request_id = [](const void* raw) {
    return static_cast<const AlternateOutputCarrier*>(raw)->routing.correlation;
  };
  binding.output_layout.string_capacity_fields = {{"entities_json", {65536}}};
  binding.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec& spec, size_t* bytes, std::string*) {
        *bytes =
            sizeof(AlternateOutputCarrier) + spec.GetCapacity("entities_json");
        return true;
      };
  binding.allocate_external = [](const ResolvedOutputPoolSpec& spec,
                                 OwnedExternalBlock* block, std::string*) {
    auto value = std::make_unique<AlternateOutputCarrier>();
    value->document.reserve(spec.GetCapacity("entities_json"));
    block->raw_struct = block->Own(std::move(value));
    return COMPANY_ALG_SUCCESS;
  };
  binding.reset_external = [](void* raw, const ResolvedOutputPoolSpec&) {
    auto& value = *static_cast<AlternateOutputCarrier*>(raw);
    value.document.clear();
    value.result = -1;
    value.routing = {};
  };
  binding.destroy_external = [](OwnedExternalBlock* block) {
    block->Destroy();
  };
  SetOutputValue<AlternateOutputCarrier, EntityOutputValue>(
      &binding, [](AlternateOutputCarrier& row, const EntityOutputValue& value,
                   const ResolvedOutputPoolSpec& spec) {
        const auto capacity = spec.capacities.find("entities_json");
        if (capacity == spec.capacities.end() ||
            value.entities_json.size() > capacity->second)
          return AdapterStatus::BufferTooSmall("document exceeds capacity",
                                               "entities_json");
        row.document.assign(value.entities_json.begin(),
                            value.entities_json.end());
        row.result = value.status_code;
        return AdapterStatus::Ok();
      });
  return binding;
}
}  // namespace

TEST(IoConverterTest, SameTranslationConvertersServeDifferentPlatformLayouts) {
  const auto* input = IoConverterRegistry::Instance().FindInputConverter(
      "entity_in", "translate");
  const auto* output = IoConverterRegistry::Instance().FindOutputConverter(
      "entity_out", "translate");
  ASSERT_NE(input, nullptr);
  ASSERT_NE(output, nullptr);
  const auto original_input =
      OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("entity_in");
  const auto original_output =
      OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("entity_out");
  ASSERT_TRUE(original_input);
  ASSERT_TRUE(original_output);
  const auto alternate_input = AlternateInputBinding();
  const auto alternate_output = AlternateOutputBinding();
  ASSERT_EQ(input->slot.value_type, alternate_input.value_type);
  ASSERT_EQ(output->slot.value_type, alternate_output.value_type);
  const std::string json_request = R"({"query":"hello","metadata":{"tag":7}})";
  std::string mock_request = json_request;
  CompanyString mock_text{static_cast<int32_t>(mock_request.size()),
                          mock_request.data()};
  CompanyOperatorEntityInput mock_input{
      91, *original_input->ServiceType("translate"), &mock_text};
  AlternateInputCarrier alternate{json_request, 501, {91}};
  char bytes[128]{};
  CompanyString mock_output_text{0, bytes};
  CompanyOperatorEntityOutput mock_output{0, 0, &mock_output_text, -1};
  AlternateOutputCarrier alternate_result;
  for (bool alternate_layout : {false, true}) {
    SCOPED_TRACE(alternate_layout);
    const auto& in_binding =
        alternate_layout ? alternate_input : *original_input;
    const auto& out_binding =
        alternate_layout ? alternate_output : *original_output;
    InputConverterDefinition selected_input = *input;
    const std::vector<SelectedInput> selected{
        {&selected_input, {}, {}, in_binding}};
    operator_api::NamedIoBatch inputs(1);
    inputs[0]["request.entity_in"] = std::shared_ptr<void>(
        alternate_layout ? static_cast<void*>(&alternate)
                         : static_cast<void*>(&mock_input),
        [](void*) {});
    std::vector<ExternalInputBatchView> views;
    std::vector<uint64_t> ids;
    std::string error;
    ASSERT_EQ(ValidateAndExtractOperatorInputs(inputs, selected, {}, &views,
                                               &ids, &error),
              0)
        << error;
    ASSERT_EQ(ids, (std::vector<uint64_t>{91}));
    test::ParsedInputOptions input_options(*input);
    input_options.request_ids = &ids;
    AlgContext context;
    AdapterStatus status;
    ASSERT_EQ(input->decode_fn(views.front(), input_options, &context, &status),
              0)
        << status.Message();
    const auto* queries = context.Read<TextBatch>("query");
    ASSERT_NE(queries, nullptr);
    ASSERT_EQ(queries->size(), 1U);
    EXPECT_EQ(queries->front().data, "hello");
    ASSERT_TRUE(context.Publish("translation",
                                TextBatch{{0, 0, std::string("a\0b", 3)}}));
    test::ParsedOutputOptions output_options(*output);
    output_options.request_ids = &ids;
    TestOutputBatchView destination;
    destination.count = 1;
    destination.binding = &out_binding;
    void* raw_output = alternate_layout ? static_cast<void*>(&alternate_result)
                                        : static_cast<void*>(&mock_output);
    destination.leased_slots["entity_out"] = {raw_output};
    destination.slot_types["entity_out"] = out_binding.external_c_type_name;
    destination.SetCapacity("entity_out", "entities_json", 127);
    out_binding.write_service_type(raw_output,
                                   *out_binding.ServiceType("translate"));
    size_t written = 99;
    ASSERT_EQ(output->encode_fn(&context, output_options, &destination,
                                &written, &status),
              0)
        << status.Message();
    EXPECT_EQ(written, 1U);
    const std::string response =
        alternate_layout
            ? std::string(alternate_result.document.begin(),
                          alternate_result.document.end())
            : std::string(mock_output_text.data, mock_output_text.length);
    EXPECT_EQ(nlohmann::json::parse(response),
              nlohmann::json({{"translated", std::string("a\0b", 3)}}));
    if (alternate_layout) {
      EXPECT_EQ(alternate_result.routing.correlation, 91U);
      EXPECT_EQ(alternate_result.routing.operation, 502);
      EXPECT_EQ(alternate_result.result, 0);
    } else {
      EXPECT_EQ(mock_output.request_id, 91U);
      EXPECT_EQ(mock_output.service_type,
                *original_output->ServiceType("translate"));
      EXPECT_EQ(mock_output.status_code, 0);
    }
    destination.SetCapacity("entity_out", "entities_json", 1);
    EXPECT_EQ(output->encode_fn(&context, output_options, &destination,
                                &written, &status),
              COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(written, 0U);
    EXPECT_EQ(status.SampleIndex(), 0);
    EXPECT_EQ(status.AdapterName(), "entity_out/translate");
    EXPECT_EQ(status.FieldPath(), "entities_json");
    if (alternate_layout) {
      EXPECT_EQ(std::string(alternate_result.document.begin(),
                            alternate_result.document.end()),
                response);
    } else {
      EXPECT_EQ(std::string(mock_output_text.data, mock_output_text.length),
                response);
    }
    if (alternate_layout) {
      alternate.document = R"({"query":7})";
    } else {
      mock_request = R"({"query":7})";
      mock_text = {static_cast<int32_t>(mock_request.size()),
                   mock_request.data()};
    }
    ASSERT_EQ(ValidateAndExtractOperatorInputs(inputs, selected, {}, &views,
                                               &ids, &error),
              0)
        << error;
    AlgContext invalid_context;
    EXPECT_EQ(input->decode_fn(views.front(), input_options, &invalid_context,
                               &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_EQ(status.FieldPath(), "json");
    EXPECT_EQ(status.SampleIndex(), 0);
    EXPECT_EQ(status.AdapterName(), "entity_in/translate");
    EXPECT_FALSE(invalid_context.Has("query"));
    // Input extraction also enforces the service mapping from the selected
    // binding.
    if (alternate_layout)
      alternate.operation = -1;
    else
      mock_input.service_type = -1;
    EXPECT_EQ(ValidateAndExtractOperatorInputs(inputs, selected, {}, &views,
                                               &ids, &error),
              COMPANY_ALG_ERR_INVALID_INPUT);
    EXPECT_NE(error.find("service_type"), std::string::npos);
  }
}

class IoConverterProcessTest : public test_support::OperatorTestFixture {
 protected:
  void SetUp() override {
    test_support::RegistryTestAccess::SetValueBinding(AlternateInputBinding());
    test_support::RegistryTestAccess::SetValueBinding(AlternateOutputBinding());
    directory_ = std::filesystem::temp_directory_path() /
                 ("edgeflow_alternate_binding_" + std::to_string(getpid()));
    std::filesystem::create_directories(directory_);
    OperatorTestFixture::SetUp();
  }
  void TearDown() override {
    OperatorTestFixture::TearDown();
    std::filesystem::remove_all(directory_);
  }
  std::filesystem::path directory_;
  test_support::RegistryTestAccess::ScopedValueTypeState value_types_;
};

TEST_F(IoConverterProcessTest,
       TranslationBindingsOwnPlatformLayoutThroughProcess) {
  const nlohmann::json document = {
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "translate"}}}},
        {"output",
         {{{"type", "entity_out"},
           {"name", "translate"},
           {"params", {{"entities_json_max_bytes", 64}}},
           {"inputs", {{"translation", "copy.text"}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"name", "copy"},
         {"type", "text_template"},
         {"inputs", {{"primary", "input.query"}}},
         {"params", {{"template", "copy:{{primary}}"}}}}}}};
  std::ofstream(directory_ / "pipeline.json") << document;
  std::ofstream(directory_ / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  operator_api::OperatorIoContract contract;
  ASSERT_EQ(operator_api::ResolveOperatorConfigIo(directory_.c_str(),
                                                  "pipeline.conf", &contract),
            0)
      << operator_api::GetOperatorLastError();
  ASSERT_EQ(contract.inputs.size(), 1U);
  ASSERT_EQ(contract.outputs.size(), 1U);
  EXPECT_EQ(contract.inputs.front().type_name, "AlternateInputCarrier");
  EXPECT_EQ(contract.inputs.front().service_type, 501);
  EXPECT_EQ(contract.outputs.front().type_name, "AlternateOutputCarrier");
  EXPECT_EQ(contract.outputs.front().service_type, 502);
  test_support::ScopedTestOperator instance(ops_);
  ASSERT_EQ(instance.Create("pipeline.conf", directory_.string(),
                            operator_api::ComputePlatform::kCpu, 2),
            0)
      << instance.create_diagnostic();
  AlternateInputCarrier first{R"({"query":"one"})", 501, {911}};
  AlternateInputCarrier second{
      nlohmann::json{{"query", std::string(80, 'x')}}.dump(), 501, {722}};
  operator_api::NamedIoBatch inputs(2), outputs(2);
  inputs[0]["request.entity_in"] =
      operator_api::MakeBorrowedOperatorInput(&first);
  inputs[1]["request.entity_in"] =
      operator_api::MakeBorrowedOperatorInput(&second);
  for (auto& row : outputs) row["response.entity_out"] = nullptr;
  // Row zero encodes successfully before row one's larger response fails.
  EXPECT_EQ(ops_.Process(instance.get(), inputs, outputs),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL)
      << operator_api::GetOperatorLastError();
  for (const auto& row : outputs) EXPECT_FALSE(row.at("response.entity_out"));
  second.document = R"({"query":"two"})";
  ASSERT_EQ(ops_.Process(instance.get(), inputs, outputs), COMPANY_ALG_SUCCESS)
      << operator_api::GetOperatorLastError();
  std::vector<nlohmann::json> copied;
  for (size_t i = 0; i < outputs.size(); ++i) {
    const auto* result = static_cast<const AlternateOutputCarrier*>(
        outputs[i].at("response.entity_out").get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->routing.correlation, i == 0 ? 911U : 722U);
    EXPECT_EQ(result->routing.operation, 502);
    EXPECT_EQ(result->result, 0);
    copied.push_back(nlohmann::json::parse(
        std::string(result->document.begin(), result->document.end())));
    EXPECT_EQ(
        copied.back(),
        nlohmann::json({{"translated", i == 0 ? "copy:one" : "copy:two"}}));
  }
  for (auto& row : outputs) row["response.entity_out"].reset();
  // Returning both leases permits another full batch on the same depth-two
  // pool.
  ASSERT_EQ(ops_.Process(instance.get(), inputs, outputs), COMPANY_ALG_SUCCESS)
      << operator_api::GetOperatorLastError();
  outputs.clear();
  EXPECT_EQ(instance.Close(), 0) << instance.close_diagnostic();
  EXPECT_EQ(copied,
            (std::vector<nlohmann::json>{{{"translated", "copy:one"}},
                                         {{"translated", "copy:two"}}}));
}

}  // namespace llm_edgeflow
