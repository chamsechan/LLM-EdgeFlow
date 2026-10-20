#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <string>

#include "adapter/io_values.h"
#include "adapter/operator/operator_output_pool.h"
#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow {
namespace {
OperatorValueTypeBinding NativeInput() {
  auto binding =
      OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("string");
  if (!binding) throw std::runtime_error("Native string binding is missing");
  binding->canonical_suffix = "registry_input";
  return *binding;
}
OperatorValueTypeBinding NativeOutput() {
  auto binding =
      OperatorValueTypeRegistry::Instance().CopyOutputBinding("json_out", "");
  if (!binding)
    throw std::runtime_error("Native JSON output binding is missing");
  binding->canonical_suffix = "registry_output";
  return *binding;
}

TEST(OperatorValueRegistryTest,
     NativeDescriptorsAreOwnedAndGlobalInitFreezesBothRegistrationKinds) {
  OperatorValueTypeRegistry registry;
  auto input = NativeInput();
  auto output = NativeOutput();
  ASSERT_TRUE(registry.RegisterBinding(input));
  ASSERT_TRUE(registry.RegisterBinding(output));
  ASSERT_TRUE(registry.RegisterOutputAllocator("native_layout", output));
  input.external_c_type_name = "changed";
  output.services["translate"] = 1234;
  const auto copied_input = registry.CopyBindingBySuffix("registry_input");
  ASSERT_TRUE(copied_input);
  EXPECT_EQ(copied_input->external_c_type_name, "MockString");
  EXPECT_EQ(copied_input->value_type, typeid(std::string));
  auto copied_output =
      registry.CopyOutputBinding("registry_output", "native_layout");
  ASSERT_TRUE(copied_output);
  EXPECT_EQ(copied_output->allocation_name, "native_layout");
  EXPECT_EQ(copied_output->ServiceType("translate"), kMockServiceTranslate);
  EXPECT_FALSE(copied_output->ServiceType("unknown"));
  EXPECT_EQ(registry.GetOutputBinding("wrong_suffix", "native_layout"),
            nullptr);
  MockOutput carrier{sizeof(MockOutput), kMockOutputOc,
                     kMockServiceKeywordMatch, nullptr};
  copied_output->write_service_type(&carrier, kMockServiceTranslate);
  EXPECT_EQ(carrier.service_type, kMockServiceTranslate);
  EXPECT_EQ(carrier.type, kMockOutputOc);
  EXPECT_EQ(copied_output->read_service_type(&carrier), kMockServiceTranslate);
  copied_output->services.clear();
  EXPECT_EQ(registry.GetOutputBinding("registry_output", "native_layout")
                ->ServiceType("translate"),
            kMockServiceTranslate);
  ASSERT_EQ(registry.GlobalInit(), 0);
  EXPECT_EQ(registry.GlobalInit(), 0);
  input.canonical_suffix = "late_input";
  EXPECT_FALSE(registry.RegisterBinding(input));
  EXPECT_FALSE(registry.RegisterOutputAllocator("late_layout", NativeOutput()));
  EXPECT_EQ(registry.GetBindingBySuffix("late_input"), nullptr);
  EXPECT_EQ(registry.GetOutputBinding("registry_output", "late_layout"),
            nullptr);
  EXPECT_FALSE(registry.HasConflict());
}

TEST(
    OperatorValueRegistryTest,
    NativePayloadBudgetIncludesOuterAndNestedStorageAndChecksCallbackFailures) {
  auto binding = NativeOutput();
  ResolvedOutputPoolSpec spec;
  spec.type = "registry_output";
  spec.capacities = {{"result_json", 9}};
  const size_t block_bytes = sizeof(MockOutput) + sizeof(MockTextOutput) + 10;
  size_t bytes = 0;
  std::string error;
  ASSERT_TRUE(ComputeOutputPoolPayloadBytes(binding, spec, 3, &bytes, &error))
      << error;
  EXPECT_EQ(bytes, block_bytes * 3);
  ASSERT_TRUE(ComputeOutputPoolPayloadBytes(binding, spec, 0, &bytes, &error))
      << error;
  EXPECT_EQ(bytes, block_bytes * 25);
  EXPECT_FALSE(
      ComputeOutputPoolPayloadBytes(binding, spec, 1025, &bytes, &error));
  EXPECT_EQ(bytes, 0u);
  const auto check = [&](ComputeOutputBlockPayloadBytesFn callback,
                         const char* expected) {
    auto invalid = binding;
    invalid.output_layout.compute_block_payload_bytes = std::move(callback);
    bytes = 99;
    EXPECT_FALSE(
        ComputeOutputPoolPayloadBytes(invalid, spec, 2, &bytes, &error));
    EXPECT_EQ(bytes, 0u);
    EXPECT_NE(error.find(expected), std::string::npos) << error;
    std::shared_ptr<OutputPoolState> pool;
    EXPECT_EQ(OutputPoolState::Create(invalid.canonical_suffix, 2, spec,
                                      &invalid, &pool, &error),
              error::kInvalidOutputAllocation);
    EXPECT_EQ(pool, nullptr);
    EXPECT_NE(error.find(expected), std::string::npos) << error;
  };
  check(
      [](const ResolvedOutputPoolSpec&, size_t* out, std::string*) {
        *out = 0;
        return true;
      },
      "payload is zero");
  check(
      [](const ResolvedOutputPoolSpec&, size_t* out, std::string*) {
        *out = std::numeric_limits<size_t>::max();
        return true;
      },
      "overflowed");
  check([](const ResolvedOutputPoolSpec&, size_t*,
           std::string*) -> bool { throw std::runtime_error("budget fault"); },
        "budget fault");
  check([](const ResolvedOutputPoolSpec&, size_t*,
           std::string*) -> bool { throw 17; },
        "Unknown exception");
  binding.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec&, size_t* out, std::string*) {
        *out = 40 * 1024 * 1024;
        return true;
      };
  EXPECT_FALSE(ComputeOutputPoolPayloadBytes(binding, spec, 2, &bytes, &error));
  EXPECT_EQ(bytes, 0u);
  EXPECT_NE(error.find("64 MiB"), std::string::npos);
}

TEST(OperatorValueRegistryTest,
     JsonOutputBlockClearsOnResetAndRejectsOverflowWithoutPartialWrite) {
  const auto binding = NativeOutput();
  ResolvedOutputPoolSpec spec;
  spec.type = "registry_output";
  spec.capacities = {{"result_json", 4}};
  OwnedExternalBlock block;
  std::string error;
  ASSERT_EQ(binding.allocate_external(spec, &block, &error), 0) << error;
  auto* out = static_cast<MockOutput*>(block.raw_struct);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->byte_size, static_cast<int32_t>(sizeof(MockOutput)));
  EXPECT_EQ(out->type, kMockOutputOc);
  ASSERT_NE(out->text, nullptr);
  EXPECT_EQ(out->text->byte_size, static_cast<int32_t>(sizeof(MockTextOutput)));
  ASSERT_NE(out->text->value, nullptr);

  ASSERT_TRUE(binding.write_value(out, JsonOutputValue{"{}"}, spec).IsOk());
  EXPECT_EQ(std::string(out->text->value, out->text->length), "{}");
  // 刚好等于容量时成功，并保留结尾 NUL。
  ASSERT_TRUE(binding.write_value(out, JsonOutputValue{"[12]"}, spec).IsOk());
  EXPECT_EQ(out->text->length, 4);
  EXPECT_EQ(out->text->value[4], '\0');

  binding.reset_external(out, spec);
  EXPECT_EQ(out->text->length, 0);
  EXPECT_EQ(out->text->value[0], '\0');
  EXPECT_EQ(out->byte_size, static_cast<int32_t>(sizeof(MockOutput)));
  EXPECT_EQ(out->type, kMockOutputOc);

  ASSERT_TRUE(binding.write_value(out, JsonOutputValue{"[12]"}, spec).IsOk());
  const auto overflow =
      binding.write_value(out, JsonOutputValue{"[123]"}, spec);
  EXPECT_EQ(overflow.Code(), error::kInvalidOutput);
  EXPECT_EQ(out->text->length, 0);
  EXPECT_EQ(out->text->value[0], '\0');

  out->text->byte_size = 0;
  EXPECT_FALSE(binding.write_value(out, JsonOutputValue{"{}"}, spec).IsOk());
  out->text->byte_size = sizeof(MockTextOutput);
  out->type = 0;
  EXPECT_FALSE(binding.write_value(out, JsonOutputValue{"{}"}, spec).IsOk());
}

}  // namespace
}  // namespace llm_edgeflow
