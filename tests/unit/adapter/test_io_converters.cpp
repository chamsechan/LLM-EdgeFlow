#include <gtest/gtest.h>

#include <string_view>

#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_process_binding.h"
#include "contracts/inference_payloads.h"
#include "core/common_contracts.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {
namespace {

int DummyDecode(const ExternalInputBatchView&, const InputDecodeOptions&,
                const InputPortBindings&, AlgContext*, AdapterStatus*) {
  return 0;
}

int DummyEncode(AlgContext*, const OutputPortBindings&,
                const OutputEncodeOptions&, ExternalOutputBatchView*,
                size_t* written_count, AdapterStatus*) {
  if (written_count) *written_count = 1;
  return 0;
}

}  // namespace

struct ReproA {
  int a = 7;
};
struct ReproB {
  uint64_t b = 42;
};
DECLARE_EXTERNAL_TYPE_TRAITS(ReproA, "ReproA");
DECLARE_EXTERNAL_TYPE_TRAITS(ReproB, "ReproB");

TEST(IoConverterTest, ViewAccessorsAndPortBindings) {
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
  EXPECT_EQ(out_view.GetSlotCapacity("out_slot", "field_1"), 1024U);
  EXPECT_EQ(out_view.GetSlotCapacity("out_slot", "unknown", 42), 42U);

  // 3. PortBindings
  InputPortBindings in_bindings({{"texts", "input_sentences"}});
  auto key = in_bindings.Key<TextBatch>("texts");
  EXPECT_STREQ(key.name, "input_sentences");
  EXPECT_EQ(in_bindings.GetActualKey("texts"), "input_sentences");
  EXPECT_TRUE(in_bindings.HasKey("texts"));
  EXPECT_EQ(in_bindings.GetActualKey("unknown"), "");
  EXPECT_FALSE(in_bindings.HasKey("unknown"));

  OutputPortBindings out_bindings({{"answers", "llm_answers"}});
  auto out_key = out_bindings.Key<TextBatch>("answers");
  EXPECT_STREQ(out_key.name, "llm_answers");
  EXPECT_TRUE(out_bindings.HasKey("answers"));
  EXPECT_EQ(out_bindings.GetActualKey("unknown"), "");
  EXPECT_FALSE(out_bindings.HasKey("unknown"));
}

TEST(IoConverterTest, RegisterAndFindInputConverter) {
  auto& reg = IoConverterRegistry::Instance();

  InputConverterDefinition def;
  def.converter_id = "test.input.operator.v1";

  def.schema_id = "test_input";
  def.schema_version = 1;
  def.external_type = "int";
  def.external_slots = {ExternalSlotDefinition(
      "inputs", "int", PortDirection::kInput, true, "int", "inputs")};
  def.max_batch_size = 64;
  def.logical_ports = {NodePortDefinition("texts", "TextBatch", true, "1:1")};
  def.decode_fn = &DummyDecode;

  EXPECT_TRUE(reg.RegisterInputConverter(def));

  const auto* found = reg.FindInputConverter("test.input.operator.v1");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->converter_id, "test.input.operator.v1");

  EXPECT_EQ(found->logical_ports.size(), 1U);

  // 重复注册拒绝并记录冲突
  EXPECT_FALSE(reg.RegisterInputConverter(def));
  EXPECT_TRUE(reg.HasConflict());
}

TEST(IoConverterTest, RegisterAndFindOutputConverter) {
  auto& reg = IoConverterRegistry::Instance();

  OutputConverterDefinition def;
  def.converter_id = "test.output.operator.v1";

  def.schema_id = "test_output";
  def.schema_version = 1;
  def.external_type = "int";
  def.external_slots = {ExternalSlotDefinition(
      "answers", "int", PortDirection::kOutput, true, "int", "answers")};
  def.max_batch_size = 64;
  def.logical_ports = {NodePortDefinition("answers", "TextBatch", true, "1:1")};
  def.encode_fn = &DummyEncode;

  EXPECT_TRUE(reg.RegisterOutputConverter(def));

  const auto* found = reg.FindOutputConverter("test.output.operator.v1");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->converter_id, "test.output.operator.v1");
}

TEST(IoConverterTest, RejectsInvalidDefinitions) {
  InputConverterDefinition bad_in;
  bad_in.converter_id = "";
  bad_in.decode_fn = &DummyDecode;
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  bad_in.converter_id = "bad.in";
  bad_in.schema_id = "test";
  bad_in.schema_version = 1;
  bad_in.external_type = "int";
  bad_in.external_slots = {ExternalSlotDefinition(
      "inputs", "int", PortDirection::kInput, true, "int", "inputs")};
  bad_in.max_batch_size = 64;
  bad_in.logical_ports = {
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  bad_in.decode_fn = nullptr;
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  // 缺少 schema_id
  bad_in.decode_fn = &DummyDecode;
  bad_in.schema_id = "";
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  // schema_version < 1
  bad_in.schema_id = "test";
  bad_in.schema_version = 0;
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  // 缺少 external_type
  bad_in.schema_version = 1;
  bad_in.external_type = "";
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  // 缺少 external_slots
  bad_in.external_type = "int";
  bad_in.external_slots.clear();
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  // 缺少 logical_ports
  bad_in.external_slots = {ExternalSlotDefinition(
      "inputs", "int", PortDirection::kInput, true, "int", "inputs")};
  bad_in.logical_ports.clear();
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  // max_batch_size == 0
  bad_in.logical_ports = {
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  bad_in.max_batch_size = 0;
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  OutputConverterDefinition bad_out;
  bad_out.converter_id = "bad.out";

  bad_out.schema_id = "test";
  bad_out.schema_version = 1;
  bad_out.external_type = "int";
  bad_out.external_slots = {ExternalSlotDefinition(
      "answers", "int", PortDirection::kOutput, true, "int", "answers")};
  bad_out.max_batch_size = 64;
  bad_out.logical_ports = {
      NodePortDefinition("answers", "TextBatch", true, "1:1")};
  bad_out.encode_fn = nullptr;
  EXPECT_FALSE(
      IoConverterRegistry::Instance().RegisterOutputConverter(bad_out));
}

TEST(IoConverterTest, ExactSlotLookupAndTypeSafety) {
  // 1. shared_ptr (slots) path
  ExternalInputBatchView v;
  v.count = 1;

  auto b_ptr = std::make_shared<ReproB>();
  v.slots["channel.slot"] = {b_ptr};
  v.slot_types["channel.slot"] = "ReproB";

  // Exact lookup with correct type succeeds
  EXPECT_EQ(v.GetSlot<ReproB>("channel.slot", 0), b_ptr.get());
  // Exact lookup with wrong type returns nullptr
  EXPECT_EQ(v.GetSlot<ReproA>("channel.slot", 0), nullptr);
  // Short suffix lookup must NOT fallback: returns nullptr
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
  EXPECT_EQ(output.GetSlotCapacity("channel.slot", "bytes"), 512U);
  EXPECT_EQ(output.GetSlotCapacity("slot", "bytes", 7), 7U);

  // 3. Multi-slot ambiguity resolution: distinct logical slots of same type
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

TEST(IoConverterTest,
     RejectsEmptyTypeSuffixAndSupportsMultipleSlotsOfSameType) {
  auto& reg = IoConverterRegistry::Instance();

  // 1. Input converter with empty type_suffix must be rejected
  InputConverterDefinition bad_in;
  bad_in.converter_id = "test.empty_suffix.in";

  bad_in.schema_id = "test_schema";
  bad_in.schema_version = 1;
  bad_in.external_type = "int";
  bad_in.external_slots = {ExternalSlotDefinition(
      "slot1", "int", PortDirection::kInput, true, "int", "")};
  bad_in.max_batch_size = 64;
  bad_in.logical_ports = {
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  bad_in.decode_fn = &DummyDecode;

  EXPECT_FALSE(reg.RegisterInputConverter(bad_in));

  // 2. Output converter with empty type_suffix must be rejected
  OutputConverterDefinition bad_out;
  bad_out.converter_id = "test.empty_suffix.out";

  bad_out.schema_id = "test_schema";
  bad_out.schema_version = 1;
  bad_out.external_type = "int";
  bad_out.external_slots = {ExternalSlotDefinition(
      "slot1", "int", PortDirection::kOutput, true, "int", "")};
  bad_out.max_batch_size = 64;
  bad_out.logical_ports = {
      NodePortDefinition("answers", "TextBatch", true, "1:1")};
  bad_out.encode_fn = &DummyEncode;

  EXPECT_FALSE(reg.RegisterOutputConverter(bad_out));

  // 3. Multiple slots of same ValueType with distinct slot names can register
  // successfully
  InputConverterDefinition multi_in;
  multi_in.converter_id = "test.multi_slot.in";

  multi_in.schema_id = "test_schema";
  multi_in.schema_version = 1;
  multi_in.external_type = "int";
  multi_in.external_slots = {
      ExternalSlotDefinition("slot_first", "int", PortDirection::kInput, true,
                             "int", "int_suffix"),
      ExternalSlotDefinition("slot_second", "int", PortDirection::kInput, true,
                             "int", "int_suffix")};
  multi_in.max_batch_size = 64;
  multi_in.logical_ports = {
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  multi_in.decode_fn = &DummyDecode;

  EXPECT_TRUE(reg.RegisterInputConverter(multi_in));
  const auto* found_in = reg.FindInputConverter("test.multi_slot.in");
  ASSERT_NE(found_in, nullptr);
  EXPECT_EQ(found_in->external_slots.size(), 2U);
  EXPECT_EQ(found_in->external_slots[0].slot_name, "slot_first");
  EXPECT_EQ(found_in->external_slots[1].slot_name, "slot_second");
  EXPECT_EQ(found_in->external_slots[0].type_suffix, "int_suffix");
  EXPECT_EQ(found_in->external_slots[1].type_suffix, "int_suffix");
}

TEST(IoConverterTest, OptionalInputSlotsPreserveEveryFramePosition) {
  InputConverterDefinition converter;
  converter.external_slots = {{"required",
                               "CompanyOperatorEntityInput",
                               PortDirection::kInput,
                               true,
                               "CompanyOperatorEntityInput",
                               "entity_in",
                               {},
                               "required"},
                              {"optional",
                               "CompanyOperatorEntityInput",
                               PortDirection::kInput,
                               false,
                               "CompanyOperatorEntityInput",
                               "entity_in",
                               {},
                               "optional"}};
  char text[] = "hello";
  CompanyString sentence{5, text};
  operator_api::NamedIoBatch inputs(5);
  std::vector<std::shared_ptr<CompanyOperatorEntityInput>> payloads;
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto payload = std::make_shared<CompanyOperatorEntityInput>();
    payload->request_id = 100 + i;
    payload->sentence_text = &sentence;
    inputs[i]["test.required"] = payload;
    if (i % 2 == 1) inputs[i]["test.optional"] = payload;
    payloads.push_back(std::move(payload));
  }
  ExternalInputBatchView view;
  std::string error;
  ASSERT_EQ(
      ValidateAndExtractOperatorInputs(inputs, converter, {}, &view, &error), 0)
      << error;
  ASSERT_EQ(view.count, 5U);
  ASSERT_EQ(view.slots.at("optional").size(), 5U);
  for (size_t i = 0; i < inputs.size(); ++i) {
    EXPECT_EQ(view.GetSlot<CompanyOperatorEntityInput>("required", i),
              payloads[i].get());
    const auto* optional =
        view.GetSlot<CompanyOperatorEntityInput>("optional", i);
    EXPECT_EQ(optional, i % 2 == 1 ? payloads[i].get() : nullptr);
    if (i % 2 == 1) {
      ASSERT_NE(optional, nullptr);
      EXPECT_EQ(optional->request_id, 100 + i);
    }
  }
  inputs[2].erase("test.required");
  EXPECT_EQ(
      ValidateAndExtractOperatorInputs(inputs, converter, {}, &view, &error),
      -3);
  EXPECT_NE(error.find("Missing required input slot"), std::string::npos);
  EXPECT_NE(error.find("frame 2"), std::string::npos);
}

TEST(IoConverterTest, HarnessPreservesNamedSlotsTypesAndPoolCapacities) {
  InputConverterDefinition input;
  input.decode_fn = [](const ExternalInputBatchView& view,
                       const InputDecodeOptions&, const InputPortBindings&,
                       AlgContext* context, AdapterStatus*) -> int {
    const auto* a = view.GetSlot<ReproA>("left", 0);
    const auto* b = view.GetSlot<ReproB>("right", 0);
    if (!a || !b || view.GetSlot<ReproB>("left", 0)) return -3;
    return context->Publish("sum", a->a + static_cast<int>(b->b)) ? 0 : -3;
  };
  OutputConverterDefinition output;
  output.encode_fn = [](AlgContext* context, const OutputPortBindings&,
                        const OutputEncodeOptions&,
                        ExternalOutputBatchView* view, size_t* written,
                        AdapterStatus*) -> int {
    auto* a = view->GetSlot<ReproA>("left", 0);
    auto* b = view->GetSlot<ReproB>("right", 0);
    const auto* sum = context->Read<int>("sum");
    if (!a || !b || !sum || view->GetSlot<ReproA>("right", 0)) return -3;
    if (view->GetSlotCapacity("right", "items") < 2) return -4;
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
  input.external_type = "aggregate.input";
  input.external_slots = {{"value", "ReproA"}};
  input.decode_fn = [](const ExternalInputBatchView& view,
                       const InputDecodeOptions&, const InputPortBindings&,
                       AlgContext*, AdapterStatus*) -> int {
    return view.GetSlot<ReproA>("value", 0) ? 0 : -3;
  };
  OutputConverterDefinition output;
  output.external_type = "aggregate.output";
  output.external_slots = {{"value", "ReproB", PortDirection::kOutput}};
  output.encode_fn = [](AlgContext*, const OutputPortBindings&,
                        const OutputEncodeOptions&,
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

TEST(IoConverterTest, TypedBindingsResolveNonIdentityPorts) {
  constexpr auto logical = MakeBlackboardKey<TextBatch>("logical_text");
  constexpr auto actual = MakeBlackboardKey<TextBatch>("storage_text");
  InputPortBindings inputs({BindIoPort(logical, actual)});
  OutputPortBindings outputs({BindIoPort(logical, actual)});
  AlgContext context;
  ASSERT_TRUE(context.Publish(inputs.Key(logical), TextBatch{{0, 0, "hello"}}));
  const auto* value = context.Read(outputs.Key(logical));
  ASSERT_NE(value, nullptr);
  ASSERT_EQ(value->size(), 1U);
  EXPECT_EQ(value->front().data, "hello");
  EXPECT_EQ(context.Read(logical), nullptr);
  EXPECT_STREQ(inputs.Key(logical).name, "storage_text");
  EXPECT_STREQ(outputs.Key(logical).name, "storage_text");
  EXPECT_EQ(BindIoPort(logical), std::make_pair(std::string("logical_text"),
                                                std::string("logical_text")));
}

TEST(IoConverterTest, OutputWriterRequiresExplicitFieldCapacityWithoutWriting) {
  TestOutputBatchView view;
  OutputEncodeOptions options;
  options.converter_id = "test.writer";
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
    EXPECT_EQ(status.AdapterName(), "test.writer");
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
  options.converter_id = "test.writer";
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
  options.converter_id = "test.writer";
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
constexpr auto kRowIds = MakeBlackboardKey<std::vector<uint64_t>>("ids");
constexpr auto kRowTexts = MakeBlackboardKey<TextBatch>("texts");
}  // namespace

TEST(IoConverterTest, DecodeRowsOwnsPayloadsAndSeparatesDuplicateExternalIds) {
  char bytes[] = {'a', '\0', 'b'};
  CompanyString text{3, bytes};
  CompanyOperatorKeywordInput first{42, &text}, second{42, &text};
  ExternalInputBatchView source;
  source.count = 2;
  source.slots["input"] = BorrowInputForTest({&first, &second});
  source.slot_types["input"] = "CompanyOperatorKeywordInput";
  InputDecodeOptions options;
  options.converter_id = "test.rows.input";
  InputPortBindings bindings(
      {{"ids", "actual_ids"}, {"texts", "actual_texts"}});
  AlgContext context;
  AdapterStatus status;
  ASSERT_EQ(DecodeRequestRows<CompanyOperatorKeywordInput>(
                source, options, bindings, &context, &status, 2, "input",
                kRowIds, kRowTexts,
                [](const CompanyOperatorKeywordInput& row, std::string* value) {
                  *value = CopyInputString(*row.sentence_text);
                  return AdapterStatus::Ok();
                }),
            COMPANY_ALG_SUCCESS);
  bytes[0] = 'x';
  const auto* ids = context.Read<std::vector<uint64_t>>("actual_ids");
  ASSERT_NE(ids, nullptr);
  EXPECT_EQ(*ids, (std::vector<uint64_t>{42, 42}));
  const auto* texts = context.Read<TextBatch>("actual_texts");
  ASSERT_NE(texts, nullptr);
  ASSERT_EQ(texts->size(), 2U);
  for (size_t i = 0; i < texts->size(); ++i) {
    EXPECT_EQ((*texts)[i].req_id, i);
    EXPECT_EQ((*texts)[i].sub_id, 0U);
    EXPECT_EQ((*texts)[i].data, std::string("a\0b", 3));
  }
  EXPECT_FALSE(context.Has("ids"));
  EXPECT_FALSE(context.Has("texts"));
}

TEST(IoConverterTest, DecodeRowsReportsCallbackFailureWithoutPublishingBatch) {
  CompanyOperatorKeywordInput first{1, nullptr}, second{2, nullptr};
  ExternalInputBatchView source;
  source.count = 2;
  source.slots["input"] = BorrowInputForTest({&first, &second});
  source.slot_types["input"] = "CompanyOperatorKeywordInput";
  InputDecodeOptions options;
  options.converter_id = "test.rows.input";
  InputPortBindings bindings(
      {{"ids", "actual_ids"}, {"texts", "actual_texts"}});
  AlgContext context;
  AdapterStatus status;
  EXPECT_EQ(DecodeRequestRows<CompanyOperatorKeywordInput>(
                source, options, bindings, &context, &status, 2, "input",
                kRowIds, kRowTexts,
                [](const CompanyOperatorKeywordInput& row, std::string* value) {
                  if (row.request_id == 2)
                    return AdapterStatus::InvalidInput("bad sentence",
                                                       "sentence_text");
                  *value = "accepted";
                  return AdapterStatus::Ok();
                }),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.AdapterName(), options.converter_id);
  EXPECT_EQ(status.SampleIndex(), 1);
  EXPECT_EQ(status.FieldPath(), "sentence_text");
  EXPECT_EQ(status.Message(), "bad sentence");
  EXPECT_FALSE(context.Has("actual_ids"));
  EXPECT_FALSE(context.Has("actual_texts"));
}

TEST(IoConverterTest, EncodeRowsRestoresOrderAndIdsAndChecksWriterCapacity) {
  AlgContext context;
  context.Publish("actual_ids", std::vector<uint64_t>{91, 17});
  context.Publish("actual_texts",
                  TextBatch{{1, 0, "two"}, {0, 0, std::string("a\0b", 3)}});
  OutputPortBindings bindings(
      {{"ids", "actual_ids"}, {"texts", "actual_texts"}});
  OutputEncodeOptions options;
  options.converter_id = "test.rows.output";
  char first_bytes[4] = {}, second_bytes[4] = {};
  CompanyString first_text{0, first_bytes}, second_text{0, second_bytes};
  CompanyOperatorEntityOutput first{}, second{};
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
                &context, bindings, options, &view, &written, &status, "output",
                kRowIds, kRowTexts, encode),
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
                &context, bindings, options, &view, &written, &status, "output",
                kRowIds, kRowTexts, encode),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(status.AdapterName(), options.converter_id);
  EXPECT_EQ(status.SampleIndex(), 0);
  EXPECT_EQ(status.FieldPath(), "entities_json");

  written = 99;
  EXPECT_EQ(EncodeResultRows<CompanyOperatorEntityOutput>(
                &context, bindings, options, &view, &written, &status, "output",
                kRowIds, kRowTexts,
                [](const std::string&, CompanyOperatorEntityOutput* row,
                   const OutputStringWriter&) {
                  return row->request_id == 17
                             ? AdapterStatus::InvalidInput("cannot encode",
                                                           "business_field")
                             : AdapterStatus::Ok();
                }),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(status.AdapterName(), options.converter_id);
  EXPECT_EQ(status.SampleIndex(), 1);
  EXPECT_EQ(status.FieldPath(), "business_field");
  EXPECT_EQ(status.Message(), "cannot encode");
}

}  // namespace llm_edgeflow
