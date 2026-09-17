#include <gtest/gtest.h>

#include "adapter/converter_authoring.h"
#include "adapter/io_converter.h"
#include "adapter/io_converter_registry.h"
#include "contracts/inference_payloads.h"

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
  in_view.leased_slots["slot_a"] = {&sample_int};
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
  ExternalOutputBatchView out_view;
  int out_sample = 0;
  out_view.count = 1;
  out_view.leased_slots["out_slot"] = {&out_sample};
  out_view.slot_types["out_slot"] = "int";
  out_view.slot_capacities["out_slot"]["field_1"] = 1024;
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
  def.transport = "operator";
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
  EXPECT_EQ(found->transport, "operator");
  EXPECT_EQ(found->logical_ports.size(), 1U);

  // 重复注册拒绝并记录冲突
  EXPECT_FALSE(reg.RegisterInputConverter(def));
  EXPECT_TRUE(reg.HasConflict());
}

TEST(IoConverterTest, RegisterAndFindOutputConverter) {
  auto& reg = IoConverterRegistry::Instance();

  OutputConverterDefinition def;
  def.converter_id = "test.output.operator.v1";
  def.transport = "operator";
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
  EXPECT_EQ(found->transport, "operator");
}

TEST(IoConverterTest, RejectsInvalidDefinitions) {
  InputConverterDefinition bad_in;
  bad_in.converter_id = "";
  bad_in.decode_fn = &DummyDecode;
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  bad_in.converter_id = "bad.in.transport";
  bad_in.transport = "invalid_transport";
  bad_in.schema_id = "test";
  bad_in.schema_version = 1;
  bad_in.external_type = "int";
  bad_in.external_slots = {ExternalSlotDefinition(
      "inputs", "int", PortDirection::kInput, true, "int", "inputs")};
  bad_in.max_batch_size = 64;
  bad_in.logical_ports = {
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  bad_in.transport = "cabi";  // cabi must be rejected!
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  bad_in.transport = "operator";
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
  bad_out.transport = "operator";
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
  v.type_id = "ReproA";
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

  // 2. leased_slots path
  ExternalInputBatchView v_leased;
  v_leased.count = 1;
  v_leased.type_id = "ReproA";
  ReproB raw_b;
  v_leased.leased_slots["channel.slot"] = {&raw_b};
  v_leased.slot_types["channel.slot"] = "ReproB";

  // Exact lookup with correct type succeeds
  EXPECT_EQ(v_leased.GetSlot<ReproB>("channel.slot", 0), &raw_b);
  // Exact lookup with wrong type returns nullptr
  EXPECT_EQ(v_leased.GetSlot<ReproA>("channel.slot", 0), nullptr);
  // Short suffix lookup must NOT fallback: returns nullptr
  EXPECT_EQ(v_leased.GetSlot<ReproB>("slot", 0), nullptr);
  EXPECT_EQ(v_leased.GetSlot<ReproA>("slot", 0), nullptr);

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
  bad_in.transport = "operator";
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
  bad_out.transport = "operator";
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
  multi_in.transport = "operator";
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

}  // namespace llm_edgeflow
