#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "adapter/operator/operator_output_pool.h"
#include "contracts/error_codes.h"

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

struct ThrowingCopy {
  explicit ThrowingCopy(std::shared_ptr<bool> armed)
      : armed(std::move(armed)) {}
  ThrowingCopy(const ThrowingCopy& other) : armed(other.armed) {
    if (*armed) throw std::runtime_error("injected binding copy failure");
  }
  template <typename... Args>
  int operator()(Args&&...) const {
    return 0;
  }
  std::shared_ptr<bool> armed;
};

TEST(
    OperatorValueRegistryTest,
    InvalidAndDuplicateDeclarationsHaveStickyConflictsWithoutPartialInsertion) {
  OperatorValueTypeRegistry registry;
  auto input = NativeInput();
  ASSERT_TRUE(registry.RegisterBinding(input));
  auto duplicate = input;
  duplicate.external_c_type_name = "replacement";
  EXPECT_FALSE(registry.RegisterBinding(duplicate));
  auto invalid = NativeOutput();
  invalid.canonical_suffix = "invalid_root";
  invalid.allocation_name = "allocator_names_belong_only_to_named_allocators";
  EXPECT_FALSE(registry.RegisterBinding(invalid));
  EXPECT_TRUE(registry.HasConflict());
  EXPECT_EQ(registry.GetBindingBySuffix("registry_input")->external_c_type_name,
            input.external_c_type_name);
  EXPECT_EQ(registry.GetBindingBySuffix("invalid_root"), nullptr);
  EXPECT_EQ(registry.GlobalInit(), error::kRegistryConflict);
  OperatorValueTypeRegistry incomplete;
  auto output = NativeOutput();
  output.allocate_external = {};
  ASSERT_TRUE(incomplete.RegisterBinding(output));
  EXPECT_EQ(incomplete.GlobalInit(), error::kRegistryConflict);
  EXPECT_TRUE(incomplete.HasConflict());
}

TEST(OperatorValueRegistryTest,
     ThrowingCopiesLeaveBindingAndAllocatorMapsUnchangedAndRetryable) {
  auto armed = std::make_shared<bool>(false);
  OperatorValueTypeRegistry registry;
  auto input = NativeInput();
  ASSERT_TRUE(registry.RegisterBinding(input));
  auto next = NativeInput();
  next.canonical_suffix = "next_input";
  next.validate_external = ThrowingCopy(armed);
  const auto* original = registry.GetBindingBySuffix("registry_input");
  *armed = true;
  EXPECT_FALSE(registry.RegisterBinding(next));
  EXPECT_EQ(registry.GetBindingBySuffix("next_input"), nullptr);
  EXPECT_EQ(registry.GetBindingBySuffix("registry_input"), original);
  EXPECT_FALSE(registry.HasConflict());
  *armed = false;
  EXPECT_TRUE(registry.RegisterBinding(next));

  OperatorValueTypeRegistry allocators;
  auto output = NativeOutput();
  ASSERT_TRUE(allocators.RegisterBinding(output));
  ASSERT_TRUE(allocators.RegisterOutputAllocator("first", output));
  auto second = output;
  second.allocate_external = ThrowingCopy(armed);
  const auto* original_allocator =
      allocators.GetOutputBinding("registry_output", "first");
  *armed = true;
  EXPECT_FALSE(allocators.RegisterOutputAllocator("second", second));
  EXPECT_EQ(allocators.GetOutputBinding("registry_output", "second"), nullptr);
  EXPECT_EQ(allocators.GetOutputBinding("registry_output", "first"),
            original_allocator);
  EXPECT_FALSE(allocators.HasConflict());
  *armed = false;
  EXPECT_TRUE(allocators.RegisterOutputAllocator("second", second));
}

TEST(OperatorValueRegistryTest,
     PoolCapacitiesMustMatchNativeFieldsAndMetadataLayout) {
  const auto binding = NativeOutput();
  ResolvedOutputPoolSpec requested;
  requested.type = "registry_output";
  requested.capacities = {{"result_json", 9}};
  ResolvedOutputPoolSpec resolved;
  std::string error;
  ASSERT_TRUE(ResolveOutputPoolSpec(binding, requested, &resolved, &error))
      << error;
  EXPECT_EQ(resolved.GetCapacity("result_json"), 9u);
  std::vector<ResolvedOutputPoolSpec> invalid;
  auto missing = requested;
  missing.capacities.clear();
  invalid.push_back(missing);
  auto unknown = requested;
  unknown.capacities["other"] = 9;
  invalid.push_back(unknown);
  auto zero = requested;
  zero.capacities["result_json"] = 0;
  invalid.push_back(zero);
  auto oversized = requested;
  oversized.capacities["result_json"] = 65537;
  invalid.push_back(oversized);
  auto metadata = requested;
  metadata.meta_num = 1;
  invalid.push_back(metadata);
  auto allocator = requested;
  allocator.allocator = "unselected";
  invalid.push_back(allocator);
  for (size_t i = 0; i < invalid.size(); ++i) {
    SCOPED_TRACE(i);
    resolved = requested;
    EXPECT_FALSE(ResolveOutputPoolSpec(binding, invalid[i], &resolved, &error));
    EXPECT_TRUE(resolved.type.empty());
    EXPECT_TRUE(resolved.capacities.empty());
    EXPECT_FALSE(error.empty());
    size_t bytes = 99;
    EXPECT_FALSE(
        ComputeOutputPoolPayloadBytes(binding, invalid[i], 2, &bytes, &error));
    EXPECT_EQ(bytes, 0u);
    std::shared_ptr<OutputPoolState> pool;
    EXPECT_EQ(OutputPoolState::Create(binding.canonical_suffix, 2, invalid[i],
                                      &binding, &pool, &error),
              error::kInvalidOutputAllocation);
    EXPECT_EQ(pool, nullptr);
  }
}

TEST(OperatorValueRegistryTest,
     NormalizationOwnsTypedValuesAndRejectsMissingResultAndExceptions) {
  struct Params {
    int value;
  };
  auto binding = NativeOutput();
  binding.normalize_parameters = MakeOutputParameterParser<Params>(
      [](const std::string& input, Params* result, std::string*) {
        result->value = std::stoi(input);
        return true;
      });
  std::shared_ptr<const OutputAllocationParameters> parameters;
  std::string error;
  ASSERT_TRUE(NormalizeOutputParameters(binding, "7", &parameters, &error));
  ResolvedOutputPoolSpec spec;
  spec.params = parameters;
  EXPECT_EQ(spec.Parameters<Params>().value, 7);
  EXPECT_THROW(spec.Parameters<int>(), std::invalid_argument);
  EXPECT_FALSE(
      NormalizeOutputParameters(binding, "bad-number", &parameters, &error));
  EXPECT_EQ(parameters, nullptr);
  EXPECT_EQ(spec.Parameters<Params>().value, 7);
  binding.normalize_parameters =
      [](const std::string&, std::shared_ptr<const OutputAllocationParameters>*,
         std::string*) { return true; };
  EXPECT_FALSE(NormalizeOutputParameters(binding, "", &parameters, &error));
  EXPECT_NE(error.find("did not produce normalized parameters"),
            std::string::npos);
  binding.normalize_parameters =
      [](const std::string&, std::shared_ptr<const OutputAllocationParameters>*,
         std::string*) -> bool { throw 19; };
  EXPECT_FALSE(NormalizeOutputParameters(binding, "", &parameters, &error));
  EXPECT_EQ(parameters, nullptr);
  EXPECT_NE(error.find("Unknown exception"), std::string::npos);
}

}  // namespace
}  // namespace llm_edgeflow
