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
#include "tests/support/scoped_converter_registry.h"

namespace llm_edgeflow {
namespace {

int DummyDecode(const ExternalInputBatchView&, const InputDecodeOptions&,
                AlgContext*, AdapterStatus*) {
  return 0;
}

int DummyEncode(AlgContext*, const OutputEncodeOptions&,
                ExternalOutputBatchView*, size_t* written_count,
                AdapterStatus*) {
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
  ASSERT_NE(out_view.GetPoolSpec("out_slot"), nullptr);
  EXPECT_EQ(out_view.GetPoolSpec("out_slot")->GetCapacity("field_1"), 1024U);
  // 容量只来自租用块的规格：未声明的字段为 0，不回退到调用方给的默认值。
  EXPECT_EQ(out_view.GetPoolSpec("out_slot")->GetCapacity("unknown"), 0U);
}

TEST(IoConverterTest, RegisterAndFindInputConverter) {
  test_support::ScopedConverterRegistry restore;
  auto& reg = IoConverterRegistry::Instance();
  reg.ClearForTesting();

  InputConverterDefinition def;
  def.type = "inputs";
  def.name = "test_input";
  def.slot.type_id = "int";
  def.slot.type_suffix = "inputs";
  def.logical_ports = {NodePortDefinition("texts", "TextBatch", true, "1:1")};
  def.decode_fn = &DummyDecode;

  EXPECT_TRUE(reg.RegisterInputConverter(def));

  const auto* found = reg.FindInputConverter("inputs", "test_input");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->Label(), "inputs/test_input");
  EXPECT_EQ(found->logical_ports.size(), 1U);
  // 查找按（type, name）精确匹配。
  EXPECT_EQ(reg.FindInputConverter("inputs", "other"), nullptr);
  EXPECT_EQ(reg.FindInputConverter("other", "test_input"), nullptr);
  EXPECT_EQ(reg.InputNamesOfType("inputs"),
            std::vector<std::string>{"test_input"});
  EXPECT_EQ(reg.InputTypes(), std::vector<std::string>{"inputs"});

  // 重复注册拒绝并记录冲突
  EXPECT_FALSE(reg.RegisterInputConverter(def));
  EXPECT_TRUE(reg.HasConflict());
}

TEST(IoConverterTest, RegisterAndFindOutputConverter) {
  test_support::ScopedConverterRegistry restore;
  auto& reg = IoConverterRegistry::Instance();
  reg.ClearForTesting();

  OutputConverterDefinition def;
  def.type = "answers";
  def.name = "test_output";
  def.slot.type_id = "int";
  def.slot.type_suffix = "answers";
  def.logical_ports = {NodePortDefinition("answers", "TextBatch", true, "1:1")};
  def.encode_fn = &DummyEncode;

  EXPECT_TRUE(reg.RegisterOutputConverter(def));

  const auto* found = reg.FindOutputConverter("answers", "test_output");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->Label(), "answers/test_output");
  // 输入与输出各有一张表：同名（type, name）互不影响。
  EXPECT_EQ(reg.FindInputConverter("answers", "test_output"), nullptr);
}

TEST(IoConverterTest, RejectsInvalidDefinitions) {
  test_support::ScopedConverterRegistry restore;
  auto& reg = IoConverterRegistry::Instance();
  reg.ClearForTesting();
  InputConverterDefinition bad_in;
  bad_in.decode_fn = &DummyDecode;
  EXPECT_FALSE(reg.RegisterInputConverter(bad_in));  // 缺少 type 和 name

  bad_in.type = "inputs";
  bad_in.name = "bad_in";
  bad_in.slot.type_id = "int";
  bad_in.slot.type_suffix = "inputs";
  bad_in.logical_ports = {
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  bad_in.decode_fn = nullptr;
  EXPECT_FALSE(reg.RegisterInputConverter(bad_in));  // 缺少回调

  // 缺少槽的结构名
  bad_in.decode_fn = &DummyDecode;
  bad_in.slot.type_id.clear();
  EXPECT_FALSE(reg.RegisterInputConverter(bad_in));

  // 缺少 logical_ports
  bad_in.slot.type_id = "int";
  bad_in.logical_ports.clear();
  EXPECT_FALSE(reg.RegisterInputConverter(bad_in));

  // 补齐必需字段后注册成功
  bad_in.logical_ports = {
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  EXPECT_TRUE(reg.RegisterInputConverter(bad_in));

  OutputConverterDefinition bad_out;
  bad_out.type = "answers";
  bad_out.name = "bad_out";
  bad_out.slot.type_id = "int";
  bad_out.slot.type_suffix = "answers";
  bad_out.logical_ports = {
      NodePortDefinition("answers", "TextBatch", true, "1:1")};
  bad_out.encode_fn = nullptr;
  EXPECT_FALSE(reg.RegisterOutputConverter(bad_out));
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

TEST(IoConverterTest, RejectsEmptySlotSuffix) {
  test_support::ScopedConverterRegistry restore;
  auto& reg = IoConverterRegistry::Instance();
  reg.ClearForTesting();

  InputConverterDefinition bad_in;
  bad_in.type = "slot1";
  bad_in.name = "empty_suffix_in";
  bad_in.slot.type_id = "int";
  bad_in.slot.type_suffix = "";
  bad_in.logical_ports = {
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  bad_in.decode_fn = &DummyDecode;
  EXPECT_FALSE(reg.RegisterInputConverter(bad_in));

  OutputConverterDefinition bad_out;
  bad_out.type = "slot1";
  bad_out.name = "empty_suffix_out";
  bad_out.slot.type_id = "int";
  bad_out.slot.type_suffix = "";
  bad_out.logical_ports = {
      NodePortDefinition("answers", "TextBatch", true, "1:1")};
  bad_out.encode_fn = &DummyEncode;
  EXPECT_FALSE(reg.RegisterOutputConverter(bad_out));
}

TEST(IoConverterTest, OptionalInputSlotsPreserveEveryFramePosition) {
  // 两个输入项：entity_in 必填，keyword_in 可选；每个宿主结构体占一个槽。
  InputConverterDefinition required;
  required.type = "entity_in";
  required.name = "required_item";
  required.slot = ExternalInputSlot<CompanyOperatorEntityInput>("entity_in");
  InputConverterDefinition optional;
  optional.type = "keyword_in";
  optional.name = "optional_item";
  optional.slot = ExternalInputSlot<CompanyOperatorKeywordInput>("keyword_in");
  optional.slot.required = false;
  const std::vector<SelectedInput> items = {{&required, nullptr},
                                            {&optional, nullptr}};
  char text[] = "hello";
  CompanyString sentence{5, text};
  operator_api::NamedIoBatch inputs(5);
  std::vector<std::shared_ptr<CompanyOperatorEntityInput>> required_payloads;
  std::vector<std::shared_ptr<CompanyOperatorKeywordInput>> optional_payloads;
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto payload = std::make_shared<CompanyOperatorEntityInput>();
    payload->request_id = 100 + i;
    payload->sentence_text = &sentence;
    // 唯一 type 的前缀不应被另一项的业务名占用。
    inputs[i]["optional_item.entity_in"] = payload;
    required_payloads.push_back(std::move(payload));
    auto extra = std::make_shared<CompanyOperatorKeywordInput>();
    extra->request_id = 100 + i;
    extra->sentence_text = &sentence;
    if (i % 2 == 1) inputs[i]["test.keyword_in"] = extra;
    optional_payloads.push_back(std::move(extra));
  }
  std::vector<ExternalInputBatchView> views;
  std::string error;
  ASSERT_EQ(ValidateAndExtractOperatorInputs(inputs, items, {}, &views, &error),
            0)
      << error;
  ASSERT_EQ(views.size(), 2U);
  ASSERT_EQ(views[0].count, 5U);
  ASSERT_EQ(views[1].slots.at("keyword_in").size(), 5U);
  for (size_t i = 0; i < inputs.size(); ++i) {
    EXPECT_EQ(views[0].GetSlot<CompanyOperatorEntityInput>("entity_in", i),
              required_payloads[i].get());
    const auto* extra =
        views[1].GetSlot<CompanyOperatorKeywordInput>("keyword_in", i);
    EXPECT_EQ(extra, i % 2 == 1 ? optional_payloads[i].get() : nullptr);
    if (i % 2 == 1) {
      ASSERT_NE(extra, nullptr);
      EXPECT_EQ(extra->request_id, 100 + i);
    }
  }
  inputs[2].erase("optional_item.entity_in");
  EXPECT_EQ(ValidateAndExtractOperatorInputs(inputs, items, {}, &views, &error),
            -3);
  EXPECT_NE(error.find("Missing required input slot"), std::string::npos);
  EXPECT_NE(error.find("frame 2"), std::string::npos);
}

TEST(IoConverterTest, RepeatedInputTypesUseExplicitNamesAndSeparateViews) {
  InputConverterDefinition first;
  first.type = "keyword_in";
  first.name = "first";
  first.service_type = 9101;
  first.slot = ExternalInputSlot<CompanyOperatorKeywordInput>(first.type);
  auto second = first;
  second.name = "second";
  second.service_type = 9102;
  second.slot.required = false;
  const std::vector<SelectedInput> items = {{&first, nullptr},
                                            {&second, nullptr}};
  char text[] = "hello";
  CompanyString sentence{5, text};
  CompanyOperatorKeywordInput a{42, &sentence, 9101};
  CompanyOperatorKeywordInput b{42, &sentence, 9102};
  operator_api::NamedIoBatch inputs(2);
  inputs[0]["first.keyword_in"] = operator_api::MakeBorrowedOperatorInput(&a);
  inputs[0]["second.keyword_in"] = operator_api::MakeBorrowedOperatorInput(&b);
  inputs[1]["first.keyword_in"] = operator_api::MakeBorrowedOperatorInput(&a);
  std::vector<ExternalInputBatchView> views;
  std::string error;
  ASSERT_EQ(ValidateAndExtractOperatorInputs(inputs, items, {}, &views, &error),
            0)
      << error;
  ASSERT_EQ(views.size(), 2U);
  EXPECT_EQ(views[0].GetSlot<CompanyOperatorKeywordInput>("keyword_in", 0), &a);
  EXPECT_EQ(views[1].GetSlot<CompanyOperatorKeywordInput>("keyword_in", 0), &b);
  EXPECT_EQ(views[1].GetSlot<CompanyOperatorKeywordInput>("keyword_in", 1),
            nullptr);
  EXPECT_EQ(views[0].slots.size(), 1U);
  EXPECT_EQ(views[1].slots.size(), 1U);

  b.service_type = 9101;
  EXPECT_EQ(ValidateAndExtractOperatorInputs(inputs, items, {}, &views, &error),
            -3);
  EXPECT_NE(error.find("service_type mismatch"), std::string::npos);
  b.service_type = 9102;
  inputs[0].erase("second.keyword_in");
  inputs[0]["unidentified.keyword_in"] =
      operator_api::MakeBorrowedOperatorInput(&b);
  EXPECT_EQ(ValidateAndExtractOperatorInputs(inputs, items, {}, &views, &error),
            -3);
  EXPECT_NE(error.find("ambiguous"), std::string::npos);

  inputs[0].erase("unidentified.keyword_in");
  inputs[0]["second.keyword_in"] = operator_api::MakeBorrowedOperatorInput(&b);
  const std::vector<SelectedInput> reversed = {{&second, nullptr},
                                               {&first, nullptr}};
  ASSERT_EQ(
      ValidateAndExtractOperatorInputs(inputs, reversed, {}, &views, &error), 0)
      << error;
  EXPECT_EQ(views[0].GetSlot<CompanyOperatorKeywordInput>("keyword_in", 0), &b);
  EXPECT_EQ(views[1].GetSlot<CompanyOperatorKeywordInput>("keyword_in", 0), &a);
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
    const auto* right_spec = view->GetPoolSpec("right");
    if (!right_spec || right_spec->GetCapacity("items") < 2) return -4;
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
  input.slot.type_id = "ReproA";
  input.slot.type_suffix = "value";
  input.decode_fn = [](const ExternalInputBatchView& view,
                       const InputDecodeOptions&, AlgContext*,
                       AdapterStatus*) -> int {
    return view.GetSlot<ReproA>("value", 0) ? 0 : -3;
  };
  OutputConverterDefinition output;
  output.slot.type_id = "ReproB";
  output.slot.type_suffix = "value";
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

TEST(IoConverterTest, OutputWriterRequiresExplicitFieldCapacityWithoutWriting) {
  TestOutputBatchView view;
  OutputEncodeOptions options;
  options.type = "writer";
  options.name = "test";
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
    EXPECT_EQ(status.AdapterName(), "writer/test");
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
  options.type = "writer";
  options.name = "test";
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
  options.type = "writer";
  options.name = "test";
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
  CompanyOperatorKeywordInput first{42, &text, 0}, second{42, &text, 0};
  ExternalInputBatchView source;
  source.count = 2;
  source.slots["input"] = BorrowInputForTest({&first, &second});
  source.slot_types["input"] = "CompanyOperatorKeywordInput";
  InputDecodeOptions options;
  options.type = "input";
  options.name = "rows";
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
  CompanyOperatorKeywordInput first{1, nullptr, 0}, second{2, nullptr, 0};
  ExternalInputBatchView source;
  source.count = 2;
  source.slots["input"] = BorrowInputForTest({&first, &second});
  source.slot_types["input"] = "CompanyOperatorKeywordInput";
  InputDecodeOptions options;
  options.type = "input";
  options.name = "rows";
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

// 宿主结构体不带 request_id 成员时，解码只发布数据，请求 ID 表保持为空，
// 由同一批的其他输入项提供（多项输入按批内序号配对）。
TEST(IoConverterTest, DecodeRowsWithoutRequestIdMemberLeavesIdTableEmpty) {
  char bytes[] = {'q', 'a'};
  CompanyString first{2, bytes}, second{1, bytes};
  ExternalInputBatchView source;
  source.count = 2;
  source.slots["string"] = BorrowInputForTest({&first, &second});
  source.slot_types["string"] = "CompanyString";
  InputDecodeOptions options;
  options.type = "string";
  options.name = "rows";
  std::vector<uint64_t> request_ids;
  options.request_ids = &request_ids;
  AlgContext context;
  AdapterStatus status;
  ASSERT_EQ(DecodeRequestRows<CompanyString>(
                source, options, &context, &status, "string", kRowTexts,
                [](const CompanyString& row, std::string* value) {
                  *value = CopyInputString(row);
                  return AdapterStatus::Ok();
                }),
            COMPANY_ALG_SUCCESS);
  EXPECT_TRUE(request_ids.empty());
  const auto* texts = context.Read(kRowTexts);
  ASSERT_NE(texts, nullptr);
  ASSERT_EQ(texts->size(), 2U);
  EXPECT_EQ((*texts)[0].data, "qa");
  EXPECT_EQ((*texts)[1].data, "q");
}

TEST(IoConverterTest, EncodeRowsRestoresOrderAndIdsAndChecksWriterCapacity) {
  AlgContext context;
  const std::vector<uint64_t> request_ids{91, 17};
  context.Publish(kRowTexts,
                  TextBatch{{1, 0, "two"}, {0, 0, std::string("a\0b", 3)}});
  OutputEncodeOptions options;
  options.type = "output";
  options.name = "rows";
  options.request_ids = &request_ids;
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
