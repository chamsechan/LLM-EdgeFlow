#include <gtest/gtest.h>

#include <charconv>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "adapter/converter_authoring.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/operator_value_type.h"
#include "core/alg_context.h"
#include "scoped_allocation_failure.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"
#include "tests/support/operator_nested_output_fixture.h"

namespace llm_edgeflow {

struct BusinessInput {
  size_t text_bytes = 0;
};

DECLARE_EXTERNAL_TYPE_TRAITS(BusinessInput, "BusinessInput");

namespace {

struct PlainOutputParameters {
  uint32_t count = 0;
};

bool ParsePlainOutput(const std::string& text, PlainOutputParameters* output,
                      std::string* error) {
  uint32_t count = 0;
  if (text.compare(0, 6, "count=") == 0) {
    const auto parsed =
        std::from_chars(text.data() + 6, text.data() + text.size(), count);
    if (parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() &&
        count > 0 && count <= 65536) {
      output->count = count;
      return true;
    }
  }
  if (error) *error = "Expected count=<positive integer>";
  return false;
}

void SetMinimalOutputContract(OperatorValueTypeBinding* binding) {
  ASSERT_NE(binding, nullptr);
  binding->direction = IoDirection::kOutput;
  binding->output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec&, size_t* out_bytes,
         std::string*) noexcept {
        if (!out_bytes) return false;
        *out_bytes = 1;
        return true;
      };
}

}  // namespace

TEST(OperatorValueRegistryTest,
     NamedAllocatorsSelectLayoutAndFreezeIndependently) {
  OperatorValueTypeRegistry registry;
  const auto root = test_support::MakeNestedOutputBinding();
  ASSERT_TRUE(registry.RegisterBinding(root));
  ASSERT_TRUE(registry.RegisterOutputAllocator("nested_one", root));
  ASSERT_TRUE(registry.RegisterOutputAllocator(
      "nested_two", test_support::MakeNestedOutputBinding(2)));
  ASSERT_EQ(registry.GlobalInit(), 0);
  const auto* first =
      registry.GetOutputBinding(root.canonical_suffix, "nested_one");
  const auto* second =
      registry.GetOutputBinding(root.canonical_suffix, "nested_two");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, second);
  EXPECT_EQ(first->external_c_type_name, second->external_c_type_name);
  EXPECT_EQ(first->allocation_name, "nested_one");
  EXPECT_EQ(second->allocation_name, "nested_two");
  EXPECT_NE(registry.GetOutputBinding(root.canonical_suffix, ""), nullptr);
  EXPECT_EQ(registry.GetOutputBinding(root.canonical_suffix, "unknown"),
            nullptr);
  EXPECT_EQ(registry.GetOutputBinding("keyword_out", "nested_one"), nullptr);
  EXPECT_FALSE(registry.RegisterOutputAllocator("late", root));
  EXPECT_FALSE(registry.HasConflict());
  EXPECT_EQ(registry.GlobalInit(), 0);

  ResolvedOutputPoolSpec requested;
  requested.type = root.canonical_suffix;
  requested.allocator = "nested_two";
  ResolvedOutputPoolSpec resolved;
  std::string error;
  ASSERT_TRUE(NormalizeOutputParameters(*second, R"({"kind":2})",
                                        &requested.params, &error))
      << error;
  ASSERT_TRUE(ResolveOutputPoolSpec(*second, requested, &resolved, &error))
      << error;
  EXPECT_EQ(
      resolved.Parameters<test_support::NestedOutputParameters>().capacity, 3u);
  EXPECT_FALSE(
      resolved.Parameters<test_support::NestedOutputParameters>().reject_hit);
  ResolvedOutputPoolSpec repeated;
  ASSERT_TRUE(ResolveOutputPoolSpec(*second, resolved, &repeated, &error))
      << error;
  EXPECT_EQ(repeated.params, resolved.params);
  EXPECT_FALSE(ResolveOutputPoolSpec(*first, requested, &resolved, &error));

  struct WrongParameters {};
  EXPECT_THROW(requested.Parameters<WrongParameters>(), std::invalid_argument);
}

TEST(OperatorValueRegistryTest,
     ParameterNormalizationAllocationFailureRollsBackAndAllowsRetry) {
  auto binding = test_support::MakeNestedOutputBinding();
  binding.normalize_parameters =
      MakeOutputParameterParser<PlainOutputParameters>(ParsePlainOutput);
  const std::string raw_parameters = "count=7";
  bool normalized = true;
  bool published = true;
  bool injected = false;
  size_t outstanding = 1;
  {
    test_support::ScopedAllocationFailure failure(0);
    {
      std::shared_ptr<const OutputAllocationParameters> parameters;
      std::string error;
      normalized = NormalizeOutputParameters(binding, raw_parameters,
                                             &parameters, &error);
      failure.DisableFailure();
      published = parameters != nullptr;
    }
    injected = failure.Triggered();
    outstanding = failure.Outstanding();
  }
  EXPECT_TRUE(injected);
  EXPECT_FALSE(normalized);
  EXPECT_FALSE(published);
  EXPECT_EQ(outstanding, 0u);

  ResolvedOutputPoolSpec recovered;
  std::string error;
  ASSERT_TRUE(NormalizeOutputParameters(binding, raw_parameters,
                                        &recovered.params, &error))
      << error;
  EXPECT_EQ(recovered.Parameters<PlainOutputParameters>().count, 7u);
}

TEST(OperatorValueRegistryTest, OrdinaryParameterStructCanParseNonJsonText) {
  auto binding = test_support::MakeNestedOutputBinding();
  binding.normalize_parameters =
      MakeOutputParameterParser<PlainOutputParameters>(ParsePlainOutput);
  ResolvedOutputPoolSpec spec;
  std::string error;
  ASSERT_TRUE(
      NormalizeOutputParameters(binding, "count=12", &spec.params, &error))
      << error;
  EXPECT_EQ(spec.Parameters<PlainOutputParameters>().count, 12u);
  for (const char* invalid : {"", "count=", "count=0", "count=-1", "count=7x",
                              "count=65537", "{\"count\":12}"}) {
    EXPECT_FALSE(
        NormalizeOutputParameters(binding, invalid, &spec.params, &error))
        << invalid;
    EXPECT_EQ(spec.params, nullptr);
  }
}

TEST(OperatorValueRegistryTest,
     NamedAllocatorAuditRejectsInvalidRootAndCallbacks) {
  for (int mutation = 0; mutation < 7; ++mutation) {
    SCOPED_TRACE(mutation);
    OperatorValueTypeRegistry registry;
    const auto root = test_support::MakeNestedOutputBinding();
    ASSERT_TRUE(registry.RegisterBinding(root));
    auto allocator = root;
    if (mutation == 0) allocator.canonical_suffix = "unregistered_output";
    if (mutation == 1) allocator.external_c_type_name = "DifferentOuterStruct";
    if (mutation == 2) allocator.allocate_external = {};
    if (mutation == 3) allocator.reset_external = {};
    if (mutation == 4) allocator.destroy_external = {};
    if (mutation == 5) allocator.output_layout.compute_block_payload_bytes = {};
    if (mutation == 6) allocator.direction = IoDirection::kInput;
    // 为适应静态注册顺序，根类型交叉检查可推迟到 Init。
    registry.RegisterOutputAllocator("invalid", allocator);
    EXPECT_EQ(registry.GlobalInit(), -6);
    EXPECT_TRUE(registry.HasConflict());
  }
  for (const char* name : {"", "duplicate"}) {
    OperatorValueTypeRegistry registry;
    const auto binding = test_support::MakeNestedOutputBinding();
    ASSERT_TRUE(registry.RegisterBinding(binding));
    if (*name) {
      ASSERT_TRUE(registry.RegisterOutputAllocator(name, binding));
    }
    EXPECT_FALSE(registry.RegisterOutputAllocator(name, binding));
    EXPECT_EQ(registry.GlobalInit(), -6);
  }
}

TEST(OperatorValueRegistryTest,
     OutputParametersRejectUnsupportedAndMalformedData) {
  const auto* builtin =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("keyword_out");
  ASSERT_NE(builtin, nullptr);
  ResolvedOutputPoolSpec requested;
  requested.type = "keyword_out";
  ResolvedOutputPoolSpec resolved;
  std::string error;
  EXPECT_FALSE(NormalizeOutputParameters(*builtin, R"({"kind":1})",
                                         &requested.params, &error));

  auto binding = test_support::MakeNestedOutputBinding();
  requested.type = binding.canonical_suffix;
  for (const char* params : {"[]", R"({"kind":3})", R"({"capacity":-1})",
                             R"({"unknown":1})", "invalid"}) {
    EXPECT_FALSE(
        NormalizeOutputParameters(binding, params, &requested.params, &error));
  }
  // 归一化成功时必须提供类型化参数；回调出错时不能让部分参数
  // 对池创建可见。
  binding.normalize_parameters =
      [](const std::string&,
         std::shared_ptr<const OutputAllocationParameters>* result,
         std::string*) {
        result->reset();
        return true;
      };
  EXPECT_FALSE(
      NormalizeOutputParameters(binding, "{}", &requested.params, &error));
  binding.normalize_parameters =
      [](const std::string&,
         std::shared_ptr<const OutputAllocationParameters>* result,
         std::string*) {
        *result = std::make_shared<OutputAllocationParameters>();
        return false;
      };
  EXPECT_FALSE(
      NormalizeOutputParameters(binding, "{}", &requested.params, &error));
  EXPECT_EQ(requested.params, nullptr);
  binding.normalize_parameters =
      [](const std::string&, std::shared_ptr<const OutputAllocationParameters>*,
         std::string*) -> bool {
    throw std::runtime_error("normalization failed");
  };
  EXPECT_FALSE(
      NormalizeOutputParameters(binding, "{}", &requested.params, &error));
  EXPECT_NE(error.find("normalization failed"), std::string::npos);

  binding = test_support::MakeNestedOutputBinding();
  requested.params.reset();
  EXPECT_FALSE(ResolveOutputPoolSpec(binding, requested, &resolved, &error));
  ASSERT_TRUE(
      NormalizeOutputParameters(binding, "{}", &requested.params, &error));
  requested.type = "keyword_out";
  requested.capacities["match_result_json"] = 2047;
  EXPECT_FALSE(ResolveOutputPoolSpec(*builtin, requested, &resolved, &error));
}

TEST(OperatorValueRegistryTest, BuiltinInputsPreserveNullDiagnostics) {
  const char* suffixes[] = {"string",     "buffer",    "any",    "frame",
                            "keyword_in", "entity_in", "doc_in", "audit_in",
                            "audio_in",   "rerank_in"};
  InputLimits limits;
  for (const char* suffix : suffixes) {
    SCOPED_TRACE(suffix);
    const auto* binding =
        OperatorValueTypeRegistry::Instance().GetBindingBySuffix(suffix);
    ASSERT_NE(binding, nullptr);
    ASSERT_EQ(binding->direction, IoDirection::kInput);
    ASSERT_TRUE(binding->validate_external);
    std::string err;
    EXPECT_EQ(binding->validate_external(nullptr, limits, &err), -3);
    EXPECT_EQ(err, binding->external_c_type_name + " pointer is null");
    EXPECT_EQ(binding->validate_external(nullptr, limits, nullptr), -3);
  }
}

// 1. Any 类型白名单与尺寸查找
TEST(OperatorValueRegistryTest, CompanyAnyTypeWhitelistAndSizes) {
  const auto* t0 = FindCompanyAnyType(0);
  ASSERT_NE(t0, nullptr);
  EXPECT_EQ(t0->element_size, 0u);

  const auto* t1 = FindCompanyAnyType(1);
  ASSERT_NE(t1, nullptr);
  EXPECT_EQ(t1->element_size, sizeof(float));

  const auto* t2 = FindCompanyAnyType(2);
  ASSERT_NE(t2, nullptr);
  EXPECT_EQ(t2->element_size, sizeof(int32_t));

  const auto* t3 = FindCompanyAnyType(3);
  ASSERT_NE(t3, nullptr);
  EXPECT_EQ(t3->element_size, sizeof(uint8_t));

  const auto* t4 = FindCompanyAnyType(4);
  ASSERT_NE(t4, nullptr);
  EXPECT_EQ(t4->element_size, sizeof(int64_t));

  const auto* t5 = FindCompanyAnyType(5);
  ASSERT_NE(t5, nullptr);
  EXPECT_EQ(t5->element_size, sizeof(double));

  // 未白名单类型
  EXPECT_EQ(FindCompanyAnyType(-1), nullptr);
  EXPECT_EQ(FindCompanyAnyType(99), nullptr);
}

// 2. CheckedMultiply 溢出与边界检测
TEST(OperatorValueRegistryTest, CheckedMultiplySafety) {
  size_t out = 0;
  EXPECT_TRUE(CheckedMultiply(10, 20, &out));
  EXPECT_EQ(out, 200u);

  EXPECT_TRUE(CheckedMultiply(0, std::numeric_limits<size_t>::max(), &out));
  EXPECT_EQ(out, 0u);

  // 溢出
  EXPECT_FALSE(CheckedMultiply(std::numeric_limits<size_t>::max(), 2, &out));
  EXPECT_FALSE(
      CheckedMultiply(std::numeric_limits<size_t>::max() / 2 + 1, 2, &out));
}

// 3. CompanyAny 尺寸方程校验与 Fail-Closed
TEST(OperatorValueRegistryTest, CompanyAnyValidationSuite) {
  std::string err;
  uint8_t dummy[64] = {0};

  // 1. 空指针 -> -3
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyAnyPayload(nullptr, 1024,
                                                                 "test", &err),
            -3);

  // 2. 负数字段 -> -3
  CompanyAny any_neg_cnt{1, -1, 4, dummy};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_neg_cnt, 1024, "test", &err),
            -3);

  CompanyAny any_neg_len{1, 1, -4, dummy};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_neg_len, 1024, "test", &err),
            -3);

  // 3. 未白名单 type_id -> -3
  CompanyAny any_unknown_type{999, 1, 4, dummy};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_unknown_type, 1024, "test", &err),
            -3);

  // 4. type_id 为 0 但 count/len 非零 -> -3
  CompanyAny any_zero_nonzero{0, 1, 4, dummy};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_zero_nonzero, 1024, "test", &err),
            -3);

  // 5. type_id 为 0 且 count/len 为零 -> 0 (合法无 metadata)
  CompanyAny any_zero_valid{0, 0, 0, nullptr};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_zero_valid, 1024, "test", &err),
            0);

  // 6. 尺寸方程不匹配: float32 (type_id=1), count=2, 期望 8 bytes, 但给出 6
  // bytes -> -3
  CompanyAny any_mismatch{1, 2, 6, dummy};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_mismatch, 1024, "test", &err),
            -3);

  // 7. 正确尺寸方程: float32 (type_id=1), count=2, byte_length=8 -> 0
  CompanyAny any_valid{1, 2, 8, dummy};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_valid, 1024, "test", &err),
            0);

  // 8. 正字节数但空 data 指针 -> -3
  CompanyAny any_nulldata{1, 2, 8, nullptr};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_nulldata, 1024, "test", &err),
            -3);

  // 9. 超过最大字节上限 -> -3
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyAnyPayload(&any_valid, 4,
                                                                 "test", &err),
            -3);
}

// 4. CompanyString 嵌入 NUL、负长度与超限拦截
TEST(OperatorValueRegistryTest, BuiltinBufferAndAnyBindingsForwardValidation) {
  using namespace llm_edgeflow;
  const auto* buf_binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("buffer");
  ASSERT_NE(buf_binding, nullptr);
  ASSERT_TRUE(buf_binding->validate_external);

  InputLimits limits;
  std::string err;

  // CompanyBuffer：空指针
  EXPECT_EQ(buf_binding->validate_external(nullptr, limits, &err), -3);

  // CompanyBuffer：负长度
  uint8_t dummy_data[] = {0x01, 0x02, 0x03};
  CompanyBuffer buf_neg{-1, dummy_data};
  EXPECT_EQ(buf_binding->validate_external(&buf_neg, limits, &err), -3);

  // CompanyBuffer：length > max
  CompanyBuffer buf_toolarge{static_cast<int32_t>(limits.max_buffer_bytes + 1),
                             dummy_data};
  EXPECT_EQ(buf_binding->validate_external(&buf_toolarge, limits, &err), -3);

  // CompanyBuffer：length > 0 但 data 为空
  CompanyBuffer buf_nulldata{10, nullptr};
  EXPECT_EQ(buf_binding->validate_external(&buf_nulldata, limits, &err), -3);

  // CompanyBuffer：合法二进制数据
  CompanyBuffer buf_valid{3, dummy_data};
  EXPECT_EQ(buf_binding->validate_external(&buf_valid, limits, &err), 0);

  // CompanyAny
  const auto* any_binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("any");
  ASSERT_NE(any_binding, nullptr);
  ASSERT_TRUE(any_binding->validate_external);

  // CompanyAny：空指针
  EXPECT_EQ(any_binding->validate_external(nullptr, limits, &err), -3);

  // CompanyAny：负的 count / length
  CompanyAny any_neg{1, -1, 10, dummy_data};
  EXPECT_EQ(any_binding->validate_external(&any_neg, limits, &err), -3);

  // CompanyAny：byte_length > max
  CompanyAny any_toolarge{1, 10, static_cast<int32_t>(limits.max_any_bytes + 1),
                          dummy_data};
  EXPECT_EQ(any_binding->validate_external(&any_toolarge, limits, &err), -3);

  // 7. 正确尺寸方程: float32 (type_id=1), count=3, byte_length=12 -> 0
  CompanyAny any_valid{1, 3, 12, dummy_data};
  EXPECT_EQ(any_binding->validate_external(&any_valid, limits, &err), 0);
}

TEST(OperatorValueRegistryTest, CompanyStringValidation) {
  std::string err;

  CompanyString empty{0, nullptr};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&empty, 100,
                                                             "test", &err),
            0);
  CompanyString missing_data{1, nullptr};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&missing_data, 100,
                                                             "test", &err),
            -3);

  // 1. 空指针
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(nullptr, 100,
                                                             "test", &err),
            -3);

  // 2. 负长度
  char buf[] = "hello";
  CompanyString cs_neg{-1, buf};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&cs_neg, 100,
                                                             "test", &err),
            -3);

  // 3. 超限
  CompanyString cs_toolarge{10, buf};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&cs_toolarge, 5,
                                                             "test", &err),
            -3);

  // 4. 嵌入 NUL 字节拦截
  char embedded_nul[] = {'a', 'b', '\0', 'c'};
  CompanyString cs_nul{4, embedded_nul};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&cs_nul, 100,
                                                             "test", &err),
            -3);

  // 5. 正常字符串
  CompanyString cs_valid{5, buf};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyString(&cs_valid, 100,
                                                             "test", &err),
            0);
}

// 5. CompanyBuffer 二进制透明性与校验测试 (允许嵌入 NUL 字节)
TEST(OperatorValueRegistryTest, CompanyBufferValidation) {
  std::string err;

  // 1. 空指针 -> -3
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyBuffer(nullptr, 100,
                                                             "buf", &err),
            -3);

  // 2. 负长度 -> -3
  uint8_t raw[] = {0x01, 0x00, 0x02, 0xFF};
  CompanyBuffer cb_neg{-1, raw};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyBuffer(&cb_neg, 100,
                                                             "buf", &err),
            -3);

  // 3. 超限 -> -3
  CompanyBuffer cb_toolarge{10, raw};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyBuffer(&cb_toolarge, 3,
                                                             "buf", &err),
            -3);

  // 4. 包含嵌入 0x00 字节的二进制数据 (对于 Buffer 必须合法通过)
  CompanyBuffer cb_valid{4, raw};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyBuffer(&cb_valid, 100,
                                                             "buf", &err),
            0);

  // 5. 正长度但空数据指针 -> -3
  CompanyBuffer cb_nulldata{4, nullptr};
  EXPECT_EQ(OperatorValueTypeRegistry::ValidateCompanyBuffer(&cb_nulldata, 100,
                                                             "buf", &err),
            -3);
}

// 6. ValueTypeRegistry 原子预检与只读冻结测试 (R9-008)
TEST(OperatorValueRegistryTest,
     BindingSnapshotsSurviveRegistrationAndRegistry) {
  std::optional<OperatorValueTypeBinding> host;
  std::optional<OperatorValueTypeBinding> allocator;
  {
    OperatorValueTypeRegistry registry;
    host = registry.CopyBindingBySuffix("entity_in");
    auto output = registry.CopyOutputBinding("entity_out", "");
    ASSERT_TRUE(host);
    ASSERT_TRUE(output);
    ASSERT_TRUE(registry.RegisterOutputAllocator("first", *output));
    allocator = registry.CopyOutputBinding("entity_out", "first");
    ASSERT_TRUE(allocator);
    auto another = *host;
    another.canonical_suffix = "another_input";
    ASSERT_TRUE(registry.RegisterBinding(another));
    ASSERT_TRUE(registry.RegisterOutputAllocator("second", *output));
    EXPECT_FALSE(registry.CopyBindingBySuffix("missing"));
    EXPECT_FALSE(registry.CopyOutputBinding("entity_in", "first"));
  }
  CompanyOperatorEntityInput input{};
  input.request_id = 123;
  EXPECT_EQ(host->read_request_id(&input), 123u);
  EXPECT_EQ(allocator->allocation_name, "first");
  ResolvedOutputPoolSpec spec;
  spec.type = "entity_out";
  spec.capacities = {{"entities_json", 31}};
  OwnedExternalBlock block;
  std::string error;
  ASSERT_EQ(allocator->allocate_external(spec, &block, &error), 0) << error;
  ASSERT_NE(block.raw_struct, nullptr);
  allocator->reset_external(block.raw_struct, spec);
  allocator->destroy_external(&block);
  EXPECT_EQ(block.raw_struct, nullptr);
}

TEST(OperatorValueRegistryTest, RegisterBindingAtomicPrecheckAndFreeze) {
  auto& reg = OperatorValueTypeRegistry::Instance();
  EXPECT_EQ(reg.GlobalInit(), 0);

  // 尝试在 GlobalInit 之后晚注册 -> 必须拒绝
  OperatorValueTypeBinding late_binding;
  late_binding.canonical_suffix = "late_in";
  EXPECT_FALSE(reg.RegisterBinding(late_binding));
}

// 7. 内置宿主后缀均有注册的类型绑定
TEST(OperatorValueRegistryTest, BuiltinSuffixesHaveRegisteredBindings) {
  auto& reg = OperatorValueTypeRegistry::Instance();
  const std::vector<std::string> canonical_suffixes = {
      "string",     "buffer",      "any",       "frame",      "od_out",
      "keyword_in", "keyword_out", "entity_in", "entity_out", "doc_in",
      "doc_out",    "audit_in",    "audit_out", "audio_in",   "audio_out",
      "rerank_in",  "rerank_out"};
  for (const auto& suffix : canonical_suffixes) {
    const auto* binding = reg.GetBindingBySuffix(suffix);
    ASSERT_NE(binding, nullptr) << suffix;
    EXPECT_EQ(binding->canonical_suffix, suffix);
  }
}

// 8. ComputeOutputPoolPayloadBytes 预算计算与边界检测 (R9-005)
TEST(OperatorValueRegistryTest, AllSevenOutputTypesFootprintAndBudget) {
  const struct {
    const char* suffix;
    std::unordered_map<std::string, uint32_t> capacities;
  } cases[] = {
      {"doc_out", {{"intent_name", 63}, {"answer_text", 1023}}},
      {"keyword_out", {{"match_result_json", 2047}}},
      {"entity_out", {{"entities_json", 2047}}},
      {"audit_out",
       {{"risk_level", 31},
        {"matched_policy_clause", 255},
        {"audit_verdict_json", 1023}}},
      {"od_out", {{"result_json", 2047}}},
      {"audio_out", {{"transcribed_text", 511}, {"intent_slot_json", 1023}}},
      {"rerank_out", {}}};

  for (const auto& test : cases) {
    const std::string suffix(test.suffix);
    ResolvedOutputPoolSpec spec;
    spec.type = suffix;
    spec.capacities = test.capacities;
    if (suffix == "od_out") {
      spec.meta_num = 64;
      spec.metadata_type_id = 1;  // float32
    }

    // Depth 0 -> 归一化为 25
    size_t bytes_d0 = 0;
    std::string err;
    EXPECT_TRUE(
        ComputeOutputPoolPayloadBytes(suffix, spec, 0, &bytes_d0, &err));
    EXPECT_GT(bytes_d0, 0u);
    EXPECT_LT(bytes_d0, kMaxHandlePoolPayloadBytes);

    // 深度 1
    size_t bytes_d1 = 0;
    EXPECT_TRUE(
        ComputeOutputPoolPayloadBytes(suffix, spec, 1, &bytes_d1, &err));
    EXPECT_GT(bytes_d1, 0u);

    // 深度 25
    size_t bytes_d25 = 0;
    EXPECT_TRUE(
        ComputeOutputPoolPayloadBytes(suffix, spec, 25, &bytes_d25, &err));
    EXPECT_EQ(bytes_d0, bytes_d25);

    // Depth 1024 (上限)
    size_t bytes_d1024 = 0;
    EXPECT_TRUE(
        ComputeOutputPoolPayloadBytes(suffix, spec, 1024, &bytes_d1024, &err));
    EXPECT_GT(bytes_d1024, bytes_d25);

    // Depth 1025 (超限拦截)
    size_t bytes_overflow = 0;
    EXPECT_FALSE(ComputeOutputPoolPayloadBytes(suffix, spec, 1025,
                                               &bytes_overflow, &err));
    EXPECT_FALSE(err.empty());

    // spec.type 与 suffix 不匹配拦截
    ResolvedOutputPoolSpec mismatched_spec = spec;
    mismatched_spec.type = "completely_mismatched_suffix";
    size_t bytes_mismatch = 0;
    EXPECT_FALSE(ComputeOutputPoolPayloadBytes(suffix, mismatched_spec, 25,
                                               &bytes_mismatch, &err));
    EXPECT_FALSE(err.empty());
  }

  // 64 MiB 预算超限拦截测试
  {
    ResolvedOutputPoolSpec huge_spec;
    huge_spec.type = "keyword_out";
    huge_spec.capacities["match_result_json"] =
        100 * 1024 * 1024;  // 100 MiB 容量
    size_t huge_bytes = 0;
    std::string huge_err;
    EXPECT_FALSE(ComputeOutputPoolPayloadBytes("keyword_out", huge_spec, 25,
                                               &huge_bytes, &huge_err));
    EXPECT_FALSE(huge_err.empty());
  }

  // 未知 suffix -> 无 fallback 直接返回 false
  ResolvedOutputPoolSpec bad_spec;
  bad_spec.type = "unknown_out";
  size_t bad_bytes = 0;
  std::string bad_err;
  EXPECT_FALSE(ComputeOutputPoolPayloadBytes("unknown_out", bad_spec, 25,
                                             &bad_bytes, &bad_err));
  EXPECT_FALSE(bad_err.empty());
}

// 9. 独立 ValueTypeRegistry 实例与原子回滚测试
TEST(OperatorValueRegistryTest, IsolatedValueTypeRegistryAtomicRollback) {
  OperatorValueTypeRegistry local_reg;

  // 1. 重复 canonical 必须 fail-closed
  OperatorValueTypeBinding bad_b1;
  bad_b1.canonical_suffix = "string";
  EXPECT_FALSE(local_reg.RegisterBinding(bad_b1));
  EXPECT_TRUE(local_reg.HasConflict());
  EXPECT_EQ(local_reg.GlobalInit(), -6);

  // 2. 干净的本地 Registry 实例
  OperatorValueTypeRegistry clean_reg;
  EXPECT_EQ(clean_reg.GlobalInit(), 0);
  // 幂等多次 GlobalInit
  EXPECT_EQ(clean_reg.GlobalInit(), 0);
  EXPECT_FALSE(clean_reg.HasConflict());

  // 3. 晚注册被拒绝但不会破坏幂等 GlobalInit
  OperatorValueTypeBinding late_b;
  late_b.canonical_suffix = "late_slot";
  EXPECT_FALSE(clean_reg.RegisterBinding(late_b));
  EXPECT_EQ(clean_reg.GlobalInit(), 0);
  EXPECT_FALSE(clean_reg.HasConflict());
}

// 11. 缺少 Validator 或 Output Factory 时的 Fail-Closed 审计 (分项独立测试)
TEST(OperatorValueRegistryTest, MissingValidatorOrFactoryAuditRejection) {
  // 1. 输入类型缺少 validate_external
  {
    OperatorValueTypeRegistry reg;
    OperatorValueTypeBinding b;
    b.canonical_suffix = "custom_in";
    b.external_c_type_name = "CustomInput";
    b.direction = IoDirection::kInput;
    b.validate_external = nullptr;
    EXPECT_TRUE(reg.RegisterBinding(b));
    EXPECT_EQ(reg.GlobalInit(), -6);
    EXPECT_TRUE(reg.HasConflict());
  }

  // 2. 输出类型缺少 allocate_external
  {
    OperatorValueTypeRegistry reg;
    OperatorValueTypeBinding b;
    b.canonical_suffix = "custom_out1";
    b.external_c_type_name = "CustomOutput1";
    SetMinimalOutputContract(&b);
    b.allocate_external = nullptr;
    b.reset_external = [](void*, const ResolvedOutputPoolSpec&) {};
    b.destroy_external = [](OwnedExternalBlock*) {};
    EXPECT_TRUE(reg.RegisterBinding(b));
    EXPECT_EQ(reg.GlobalInit(), -6);
    EXPECT_TRUE(reg.HasConflict());
  }

  // 3. 输出类型缺少 reset_external
  {
    OperatorValueTypeRegistry reg;
    OperatorValueTypeBinding b;
    b.canonical_suffix = "custom_out2";
    b.external_c_type_name = "CustomOutput2";
    SetMinimalOutputContract(&b);
    b.allocate_external = [](const ResolvedOutputPoolSpec&, OwnedExternalBlock*,
                             std::string*) { return 0; };
    b.reset_external = nullptr;
    b.destroy_external = [](OwnedExternalBlock*) {};
    EXPECT_TRUE(reg.RegisterBinding(b));
    EXPECT_EQ(reg.GlobalInit(), -6);
    EXPECT_TRUE(reg.HasConflict());
  }

  // 4. 输出类型缺少 destroy_external
  {
    OperatorValueTypeRegistry reg;
    OperatorValueTypeBinding b;
    b.canonical_suffix = "custom_out3";
    b.external_c_type_name = "CustomOutput3";
    SetMinimalOutputContract(&b);
    b.allocate_external = [](const ResolvedOutputPoolSpec&, OwnedExternalBlock*,
                             std::string*) { return 0; };
    b.reset_external = [](void*, const ResolvedOutputPoolSpec&) {};
    b.destroy_external = nullptr;
    EXPECT_TRUE(reg.RegisterBinding(b));
    EXPECT_EQ(reg.GlobalInit(), -6);
    EXPECT_TRUE(reg.HasConflict());
  }

  // 5. 输出类型缺少预算回调
  {
    OperatorValueTypeRegistry reg;
    OperatorValueTypeBinding b;
    b.canonical_suffix = "custom_out4";
    b.external_c_type_name = "CustomOutput4";
    b.direction = IoDirection::kOutput;
    b.allocate_external = [](const ResolvedOutputPoolSpec&, OwnedExternalBlock*,
                             std::string*) { return 0; };
    b.reset_external = [](void*, const ResolvedOutputPoolSpec&) {};
    b.destroy_external = [](OwnedExternalBlock*) {};
    EXPECT_TRUE(reg.RegisterBinding(b));
    EXPECT_EQ(reg.GlobalInit(), -6);
    EXPECT_TRUE(reg.HasConflict());
  }

  // 6. 空 external_c_type_name 拒绝
  {
    OperatorValueTypeRegistry reg;
    OperatorValueTypeBinding b;
    b.canonical_suffix = "custom_out5";
    b.external_c_type_name = "";
    SetMinimalOutputContract(&b);
    b.allocate_external = [](const ResolvedOutputPoolSpec&, OwnedExternalBlock*,
                             std::string*) { return 0; };
    b.reset_external = [](void*, const ResolvedOutputPoolSpec&) {};
    b.destroy_external = [](OwnedExternalBlock*) {};
    EXPECT_FALSE(reg.RegisterBinding(b));
    EXPECT_TRUE(reg.HasConflict());
    EXPECT_EQ(reg.GlobalInit(), -6);
  }
}

// 真实分配失败时，注册必须可重试且旧 catalog 保持不变，
// 与内部使用何种容器无关。
TEST(OperatorValueRegistryTest,
     AllocationFailurePreservesRegistryAndAllowsRetry) {
  bool completed = false;
  for (int step = 0; step < 4096; ++step) {
    OperatorValueTypeRegistry reg;
    const auto* original = reg.GetBindingBySuffix("keyword_out");
    ASSERT_NE(original, nullptr);
    OperatorValueTypeBinding binding = *original;
    binding.canonical_suffix = "custom_retry_output";
    bool registered = false;
    bool injected = false;
    size_t outstanding = 0;
    bool overflowed = false;
    {
      test_support::ScopedAllocationFailure failure(step);
      registered = reg.RegisterBinding(binding);
      injected = failure.Triggered();
      outstanding = failure.Outstanding();
      overflowed = failure.Overflowed();
    }
    ASSERT_FALSE(overflowed);
    EXPECT_FALSE(reg.HasConflict());
    if (!injected) {
      ASSERT_TRUE(registered);
      ASSERT_NE(reg.GetBindingBySuffix(binding.canonical_suffix), nullptr);
      EXPECT_EQ(reg.GlobalInit(), 0);
      completed = true;
      break;
    }
    SCOPED_TRACE(step);
    EXPECT_FALSE(registered);
    EXPECT_EQ(outstanding, 0u);
    EXPECT_EQ(reg.GetBindingBySuffix("keyword_out"), original);
    EXPECT_EQ(reg.GetBindingBySuffix(binding.canonical_suffix), nullptr);
    ASSERT_TRUE(reg.RegisterBinding(binding));
    EXPECT_NE(reg.GetBindingBySuffix(binding.canonical_suffix), nullptr);
    EXPECT_EQ(reg.GlobalInit(), 0);
  }
  EXPECT_TRUE(completed)
      << "Allocation sweep never reached successful registration";
}

// 13. ComputeOutputPoolPayloadBytes 穷尽预算公式与边界测试 (R9-005)
TEST(OperatorValueRegistryTest,
     ComputeOutputPoolPayloadBytesExhaustiveBudgetSuite) {
  std::string err;
  size_t out_bytes = 0;

  // 1. 空指针
  EXPECT_FALSE(ComputeOutputPoolPayloadBytes("doc_out", {}, 10, nullptr, &err));

  // 2. 空 spec.type (必须严格拒绝)
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "";
    EXPECT_FALSE(
        ComputeOutputPoolPayloadBytes("doc_out", spec, 10, &out_bytes, &err));
    EXPECT_FALSE(err.empty());
  }

  // 3. spec.type 与 suffix 不匹配
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "od_out";
    EXPECT_FALSE(
        ComputeOutputPoolPayloadBytes("doc_out", spec, 10, &out_bytes, &err));
  }

  // 4. 深度 0 归一化为 25 且输出总大小在合理范围内
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "doc_out";
    spec.capacities["intent_name"] = 63;
    spec.capacities["answer_text"] = 1023;
    EXPECT_TRUE(
        ComputeOutputPoolPayloadBytes("doc_out", spec, 0, &out_bytes, &err));
    EXPECT_GT(out_bytes, 0u);
    EXPECT_LE(out_bytes, 64 * 1024 * 1024u);
  }

  // 5. 深度 1024 正常通过
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "keyword_out";
    spec.capacities["match_result_json"] = 2047;
    EXPECT_TRUE(ComputeOutputPoolPayloadBytes("keyword_out", spec, 1024,
                                              &out_bytes, &err));
    EXPECT_GT(out_bytes, 0u);
  }

  // 6. 深度 1025 超限拒绝
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "keyword_out";
    spec.capacities["match_result_json"] = 2047;
    EXPECT_FALSE(ComputeOutputPoolPayloadBytes("keyword_out", spec, 1025,
                                               &out_bytes, &err));
  }

  // 7. Schema 最大字符串容量下，64 MiB 总预算边界仍由统一预算器拦截。
  {
    constexpr uint32_t kMaxKeywordCapacity = 65536;
    constexpr size_t kBlockPayload = sizeof(CompanyOperatorKeywordOutput) +
                                     sizeof(CompanyString) +
                                     kMaxKeywordCapacity + 1;
    constexpr uint32_t kLargestAllowedDepth =
        static_cast<uint32_t>(kMaxHandlePoolPayloadBytes / kBlockPayload);
    static_assert(kLargestAllowedDepth > 0);
    static_assert(kLargestAllowedDepth < kMaxOutputPoolDepth);

    ResolvedOutputPoolSpec spec;
    spec.type = "keyword_out";
    spec.capacities["match_result_json"] = kMaxKeywordCapacity;
    EXPECT_TRUE(ComputeOutputPoolPayloadBytes(
        "keyword_out", spec, kLargestAllowedDepth, &out_bytes, &err));
    EXPECT_EQ(out_bytes, kBlockPayload * kLargestAllowedDepth);

    EXPECT_FALSE(ComputeOutputPoolPayloadBytes(
        "keyword_out", spec, kLargestAllowedDepth + 1, &out_bytes, &err));
    EXPECT_NE(err.find("64 MiB"), std::string::npos);

    spec.capacities["match_result_json"] = kMaxKeywordCapacity + 1;
    EXPECT_FALSE(ComputeOutputPoolPayloadBytes("keyword_out", spec, 1,
                                               &out_bytes, &err));
    EXPECT_NE(err.find("max hard limit"), std::string::npos);
  }

  // 8. checked arithmetic 与多池累加边界。
  {
    size_t checked = 0;
    EXPECT_FALSE(CheckedAdd(std::numeric_limits<size_t>::max(), 1, &checked));
    EXPECT_FALSE(
        CheckedMultiply(std::numeric_limits<size_t>::max(), 2, &checked));
    EXPECT_TRUE(CheckedAdd(kMaxHandlePoolPayloadBytes - 1, 1, &checked));
    EXPECT_EQ(checked, kMaxHandlePoolPayloadBytes);
    EXPECT_TRUE(CheckedAdd(kMaxHandlePoolPayloadBytes, 1, &checked));
    EXPECT_GT(checked, kMaxHandlePoolPayloadBytes);
  }

  // 9. 只有 od_out 镜像声明 metadata；其他输出不得接受未分配却未计费的配置。
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "doc_out";
    spec.capacities["intent_name"] = 63;
    spec.capacities["answer_text"] = 1023;
    spec.meta_num = 1;
    spec.metadata_type_id = 1;
    EXPECT_FALSE(
        ComputeOutputPoolPayloadBytes("doc_out", spec, 1, &out_bytes, &err));
  }
}

TEST(OperatorValueRegistryTest, OutputBindingOwnsDirectionAndCapacityLimits) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("doc_out");
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->direction, IoDirection::kOutput);
  ASSERT_EQ(binding->output_layout.string_capacity_fields.size(), 2u);

  ResolvedOutputPoolSpec requested;
  requested.type = "doc_out";
  requested.capacities["intent_name"] = 100;
  ResolvedOutputPoolSpec resolved;
  std::string err;
  EXPECT_FALSE(ResolveOutputPoolSpec(*binding, requested, &resolved, &err));
  EXPECT_EQ(err, "Missing output capacity field: answer_text");
  EXPECT_TRUE(resolved.capacities.empty());
  requested.capacities["answer_text"] = 1023;
  ASSERT_TRUE(ResolveOutputPoolSpec(*binding, requested, &resolved, &err))
      << err;
  EXPECT_EQ(resolved.GetCapacity("intent_name"), 100u);
  EXPECT_EQ(resolved.GetCapacity("answer_text"), 1023u);

  requested.capacities["unknown_field"] = 1;
  EXPECT_FALSE(ResolveOutputPoolSpec(*binding, requested, &resolved, &err));
  EXPECT_NE(err.find("Unknown capacity field"), std::string::npos);

  requested.capacities.erase("unknown_field");
  requested.capacities["answer_text"] = 65537;
  EXPECT_FALSE(ResolveOutputPoolSpec(*binding, requested, &resolved, &err));
  EXPECT_NE(err.find("max hard limit"), std::string::npos);
}

TEST(OperatorValueRegistryTest, SingleStringOutputCapacityContracts) {
  const struct {
    const char* suffix;
    const char* field;
    size_t root_bytes;
  } cases[] = {
      {"keyword_out", "match_result_json",
       sizeof(CompanyOperatorKeywordOutput)},
      {"entity_out", "entities_json", sizeof(CompanyOperatorEntityOutput)}};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.suffix);
    const auto* binding =
        OperatorValueTypeRegistry::Instance().GetBindingBySuffix(test.suffix);
    ASSERT_NE(binding, nullptr);
    ResolvedOutputPoolSpec requested;
    requested.type = test.suffix;
    ResolvedOutputPoolSpec resolved;
    std::string err;
    EXPECT_FALSE(ResolveOutputPoolSpec(*binding, requested, &resolved, &err));
    EXPECT_EQ(err, std::string("Missing output capacity field: ") + test.field);
    EXPECT_TRUE(resolved.capacities.empty());
    requested.capacities[test.field] = 2047;
    ASSERT_TRUE(ResolveOutputPoolSpec(*binding, requested, &resolved, &err));
    ASSERT_EQ(resolved.capacities.size(), 1u);
    EXPECT_EQ(resolved.GetCapacity(test.field), 2047u);
    for (uint32_t capacity : {100u, 65536u}) {
      requested.capacities[test.field] = capacity;
      ASSERT_TRUE(ResolveOutputPoolSpec(*binding, requested, &resolved, &err));
      EXPECT_EQ(resolved.GetCapacity(test.field), capacity);
      size_t bytes = 0;
      ASSERT_TRUE(
          ComputeOutputPoolPayloadBytes(*binding, requested, 1, &bytes, &err));
      EXPECT_EQ(bytes, test.root_bytes + sizeof(CompanyString) + capacity + 1);
    }
    for (uint32_t capacity : {0u, 65537u}) {
      requested.capacities[test.field] = capacity;
      EXPECT_FALSE(ResolveOutputPoolSpec(*binding, requested, &resolved, &err));
    }
  }
}

TEST(OperatorValueRegistryTest, DirectionDoesNotDependOnSuffixNaming) {
  OperatorValueTypeRegistry reg;
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = "opaque_payload";
  binding.external_c_type_name = "OpaquePayload";
  binding.direction = IoDirection::kInput;
  binding.validate_external = [](const void*, const InputLimits&,
                                 std::string*) { return 0; };

  ASSERT_TRUE(reg.RegisterBinding(binding));
  EXPECT_EQ(reg.GlobalInit(), 0);
  EXPECT_FALSE(reg.HasConflict());
}

TEST(OperatorValueRegistryTest, OutputBudgetCallbackFailsClosed) {
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = "throwing_output";
  binding.external_c_type_name = "ThrowingOutput";
  binding.direction = IoDirection::kOutput;
  binding.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec&, size_t*, std::string*) -> bool {
    throw std::runtime_error("injected budget failure");
  };

  ResolvedOutputPoolSpec spec;
  spec.type = binding.canonical_suffix;
  size_t out_bytes = 0;
  std::string err;
  EXPECT_FALSE(
      ComputeOutputPoolPayloadBytes(binding, spec, 1, &out_bytes, &err));
  EXPECT_EQ(out_bytes, 0u);
  EXPECT_NE(err.find("injected budget failure"), std::string::npos);

  binding.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec&, size_t* bytes, std::string*) noexcept {
        *bytes = 0;
        return true;
      };
  EXPECT_FALSE(
      ComputeOutputPoolPayloadBytes(binding, spec, 1, &out_bytes, &err));
  EXPECT_NE(err.find("payload is zero"), std::string::npos);
}

// 14. TSan 并发查询与冻结交错测试
TEST(OperatorValueRegistryTest, TSanConcurrentQueryAndFreeze) {
  OperatorValueTypeRegistry reg;
  std::atomic<bool> stop_flag{false};

  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&]() {
      while (!stop_flag.load()) {
        const auto* b = reg.GetBindingBySuffix("doc_out");
        EXPECT_NE(b, nullptr);
        EXPECT_EQ(reg.GetBindingBySuffix("unregistered_output"), nullptr);
      }
    });
  }

  std::thread freezer([&]() {
    for (int i = 0; i < 50; ++i) {
      EXPECT_EQ(reg.GlobalInit(), 0);
      OperatorValueTypeBinding late_b;
      late_b.canonical_suffix = "late_b";
      EXPECT_FALSE(reg.RegisterBinding(late_b));
    }
  });

  freezer.join();
  stop_flag.store(true);
  for (auto& r : readers) {
    r.join();
  }
  EXPECT_EQ(reg.GlobalInit(), 0);
  EXPECT_FALSE(reg.HasConflict());
}

// 15. noexcept 安全性与故障注入不抛出、不 terminate 测试
TEST(OperatorValueRegistryTest, NoexceptOOMFaultTolerance) {
  CompanyString str{-1, nullptr};
  CompanyBuffer buf{-1, nullptr};
  CompanyAny any{1, -1, 4, nullptr};
  for (int operation = 0; operation < 4; ++operation) {
    bool injected = false;
    for (int step = 0; step < 4; ++step) {
      std::string err;
      int result = 0;
      {
        test_support::ScopedAllocationFailure fail(step);
        switch (operation) {
          case 0:
            result = OperatorValueTypeRegistry::ValidateCompanyString(
                &str, 10, "test", &err);
            break;
          case 1:
            result = OperatorValueTypeRegistry::ValidateCompanyBuffer(
                &buf, 10, "test", &err);
            break;
          case 2:
            result = OperatorValueTypeRegistry::ValidateCompanyAnyPayload(
                &any, 10, "test", &err);
            break;
          case 3:
            result = CopyToOperatorString("source", nullptr, 1, "field", &err);
            break;
        }
        injected |= fail.Triggered();
      }
      EXPECT_EQ(result, operation == 3 ? -4 : -3);
    }
    EXPECT_TRUE(injected);
  }
}

// 16. audio_in 零长度时接受空缓冲区，拒绝负长度
TEST(OperatorValueRegistryTest,
     AudioInZeroLengthAcceptsNullBufferAndRejectsNegative) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("audio_in");
  ASSERT_NE(binding, nullptr);
  ASSERT_TRUE(binding->validate_external);

  InputLimits limits;
  limits.min_sample_rate = 8000;
  limits.max_sample_rate = 48000;
  limits.max_audio_pcm_samples = 160000;

  std::string err;

  // 情形 1：pcm_length == 0 且 pcm_buffer 为 nullptr -> 成功 (0)
  CompanyOperatorAudioInput zero_audio{};
  zero_audio.sample_rate = 16000;
  zero_audio.pcm_length = 0;
  zero_audio.pcm_buffer = nullptr;
  EXPECT_EQ(binding->validate_external(&zero_audio, limits, &err), 0) << err;

  // 情形 2：pcm_length < 0 -> 拒绝 (-3)
  CompanyOperatorAudioInput neg_audio{};
  neg_audio.sample_rate = 16000;
  neg_audio.pcm_length = -1;
  neg_audio.pcm_buffer = nullptr;
  err.clear();
  EXPECT_EQ(binding->validate_external(&neg_audio, limits, &err), -3);
  EXPECT_NE(err.find("invalid or exceeds limit"), std::string::npos);

  // 情形 3：pcm_length > 0 且 pcm_buffer 为 nullptr -> 拒绝 (-3)
  CompanyOperatorAudioInput null_buf_audio{};
  null_buf_audio.sample_rate = 16000;
  null_buf_audio.pcm_length = 100;
  null_buf_audio.pcm_buffer = nullptr;
  err.clear();
  EXPECT_EQ(binding->validate_external(&null_buf_audio, limits, &err), -3);
  EXPECT_EQ(err, "pcm_buffer pointer is null");

  // 情形 4：合法的 pcm_length 和缓冲区 -> 成功 (0)
  float dummy_pcm[100] = {0.0f};
  CompanyOperatorAudioInput valid_audio{};
  valid_audio.sample_rate = 16000;
  valid_audio.pcm_length = 100;
  valid_audio.pcm_buffer = dummy_pcm;
  err.clear();
  EXPECT_EQ(binding->validate_external(&valid_audio, limits, &err), 0);
}

TEST(OperatorValueRegistryTest, OperatorAgreesOnChannelNameBoundaries) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("audit_in");
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audit_in", "dialogue_audit");
  ASSERT_NE(binding, nullptr);
  ASSERT_NE(in_conv, nullptr);
  std::shared_ptr<const ParameterValues> parameters;
  std::string parameter_error;
  ASSERT_TRUE(in_conv->params.Parse(nlohmann::json::object(), &parameters,
                                    &parameter_error))
      << parameter_error;
  std::string query = "hello";
  CompanyString text{static_cast<int32_t>(query.size()), query.data()};
  for (int length : {-1, 0, 256, 257}) {
    std::string channel(length < 0 ? 0 : length, 'c');
    CompanyString named_channel{static_cast<int32_t>(channel.size()),
                                channel.data()};
    CompanyOperatorAuditInput op_input{7, kMockServiceDialogueAudit, &text,
                                       length < 0 ? nullptr : &named_channel};
    AlgContext ctx;
    const bool expected = length <= 256;
    ExternalInputBatchView view;
    view.slots["audit_in"] = BorrowInputForTest({&op_input});
    view.slot_types["audit_in"] = "CompanyOperatorAuditInput";
    view.count = 1;
    InputDecodeOptions options;
    const llm_edgeflow::IoPortBindings options_ports =
        llm_edgeflow::test::ConverterPortsForTest(*in_conv);
    options.ports = &options_ports;
    options.type = in_conv->type;
    options.name = in_conv->name;
    options.params = parameters.get();
    std::vector<uint64_t> request_ids;
    options.request_ids = &request_ids;
    int dec_ret = in_conv->decode_fn(view, options, &ctx, nullptr);
    EXPECT_EQ(dec_ret == 0, expected);
    EXPECT_EQ(binding->validate_external(&op_input, {}, nullptr) == 0,
              expected);
  }
}

TEST(OperatorValueRegistryTest, OperatorAgreesOnPcmBoundaries) {
  const auto* binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("audio_in");
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "audio_in", "audio_asr_intent");
  ASSERT_NE(binding, nullptr);
  ASSERT_NE(in_conv, nullptr);
  std::shared_ptr<const ParameterValues> parameters;
  std::string parameter_error;
  ASSERT_TRUE(in_conv->params.Parse(nlohmann::json::object(), &parameters,
                                    &parameter_error))
      << parameter_error;
  std::vector<float> samples(input_limits::kMaxAudioPcmSamples, 0);
  struct Case {
    int length;
    int rate;
    bool has_buffer;
    bool valid;
  };
  const Case cases[] = {
      {0, 16000, false, true},
      {0, 16000, true, true},
      {-1, 16000, false, false},
      {1, 16000, false, false},
      {1, 16000, true, true},
      {0, 7999, false, false},
      {0, 192001, false, false},
      {0, 8000, false, true},
      {0, 192000, false, true},
      {input_limits::kMaxAudioPcmSamples, 16000, true, true},
      {input_limits::kMaxAudioPcmSamples + 1, 16000, true, false}};
  for (const auto& test : cases) {
    CompanyOperatorAudioInput op_input{
        7, kMockServiceAudioAsrIntent,
        test.has_buffer ? samples.data() : nullptr, test.length, test.rate};
    AlgContext ctx;
    ExternalInputBatchView view;
    view.slots["audio_in"] = BorrowInputForTest({&op_input});
    view.slot_types["audio_in"] = "CompanyOperatorAudioInput";
    view.count = 1;
    InputDecodeOptions options;
    const llm_edgeflow::IoPortBindings options_ports =
        llm_edgeflow::test::ConverterPortsForTest(*in_conv);
    options.ports = &options_ports;
    options.type = in_conv->type;
    options.name = in_conv->name;
    options.params = parameters.get();
    std::vector<uint64_t> request_ids;
    options.request_ids = &request_ids;
    int dec_ret = in_conv->decode_fn(view, options, &ctx, nullptr);
    EXPECT_EQ(dec_ret == 0, test.valid);
    EXPECT_EQ(binding->validate_external(&op_input, {}, nullptr) == 0,
              test.valid);
  }
  InputLimits limits;
  limits.max_audio_pcm_bytes = sizeof(float);
  CompanyOperatorAudioInput input{7, kMockServiceAudioAsrIntent, samples.data(),
                                  2, 16000};
  EXPECT_NE(binding->validate_external(&input, limits, nullptr), 0);
}

TEST(OperatorValueRegistryTest,
     AuthoredInputForwardsTypedValueLimitsAndDiagnostics) {
  const auto binding = MakeTypedInputBinding<BusinessInput>(
      "test_business_input",
      [](const BusinessInput& input, const InputLimits& limits,
         std::string* error) -> int {
        if (input.text_bytes <= limits.max_text_bytes) return 0;
        if (error) *error = "business text exceeds configured limit";
        return -3;
      });
  EXPECT_EQ(binding.external_c_type_name, "BusinessInput");
  EXPECT_EQ(binding.direction, IoDirection::kInput);
  BusinessInput input{9};
  ExternalInputBatchView view;
  view.count = 1;
  view.slots["input"] = BorrowInputForTest({&input});
  view.slot_types["input"] = binding.external_c_type_name;
  const auto* decoded = view.GetSlot<BusinessInput>("input", 0);
  ASSERT_EQ(decoded, &input);
  view.slot_types["input"] = "DifferentInput";
  EXPECT_EQ(view.GetSlot<BusinessInput>("input", 0), nullptr);
  view.slot_types["input"] = binding.external_c_type_name;
  InputLimits limits;
  limits.max_text_bytes = 8;
  std::string error;
  EXPECT_EQ(binding.validate_external(decoded, limits, &error), -3);
  EXPECT_EQ(error, "business text exceeds configured limit");
  input.text_bytes = 8;
  EXPECT_EQ(binding.validate_external(&input, limits, nullptr), 0);
  EXPECT_EQ(binding.validate_external(nullptr, limits, &error), -3);
  EXPECT_EQ(error, "BusinessInput pointer is null");
}

}  // namespace llm_edgeflow
