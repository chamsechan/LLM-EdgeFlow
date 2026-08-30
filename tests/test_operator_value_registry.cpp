#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <thread>

#include "adapter/operator/operator_value_type_registry.h"

namespace alg_framework {

namespace {

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
TEST(OperatorValueRegistryTest, CompanyStringValidation) {
  std::string err;

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
TEST(OperatorValueRegistryTest, RegisterBindingAtomicPrecheckAndFreeze) {
  auto& reg = OperatorValueTypeRegistry::Instance();
  EXPECT_EQ(reg.GlobalInit(), 0);

  // 尝试在 GlobalInit 之后晚注册 -> 必须拒绝
  OperatorValueTypeBinding late_binding;
  late_binding.canonical_suffix = "late_in";
  EXPECT_FALSE(reg.RegisterBinding(late_binding));
}

// 7. Operator 槽位只接受唯一规范后缀
TEST(OperatorValueRegistryTest, CanonicalSuffixesOnly) {
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

  for (const auto* noncanonical :
       {"image_in", "ocr_out", "sentence_in", "match_out", "text_in",
        "extracted_out", "qa_in", "qa_out", "dialogue_in", "verdict_out",
        "pcm_stream", "asr_out", "pair_in", "scores_out"}) {
    EXPECT_EQ(reg.GetBindingBySuffix(noncanonical), nullptr) << noncanonical;
  }
}

// 8. ComputeOutputPoolPayloadBytes 预算计算与边界检测 (R9-005)
TEST(OperatorValueRegistryTest, AllSevenOutputTypesFootprintAndBudget) {
  const std::vector<std::string> output_suffixes = {
      "doc_out", "keyword_out", "entity_out", "audit_out",
      "od_out",  "audio_out",   "rerank_out"};

  for (const auto& suffix : output_suffixes) {
    ResolvedOutputPoolSpec spec;
    spec.type = suffix;
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

    // Depth 1
    size_t bytes_d1 = 0;
    EXPECT_TRUE(
        ComputeOutputPoolPayloadBytes(suffix, spec, 1, &bytes_d1, &err));
    EXPECT_GT(bytes_d1, 0u);

    // Depth 25
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
        100 * 1024 * 1024;  // 100 MiB capacity
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
    b.external_c_type_name = "";  // empty
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

// 12. 命名异常注入与 Copy-and-Swap 事务回滚零污染测试 (R9-008, R9-010)
TEST(OperatorValueRegistryTest, NamedExceptionInjectionRollbackZeroCorruption) {
  const auto points = {
      OperatorValueTypeRegistry::RegistryExceptionInjectPoint::
          kCopyCanonicalMap,
      OperatorValueTypeRegistry::RegistryExceptionInjectPoint::kCanonicalInsert,
      OperatorValueTypeRegistry::RegistryExceptionInjectPoint::kPublish,
  };

  for (auto pt : points) {
    OperatorValueTypeRegistry reg;
    EXPECT_FALSE(reg.HasConflict());

    OperatorValueTypeBinding b;
    b.canonical_suffix = "injected_custom_out";
    b.external_c_type_name = "InjectedCustomOutput";
    SetMinimalOutputContract(&b);
    b.allocate_external = [](const ResolvedOutputPoolSpec&, OwnedExternalBlock*,
                             std::string*) { return 0; };
    b.reset_external = [](void*, const ResolvedOutputPoolSpec&) {};
    b.destroy_external = [](OwnedExternalBlock*) {};

    OperatorValueTypeRegistry::SetExceptionInjectPoint(pt);
    EXPECT_FALSE(reg.RegisterBinding(b));
    OperatorValueTypeRegistry::SetExceptionInjectPoint(
        OperatorValueTypeRegistry::RegistryExceptionInjectPoint::kNone);

    // 状态无污染
    EXPECT_FALSE(reg.HasConflict());
    EXPECT_EQ(reg.GetBindingBySuffix("injected_custom_out"), nullptr);
    EXPECT_EQ(reg.GlobalInit(), 0);
  }
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
    EXPECT_TRUE(
        ComputeOutputPoolPayloadBytes("doc_out", spec, 0, &out_bytes, &err));
    EXPECT_GT(out_bytes, 0u);
    EXPECT_LE(out_bytes, 64 * 1024 * 1024u);
  }

  // 5. 深度 1024 正常通过
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "keyword_out";
    EXPECT_TRUE(ComputeOutputPoolPayloadBytes("keyword_out", spec, 1024,
                                              &out_bytes, &err));
    EXPECT_GT(out_bytes, 0u);
  }

  // 6. 深度 1025 超限拒绝
  {
    ResolvedOutputPoolSpec spec;
    spec.type = "keyword_out";
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
    spec.meta_num = 1;
    spec.metadata_type_id = 1;
    EXPECT_FALSE(
        ComputeOutputPoolPayloadBytes("doc_out", spec, 1, &out_bytes, &err));
  }
}

TEST(OperatorValueRegistryTest,
     OutputBindingOwnsDirectionDefaultsAndCapacityLimits) {
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

TEST(OperatorValueRegistryTest, DirectionDoesNotDependOnSuffixNaming) {
  OperatorValueTypeRegistry reg;
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = "opaque_payload";
  binding.external_c_type_name = "OpaquePayload";
  binding.direction = IoDirection::kInput;
  binding.validate_external = [](const void*, const ResolvedInputLimits&,
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
        EXPECT_EQ(reg.GetBindingBySuffix("qa_out"), nullptr);
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

}  // namespace alg_framework
