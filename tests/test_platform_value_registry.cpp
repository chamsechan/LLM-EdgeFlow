#include <gtest/gtest.h>

#include "adapter/platform/platform_value_type_registry.h"

namespace alg_framework {

// 1. Any 类型白名单与尺寸查找
TEST(PlatformValueRegistryTest, CompanyAnyTypeWhitelistAndSizes) {
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
TEST(PlatformValueRegistryTest, CheckedMultiplySafety) {
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
TEST(PlatformValueRegistryTest, CompanyAnyValidationSuite) {
  std::string err;
  uint8_t dummy[64] = {0};

  // 1. 空指针 -> -3
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyAnyPayload(nullptr, 1024,
                                                                 "test", &err),
            -3);

  // 2. 负数字段 -> -3
  CompanyAny any_neg_cnt{1, -1, 4, dummy};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_neg_cnt, 1024, "test", &err),
            -3);

  CompanyAny any_neg_len{1, 1, -4, dummy};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_neg_len, 1024, "test", &err),
            -3);

  // 3. 未白名单 type_id -> -3
  CompanyAny any_unknown_type{999, 1, 4, dummy};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_unknown_type, 1024, "test", &err),
            -3);

  // 4. type_id 为 0 但 count/len 非零 -> -3
  CompanyAny any_zero_nonzero{0, 1, 4, dummy};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_zero_nonzero, 1024, "test", &err),
            -3);

  // 5. type_id 为 0 且 count/len 为零 -> 0 (合法无 metadata)
  CompanyAny any_zero_valid{0, 0, 0, nullptr};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_zero_valid, 1024, "test", &err),
            0);

  // 6. 尺寸方程不匹配: float32 (type_id=1), count=2, 期望 8 bytes, 但给出 6
  // bytes -> -3
  CompanyAny any_mismatch{1, 2, 6, dummy};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_mismatch, 1024, "test", &err),
            -3);

  // 7. 正确尺寸方程: float32 (type_id=1), count=2, byte_length=8 -> 0
  CompanyAny any_valid{1, 2, 8, dummy};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_valid, 1024, "test", &err),
            0);

  // 8. 正字节数但空 data 指针 -> -3
  CompanyAny any_nulldata{1, 2, 8, nullptr};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyAnyPayload(
                &any_nulldata, 1024, "test", &err),
            -3);

  // 9. 超过最大字节上限 -> -3
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyAnyPayload(&any_valid, 4,
                                                                 "test", &err),
            -3);
}

// 4. CompanyString 嵌入 NUL、负长度与超限拦截
TEST(PlatformValueRegistryTest, CompanyStringValidation) {
  std::string err;

  // 1. 空指针
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(nullptr, 100,
                                                             "test", &err),
            -3);

  // 2. 负长度
  char buf[] = "hello";
  CompanyString cs_neg{-1, buf};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(&cs_neg, 100,
                                                             "test", &err),
            -3);

  // 3. 超限
  CompanyString cs_toolarge{10, buf};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(&cs_toolarge, 5,
                                                             "test", &err),
            -3);

  // 4. 嵌入 NUL 字节拦截
  char embedded_nul[] = {'a', 'b', '\0', 'c'};
  CompanyString cs_nul{4, embedded_nul};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(&cs_nul, 100,
                                                             "test", &err),
            -3);

  // 5. 正常字符串
  CompanyString cs_valid{5, buf};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyString(&cs_valid, 100,
                                                             "test", &err),
            0);
}

// 5. CompanyBuffer 二进制透明性与校验测试 (允许嵌入 NUL 字节)
TEST(PlatformValueRegistryTest, CompanyBufferValidation) {
  std::string err;

  // 1. 空指针 -> -3
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyBuffer(nullptr, 100,
                                                             "buf", &err),
            -3);

  // 2. 负长度 -> -3
  uint8_t raw[] = {0x01, 0x00, 0x02, 0xFF};
  CompanyBuffer cb_neg{-1, raw};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyBuffer(&cb_neg, 100,
                                                             "buf", &err),
            -3);

  // 3. 超限 -> -3
  CompanyBuffer cb_toolarge{10, raw};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyBuffer(&cb_toolarge, 3,
                                                             "buf", &err),
            -3);

  // 4. 包含嵌入 0x00 字节的二进制数据 (对于 Buffer 必须合法通过)
  CompanyBuffer cb_valid{4, raw};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyBuffer(&cb_valid, 100,
                                                             "buf", &err),
            0);

  // 5. 正长度但空数据指针 -> -3
  CompanyBuffer cb_nulldata{4, nullptr};
  EXPECT_EQ(PlatformValueTypeRegistry::ValidateCompanyBuffer(&cb_nulldata, 100,
                                                             "buf", &err),
            -3);
}

// 6. ValueTypeRegistry 原子预检与只读冻结测试 (R9-008)
TEST(PlatformValueRegistryTest, RegisterBindingAtomicPrecheckAndFreeze) {
  auto& reg = PlatformValueTypeRegistry::Instance();
  EXPECT_EQ(reg.GlobalInit(), 0);

  // 尝试在 GlobalInit 之后晚注册 -> 必须拒绝
  PlatformValueTypeBinding late_binding;
  late_binding.canonical_suffix = "late_in";
  EXPECT_FALSE(reg.RegisterBinding(late_binding));
}

// 7. RFC 6.3 槽位类型与别名表完整性校验 (R9-011)
TEST(PlatformValueRegistryTest, RFC63AliasesCompliance) {
  auto& reg = PlatformValueTypeRegistry::Instance();

  // 1. string -> 没有别名
  EXPECT_EQ(reg.NormalizeSuffix("string"), "string");
  EXPECT_EQ(reg.NormalizeSuffix("text"), "");
  EXPECT_EQ(reg.NormalizeSuffix("str"), "");

  // 2. buffer -> 没有别名
  EXPECT_EQ(reg.NormalizeSuffix("buffer"), "buffer");
  EXPECT_EQ(reg.NormalizeSuffix("bin"), "");

  // 3. any -> 没有别名
  EXPECT_EQ(reg.NormalizeSuffix("any"), "any");
  EXPECT_EQ(reg.NormalizeSuffix("metadata"), "");

  // 4. frame -> aliases: {"image_in"}
  EXPECT_EQ(reg.NormalizeSuffix("frame"), "frame");
  EXPECT_EQ(reg.NormalizeSuffix("image_in"), "frame");
  EXPECT_EQ(reg.NormalizeSuffix("image"), "");

  // 5. od_out -> aliases: {"ocr_out"}
  EXPECT_EQ(reg.NormalizeSuffix("od_out"), "od_out");
  EXPECT_EQ(reg.NormalizeSuffix("ocr_out"), "od_out");

  // 6. keyword_in -> aliases: {"sentence_in"}
  EXPECT_EQ(reg.NormalizeSuffix("keyword_in"), "keyword_in");
  EXPECT_EQ(reg.NormalizeSuffix("sentence_in"), "keyword_in");

  // 7. keyword_out -> aliases: {"match_out"}
  EXPECT_EQ(reg.NormalizeSuffix("keyword_out"), "keyword_out");
  EXPECT_EQ(reg.NormalizeSuffix("match_out"), "keyword_out");

  // 8. entity_in -> aliases: {"text_in"}
  EXPECT_EQ(reg.NormalizeSuffix("entity_in"), "entity_in");
  EXPECT_EQ(reg.NormalizeSuffix("text_in"), "entity_in");

  // 9. entity_out -> aliases: {"extracted_out"}
  EXPECT_EQ(reg.NormalizeSuffix("entity_out"), "entity_out");
  EXPECT_EQ(reg.NormalizeSuffix("extracted_out"), "entity_out");

  // 10. doc_in -> aliases: {"qa_in"}
  EXPECT_EQ(reg.NormalizeSuffix("doc_in"), "doc_in");
  EXPECT_EQ(reg.NormalizeSuffix("qa_in"), "doc_in");

  // 11. doc_out -> aliases: {"qa_out"}
  EXPECT_EQ(reg.NormalizeSuffix("doc_out"), "doc_out");
  EXPECT_EQ(reg.NormalizeSuffix("qa_out"), "doc_out");

  // 12. audit_in -> aliases: {"dialogue_in"}
  EXPECT_EQ(reg.NormalizeSuffix("audit_in"), "audit_in");
  EXPECT_EQ(reg.NormalizeSuffix("dialogue_in"), "audit_in");

  // 13. audit_out -> aliases: {"verdict_out"}
  EXPECT_EQ(reg.NormalizeSuffix("audit_out"), "audit_out");
  EXPECT_EQ(reg.NormalizeSuffix("verdict_out"), "audit_out");

  // 14. audio_in -> aliases: {"pcm_stream"}
  EXPECT_EQ(reg.NormalizeSuffix("audio_in"), "audio_in");
  EXPECT_EQ(reg.NormalizeSuffix("pcm_stream"), "audio_in");

  // 15. audio_out -> aliases: {"asr_out"}
  EXPECT_EQ(reg.NormalizeSuffix("audio_out"), "audio_out");
  EXPECT_EQ(reg.NormalizeSuffix("asr_out"), "audio_out");

  // 16. rerank_in -> aliases: {"pair_in"}
  EXPECT_EQ(reg.NormalizeSuffix("rerank_in"), "rerank_in");
  EXPECT_EQ(reg.NormalizeSuffix("pair_in"), "rerank_in");

  // 17. rerank_out -> aliases: {"scores_out"}
  EXPECT_EQ(reg.NormalizeSuffix("rerank_out"), "rerank_out");
  EXPECT_EQ(reg.NormalizeSuffix("scores_out"), "rerank_out");
}

// 8. ComputeOutputPoolBytes 预算计算与边界检测 (R9-005)
TEST(PlatformValueRegistryTest, ComputeOutputPoolBytesBudgeting) {
  ResolvedOutputPoolSpec spec;
  spec.type = "od_out";
  spec.meta_num = 100;
  spec.metadata_type_id = 1;  // float32 (400 bytes)
  spec.capacities["result_json"] = 1024;

  size_t bytes = 0;
  std::string err;
  EXPECT_TRUE(ComputeOutputPoolBytes("od_out", spec, 25, &bytes, &err));
  EXPECT_GT(bytes, 0u);
  EXPECT_LT(bytes, kMaxHandlePoolMemoryBytes);

  // 深度超限
  EXPECT_FALSE(ComputeOutputPoolBytes("od_out", spec, 1025, &bytes, &err));
  EXPECT_FALSE(err.empty());
}

}  // namespace alg_framework
