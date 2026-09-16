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

TEST(IoConverterTest, ViewAccessorsAndPortBindings) {
  // 1. ExternalInputBatchView C ABI 与 Slot 访问
  ExternalInputBatchView in_view;
  int sample_int = 42;
  const void* items[] = {&sample_int};
  in_view.items = items;
  in_view.count = 1;
  in_view.type_id = "int";

  EXPECT_EQ(in_view.GetCAbi<int>(0), &sample_int);
  EXPECT_EQ(in_view.GetCAbi<int>(1), nullptr);
  EXPECT_EQ(in_view.GetCAbi<float>(0), nullptr);
  EXPECT_EQ(in_view.At<int>(0), &sample_int);

  auto shared_sample = std::make_shared<int>(100);
  in_view.slots["slot_a"] = {shared_sample};
  in_view.slot_types["slot_a"] = "int";
  EXPECT_EQ(in_view.GetSlot<int>("slot_a", 0), shared_sample.get());
  EXPECT_EQ(in_view.GetSlot<float>("slot_a", 0), nullptr);
  EXPECT_EQ(in_view.GetSlot<int>("slot_a", 1), nullptr);
  EXPECT_EQ(in_view.GetSlot<int>("unknown", 0), nullptr);
  EXPECT_EQ(in_view.At<int>(0, "slot_a"), shared_sample.get());

  // 2. ExternalOutputBatchView 访问
  ExternalOutputBatchView out_view;
  int out_sample = 0;
  void* out_items[] = {&out_sample};
  out_view.items = out_items;
  out_view.count = 1;
  out_view.type_id = "int";
  EXPECT_EQ(out_view.GetCAbi<int>(0), &out_sample);
  EXPECT_EQ(out_view.GetCAbi<float>(0), nullptr);

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
  def.converter_id = "test.input.cabi.v1";
  def.transport = "cabi";
  def.schema_id = "test_input";
  def.schema_version = 1;
  def.external_type = "int";
  def.external_slots = {
      ExternalSlotDefinition("inputs", "int", PortDirection::kInput, true)};
  def.max_batch_size = 64;
  def.logical_ports = {NodePortDefinition("texts", "TextBatch", true, "1:1")};
  def.decode_fn = &DummyDecode;

  EXPECT_TRUE(reg.RegisterInputConverter(def));

  const auto* found = reg.FindInputConverter("test.input.cabi.v1");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->converter_id, "test.input.cabi.v1");
  EXPECT_EQ(found->transport, "cabi");
  EXPECT_EQ(found->logical_ports.size(), 1U);

  // 重复注册拒绝并记录冲突
  EXPECT_FALSE(reg.RegisterInputConverter(def));
  EXPECT_TRUE(reg.HasConflict());
}

TEST(IoConverterTest, RegisterAndFindOutputConverter) {
  auto& reg = IoConverterRegistry::Instance();

  OutputConverterDefinition def;
  def.converter_id = "test.output.cabi.v1";
  def.transport = "cabi";
  def.schema_id = "test_output";
  def.schema_version = 1;
  def.external_type = "int";
  def.external_slots = {
      ExternalSlotDefinition("answers", "int", PortDirection::kOutput, true)};
  def.max_batch_size = 64;
  def.logical_ports = {NodePortDefinition("answers", "TextBatch", true, "1:1")};
  def.encode_fn = &DummyEncode;

  EXPECT_TRUE(reg.RegisterOutputConverter(def));

  const auto* found = reg.FindOutputConverter("test.output.cabi.v1");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->converter_id, "test.output.cabi.v1");
  EXPECT_EQ(found->transport, "cabi");
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
  bad_in.external_slots = {
      ExternalSlotDefinition("inputs", "int", PortDirection::kInput, true)};
  bad_in.max_batch_size = 64;
  bad_in.logical_ports = {
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  bad_in.transport = "cabi";
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
  bad_in.external_slots = {
      ExternalSlotDefinition("inputs", "int", PortDirection::kInput, true)};
  bad_in.logical_ports.clear();
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  // max_batch_size == 0
  bad_in.logical_ports = {
      NodePortDefinition("texts", "TextBatch", true, "1:1")};
  bad_in.max_batch_size = 0;
  EXPECT_FALSE(IoConverterRegistry::Instance().RegisterInputConverter(bad_in));

  OutputConverterDefinition bad_out;
  bad_out.converter_id = "bad.out";
  bad_out.transport = "cabi";
  bad_out.schema_id = "test";
  bad_out.schema_version = 1;
  bad_out.external_type = "int";
  bad_out.external_slots = {
      ExternalSlotDefinition("answers", "int", PortDirection::kOutput, true)};
  bad_out.max_batch_size = 64;
  bad_out.logical_ports = {
      NodePortDefinition("answers", "TextBatch", true, "1:1")};
  bad_out.encode_fn = nullptr;
  EXPECT_FALSE(
      IoConverterRegistry::Instance().RegisterOutputConverter(bad_out));
}

}  // namespace llm_edgeflow
