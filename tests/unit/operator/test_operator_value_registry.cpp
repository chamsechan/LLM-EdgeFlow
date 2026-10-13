#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "adapter/operator/operator_value_type_registry.h"
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
  EXPECT_EQ(copied_input->external_c_type_name, "CompanyString");
  EXPECT_EQ(copied_input->value_type, typeid(std::string));
  auto copied_output =
      registry.CopyOutputBinding("registry_output", "native_layout");
  ASSERT_TRUE(copied_output);
  EXPECT_EQ(copied_output->allocation_name, "native_layout");
  EXPECT_EQ(copied_output->ServiceType("translate"), kMockServiceTranslate);
  EXPECT_FALSE(copied_output->ServiceType("unknown"));
  EXPECT_EQ(registry.GetOutputBinding("wrong_suffix", "native_layout"),
            nullptr);
  CompanyOperatorOutput carrier{kMockServiceKeywordMatch, nullptr, 7};
  copied_output->write_service_type(&carrier, kMockServiceTranslate);
  EXPECT_EQ(carrier.service_type, kMockServiceTranslate);
  EXPECT_EQ(carrier.status_code, 7);
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
            "CompanyString");
  EXPECT_EQ(registry.GetBindingBySuffix("invalid_root"), nullptr);
  EXPECT_EQ(registry.GlobalInit(), -6);
  OperatorValueTypeRegistry incomplete;
  auto output = NativeOutput();
  output.allocate_external = {};
  ASSERT_TRUE(incomplete.RegisterBinding(output));
  EXPECT_EQ(incomplete.GlobalInit(), -6);
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
  }
}

TEST(
    OperatorValueRegistryTest,
    NativePayloadBudgetIncludesOuterAndNestedStorageAndChecksCallbackFailures) {
  auto binding = NativeOutput();
  ResolvedOutputPoolSpec spec;
  spec.type = "registry_output";
  spec.capacities = {{"result_json", 9}};
  const size_t block_bytes =
      sizeof(CompanyOperatorOutput) + sizeof(CompanyString) + 10;
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
