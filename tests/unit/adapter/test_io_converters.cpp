#include <gtest/gtest.h>

#include <string_view>
#include <type_traits>

#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_process_binding.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {
struct ReproA {
  int a = 7;
};
struct ReproB {
  uint64_t b = 42;
};
DECLARE_EXTERNAL_TYPE_TRAITS(ReproA, "ReproA");
DECLARE_EXTERNAL_TYPE_TRAITS(ReproB, "ReproB");

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
  input.slot = ExternalInputSlot<CompanyOperatorKeywordInput>(input.type);
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
    return DecodeRequestRows<CompanyOperatorKeywordInput>(
        source, options, context, status, "keyword_in",
        MakeBlackboardKey<TextBatch>("parameter_texts"),
        [&options](const CompanyOperatorKeywordInput& row,
                   std::string* payload) {
          *payload = options.Params<InputRowParams>().prefix +
                     CopyInputString(*row.sentence_text);
          return AdapterStatus::Ok();
        });
  };
  OutputConverterDefinition output;
  output.type = "entity_out";
  output.name = "test_parameters";
  output.slot = ExternalOutputSlot<CompanyOperatorEntityOutput>(output.type);
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
    return EncodeResultRows<CompanyOperatorEntityOutput>(
        context, options, view, written, status, "entity_out",
        MakeBlackboardKey<TextBatch>("parameter_texts"),
        [&options](const std::string& text, CompanyOperatorEntityOutput* row,
                   const OutputStringWriter& writer) {
          return writer.Write(row->entities_json, "entities_json",
                              text + options.Params<OutputRowParams>().suffix);
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

TEST(IoConverterTest, ViewAccessors) {
  // 1. ExternalInputBatchView Slot 访问
  ExternalInputBatchView in_view;
  int sample_int = 42;
  in_view.count = 1;
  in_view.slots["slot_a"] = llm_edgeflow::BorrowInputForTest({&sample_int});
  in_view.slot_types["slot_a"] = "int";

  EXPECT_EQ(in_view.GetSlot<int>("slot_a", 0), &sample_int);
  EXPECT_EQ(in_view.GetSlot<float>("slot_a", 0), nullptr);
  EXPECT_EQ(in_view.GetSlot<int>("slot_a", 1), nullptr);
  EXPECT_EQ(in_view.GetSlot<int>("unknown", 0), nullptr);

  auto shared_sample = std::make_shared<int>(100);
  in_view.slots["slot_shared"] = {shared_sample};
  in_view.slot_types["slot_shared"] = "int";
  EXPECT_EQ(in_view.GetSlot<int>("slot_shared", 0), shared_sample.get());
  EXPECT_EQ(in_view.GetSlot<float>("slot_shared", 0), nullptr);
  EXPECT_EQ(in_view.GetSlot<int>("slot_shared", 1), nullptr);

  // 2. ExternalOutputBatchView 访问
  TestOutputBatchView out_view;
  int out_sample = 0;
  out_view.count = 1;
  out_view.leased_slots["out_slot"] = {&out_sample};
  out_view.slot_types["out_slot"] = "int";
  out_view.SetCapacity("out_slot", "field_1", 1024);
  EXPECT_EQ(out_view.GetSlot<int>("out_slot", 0), &out_sample);
  EXPECT_EQ(out_view.GetSlot<float>("out_slot", 0), nullptr);
  EXPECT_EQ(out_view.GetPoolSpec("out_slot")->GetCapacity("field_1"), 1024U);
  EXPECT_EQ(out_view.GetPoolSpec("out_slot")->GetCapacity("unknown"), 0U);
}

TEST(IoConverterTest, ExactSlotLookupAndTypeSafety) {
  // 1. shared_ptr (slots) 路径
  ExternalInputBatchView v;
  v.count = 1;

  auto b_ptr = std::make_shared<ReproB>();
  v.slots["channel.slot"] = {b_ptr};
  v.slot_types["channel.slot"] = "ReproB";

  // 类型正确的精确查找成功
  EXPECT_EQ(v.GetSlot<ReproB>("channel.slot", 0), b_ptr.get());
  // 类型错误的精确查找返回 nullptr
  EXPECT_EQ(v.GetSlot<ReproA>("channel.slot", 0), nullptr);
  // 短后缀查找不得回退：返回 nullptr
  EXPECT_EQ(v.GetSlot<ReproB>("slot", 0), nullptr);
  EXPECT_EQ(v.GetSlot<ReproA>("slot", 0), nullptr);

  v.slot_types.clear();
  EXPECT_EQ(v.GetSlot<ReproB>("channel.slot", 0), nullptr);

  TestOutputBatchView output;
  output.leased_slots["channel.slot"] = {b_ptr.get()};
  EXPECT_EQ(output.GetSlot<ReproB>("channel.slot", 0), nullptr);
  output.slot_types["channel.slot"] = "ReproB";
  EXPECT_EQ(output.GetSlot<ReproB>("channel.slot", 0), b_ptr.get());
  EXPECT_EQ(output.GetSlot<ReproA>("channel.slot", 0), nullptr);
  EXPECT_EQ(output.GetSlot<ReproB>("slot", 0), nullptr);
  output.SetCapacity("channel.slot", "bytes", 512);
  ASSERT_NE(output.GetPoolSpec("channel.slot"), nullptr);
  EXPECT_EQ(output.GetPoolSpec("slot"), nullptr);
  EXPECT_EQ(output.GetPoolSpec("channel.slot")->GetCapacity("bytes"), 512U);

  // 3. 多槽位歧义消解：同一类型的不同逻辑槽位
  ExternalInputBatchView multi;
  multi.count = 1;
  auto left = std::make_shared<ReproA>();
  left->a = 10;
  auto right = std::make_shared<ReproA>();
  right->a = 20;
  multi.slots["left"] = {left};
  multi.slots["right"] = {right};
  multi.slot_types["left"] = "ReproA";
  multi.slot_types["right"] = "ReproA";

  EXPECT_EQ(multi.GetSlot<ReproA>("left", 0), left.get());
  EXPECT_EQ(multi.GetSlot<ReproA>("right", 0), right.get());
  EXPECT_EQ(multi.GetSlot<ReproA>("left", 0)->a, 10);
  EXPECT_EQ(multi.GetSlot<ReproA>("right", 0)->a, 20);
  EXPECT_EQ(multi.GetSlot<ReproA>("unknown", 0), nullptr);
}

TEST(IoConverterTest, OptionalInputPreservesEveryFramePosition) {
  InputConverterDefinition required;
  required.type = "entity_in";
  required.name = "required";
  required.service_type = kMockServiceEntityExtract;
  required.slot = ExternalInputSlot<CompanyOperatorEntityInput>(required.type);
  auto optional = required;
  optional.name = kCommonIoName;
  optional.service_type.reset();
  optional.slot.required = false;
  const std::vector<SelectedInput> selected = {{&required, {}},
                                               {&optional, {}}};
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
    EXPECT_EQ(views[0].GetSlot<CompanyOperatorEntityInput>("entity_in", i),
              payloads[i].get());
    EXPECT_EQ(views[1].GetSlot<CompanyOperatorEntityInput>("entity_in", i),
              i % 2 == 1 ? payloads[i].get() : nullptr);
    EXPECT_EQ(request_ids[i], 100 + i);
  }
  inputs[2].erase("required.entity_in");
  EXPECT_EQ(ValidateAndExtractOperatorInputs(inputs, selected, {}, &views,
                                             &request_ids, &error),
            -3);
  EXPECT_NE(error.find("Missing required input"), std::string::npos);
  EXPECT_NE(error.find("frame 2"), std::string::npos);
}

TEST(IoConverterTest, HarnessPreservesNamedSlotsTypesAndPoolCapacities) {
  InputConverterDefinition input;
  input.decode_fn = [](const ExternalInputBatchView& view,
                       const InputDecodeOptions&, AlgContext* context,
                       AdapterStatus*) -> int {
    const auto* a = view.GetSlot<ReproA>("left", 0);
    const auto* b = view.GetSlot<ReproB>("right", 0);
    if (!a || !b || view.GetSlot<ReproB>("left", 0)) return -3;
    return context->Publish("sum", a->a + static_cast<int>(b->b)) ? 0 : -3;
  };
  OutputConverterDefinition output;
  output.encode_fn = [](AlgContext* context, const OutputEncodeOptions&,
                        ExternalOutputBatchView* view, size_t* written,
                        AdapterStatus*) -> int {
    auto* a = view->GetSlot<ReproA>("left", 0);
    auto* b = view->GetSlot<ReproB>("right", 0);
    const auto* sum = context->Read<int>("sum");
    if (!a || !b || !sum || view->GetSlot<ReproA>("right", 0)) return -3;
    const auto* pool = view->GetPoolSpec("right");
    if (!pool || pool->GetCapacity("items") < 2) return -4;
    a->a = *sum;
    b->b = *sum;
    *written = 1;
    return 0;
  };
  test::AdapterHarness harness(&input, &output);
  ReproA a;
  ReproB b;
  ExternalInputBatchView source;
  source.count = 1;
  source.slots["left"] = BorrowInputForTest({&a});
  source.slots["right"] = BorrowInputForTest({&b});
  source.slot_types = {{"left", "ReproA"}, {"right", "ReproB"}};
  ASSERT_EQ(harness.DecodeOperator(source), 0);
  TestOutputBatchView destination;
  destination.count = 1;
  destination.leased_slots = {{"left", {&a}}, {"right", {&b}}};
  destination.slot_types = source.slot_types;
  destination.SetCapacity("right", "items", 1);
  size_t written = 0;
  EXPECT_EQ(harness.EncodeOperator(&destination, &written), -4);
  EXPECT_EQ(written, 0U);
  destination.SetCapacity("right", "items", 2);
  EXPECT_EQ(harness.EncodeOperator(&destination, &written), 0);
  EXPECT_EQ(written, 1U);
  EXPECT_EQ(a.a, 49);
  EXPECT_EQ(b.b, 49U);
}

TEST(IoConverterTest, HarnessSingleSlotUsesDeclaredSlotType) {
  InputConverterDefinition input;
  input.type = "value";
  input.slot = ExternalInputSlot<ReproA>(input.type);
  input.decode_fn = [](const ExternalInputBatchView& view,
                       const InputDecodeOptions&, AlgContext*,
                       AdapterStatus*) -> int {
    return view.GetSlot<ReproA>("value", 0) ? 0 : -3;
  };
  OutputConverterDefinition output;
  output.type = "value";
  output.slot = ExternalOutputSlot<ReproB>(output.type);
  output.encode_fn = [](AlgContext*, const OutputEncodeOptions&,
                        ExternalOutputBatchView* view, size_t* written,
                        AdapterStatus*) -> int {
    if (!view->GetSlot<ReproB>("value", 0)) return -3;
    *written = 1;
    return 0;
  };
  test::AdapterHarness harness(&input, &output);
  ReproA value;
  EXPECT_EQ(harness.DecodeOperator(std::vector<const void*>{&value}), 0);
  std::vector<ReproB> outputs(2);
  EXPECT_EQ(harness.EncodeOperator(&outputs), 0);
  EXPECT_EQ(outputs.size(), 1U);
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
    std::vector<uint64_t> request_ids;
    options.request_ids = &request_ids;
    AlgContext context;
    AdapterStatus status;
    ASSERT_EQ(converter->decode_fn(source, options, &context, &status), 0);
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

TEST(IoConverterTest, OutputWriterRequiresExplicitFieldCapacityWithoutWriting) {
  TestOutputBatchView view;
  OutputEncodeOptions options;
  options.type = "output";
  options.name = "test_writer";
  char bytes[] = "old";
  CompanyString destination{3, bytes};
  AdapterStatus status;
  for (bool has_other_field : {false, true}) {
    SCOPED_TRACE(has_other_field);
    if (has_other_field) view.SetCapacity("output", "other", 3);
    EXPECT_FALSE(WriteOutputString(view, "output", &destination, "text", "new",
                                   options, &status, 2));
    EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(status.Message(), "Missing output capacity specification");
    EXPECT_EQ(status.FieldPath(), "text");
    EXPECT_EQ(status.SampleIndex(), 2);
    EXPECT_EQ(status.AdapterName(), "output/test_writer");
    EXPECT_STREQ(bytes, "old");
    EXPECT_EQ(destination.length, 3);
    EXPECT_FALSE(WriteOutputString(view, "output", &destination, "text", "new",
                                   options, nullptr, 2));
    EXPECT_STREQ(bytes, "old");
  }
}

TEST(IoConverterTest, OutputWriterHonorsPayloadCapacityAndTerminator) {
  TestOutputBatchView view;
  view.SetCapacity("output", "text", 3);
  OutputEncodeOptions options;
  options.type = "output";
  options.name = "test_writer";
  char bytes[] = {'?', '?', '?', '?', '!'};
  CompanyString destination{0, bytes};
  EXPECT_TRUE(WriteOutputString(view, "output", &destination, "text", "abc",
                                options, nullptr, 0));
  EXPECT_STREQ(bytes, "abc");
  EXPECT_EQ(destination.length, 3);
  EXPECT_EQ(bytes[3], '\0');
  EXPECT_EQ(bytes[4], '!');
  EXPECT_FALSE(WriteOutputString(view, "output", &destination, "text", "abcd",
                                 options, nullptr, 0));
  EXPECT_STREQ(bytes, "abc");
  EXPECT_EQ(destination.length, 3);
  EXPECT_EQ(bytes[4], '!');

  view.SetCapacity("output", "text", 0);
  EXPECT_TRUE(WriteOutputString(view, "output", &destination, "text", "",
                                options, nullptr, 0));
  EXPECT_EQ(destination.length, 0);
  EXPECT_EQ(bytes[0], '\0');
  EXPECT_EQ(bytes[1], 'b');
}

TEST(IoConverterTest, OutputWriterPreservesEmbeddedNullAndChecksFullLength) {
  TestOutputBatchView view;
  view.SetCapacity("output", "text", 3);
  OutputEncodeOptions options;
  options.type = "output";
  options.name = "test_writer";
  const std::string payload("a\0b", 3);
  char bytes[] = {'?', '?', '?', '?', '!'};
  CompanyString destination{0, bytes};
  ASSERT_TRUE(WriteOutputString(view, "output", &destination, "text",
                                std::string_view(payload), options, nullptr,
                                0));
  ASSERT_EQ(destination.length, 3);
  EXPECT_EQ(std::string(bytes, destination.length), payload);
  EXPECT_EQ(bytes[3], '\0');
  EXPECT_EQ(bytes[4], '!');

  view.SetCapacity("output", "text", 1);
  EXPECT_FALSE(WriteOutputString(view, "output", &destination, "text",
                                 std::string_view(payload), options, nullptr,
                                 0));
  EXPECT_EQ(destination.length, 3);
  EXPECT_EQ(std::string(bytes, 3), payload);
  EXPECT_EQ(bytes[4], '!');
}

TEST(IoConverterTest, EmptyInputStringAllowsNullDataAndCopiesEmbeddedNulls) {
  CompanyString empty{0, nullptr};
  EXPECT_TRUE(IsValidInputString(&empty));
  EXPECT_TRUE(CopyInputString(empty).empty());
  EXPECT_FALSE(IsValidInputString(nullptr));
  CompanyString missing{1, nullptr};
  EXPECT_FALSE(IsValidInputString(&missing));
  char bytes[] = {'a', '\0', 'b'};
  CompanyString negative{-1, bytes};
  EXPECT_FALSE(IsValidInputString(&negative));
  CompanyString binary{3, bytes};
  EXPECT_TRUE(IsValidInputString(&binary));
  EXPECT_EQ(CopyInputString(binary), std::string(bytes, sizeof(bytes)));
}

namespace {
constexpr auto kRowTexts = MakeBlackboardKey<TextBatch>("texts");
}  // namespace

TEST(IoConverterTest, DecodeRowsOwnsPayloadsAndSeparatesDuplicateExternalIds) {
  char bytes[] = {'a', '\0', 'b'};
  CompanyString text{3, bytes};
  CompanyOperatorKeywordInput first{42, kMockServiceKeywordMatch, &text},
      second{42, kMockServiceKeywordMatch, &text};
  ExternalInputBatchView source;
  source.count = 2;
  source.slots["input"] = BorrowInputForTest({&first, &second});
  source.slot_types["input"] = "CompanyOperatorKeywordInput";
  InputDecodeOptions options;
  options.type = "input";
  options.name = "test_rows";
  const IoPortBindings ports{{"texts", "texts"}};
  options.ports = &ports;

  std::vector<uint64_t> request_ids;
  options.request_ids = &request_ids;
  AlgContext context;
  AdapterStatus status;
  ASSERT_EQ(DecodeRequestRows<CompanyOperatorKeywordInput>(
                source, options, &context, &status, "input", kRowTexts,
                [](const CompanyOperatorKeywordInput& row, std::string* value) {
                  *value = CopyInputString(*row.sentence_text);
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
    EXPECT_EQ((*texts)[i].data, std::string("a\0b", 3));
  }
}

TEST(IoConverterTest, DecodeRowsReportsCallbackFailureWithoutPublishingBatch) {
  CompanyOperatorKeywordInput first{1, kMockServiceKeywordMatch, nullptr},
      second{2, kMockServiceKeywordMatch, nullptr};
  ExternalInputBatchView source;
  source.count = 2;
  source.slots["input"] = BorrowInputForTest({&first, &second});
  source.slot_types["input"] = "CompanyOperatorKeywordInput";
  InputDecodeOptions options;
  options.type = "input";
  options.name = "test_rows";
  const IoPortBindings ports{{"texts", "texts"}};
  options.ports = &ports;

  std::vector<uint64_t> request_ids;
  options.request_ids = &request_ids;
  AlgContext context;
  AdapterStatus status;
  EXPECT_EQ(DecodeRequestRows<CompanyOperatorKeywordInput>(
                source, options, &context, &status, "input", kRowTexts,
                [](const CompanyOperatorKeywordInput& row, std::string* value) {
                  if (row.request_id == 2)
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
  EXPECT_TRUE(request_ids.empty());
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
  view.SetCapacity("output", "entities_json", 3);
  auto encode = [](const std::string& text, CompanyOperatorEntityOutput* row,
                   const OutputStringWriter& writer) {
    row->status_code = 23;
    return writer.Write(row->entities_json, "entities_json", text);
  };
  AdapterStatus status;
  size_t written = 99;
  ASSERT_EQ(EncodeResultRows<CompanyOperatorEntityOutput>(
                &context, options, &view, &written, &status, "output",
                kRowTexts, encode),
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
  EXPECT_EQ(EncodeResultRows<CompanyOperatorEntityOutput>(
                &context, options, &view, &written, &status, "output",
                kRowTexts, encode),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(status.AdapterName(), options.Label());
  EXPECT_EQ(status.SampleIndex(), 0);
  EXPECT_EQ(status.FieldPath(), "entities_json");

  written = 99;
  EXPECT_EQ(
      EncodeResultRows<CompanyOperatorEntityOutput>(
          &context, options, &view, &written, &status, "output", kRowTexts,
          [](const std::string&, CompanyOperatorEntityOutput* row,
             const OutputStringWriter&) {
            return row->request_id == 17
                       ? AdapterStatus::InvalidInput("cannot encode",
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

}  // namespace llm_edgeflow
