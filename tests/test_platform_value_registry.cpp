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

}  // namespace alg_framework
