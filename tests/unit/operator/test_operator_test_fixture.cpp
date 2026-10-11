#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include "tests/support/operator_test_fixture.h"

namespace llm_edgeflow {
using test_support::ScopedTestOperator;

namespace {
struct FakeOperatorState {
  int create_code = 0;
  int destroy_code = 0;
  int destroy_count = 0;
  uint32_t depth = 0;
};
thread_local FakeOperatorState fake_state;

operator_api::OperatorFunc FakeOperatorTable() {
  operator_api::OperatorFunc ops{};
  ops.Create = [](void** handle, const void* create_param) noexcept {
    const auto* param =
        static_cast<const operator_api::CreateParam*>(create_param);
    fake_state.depth = param->max_frame_depth;
    *handle = fake_state.create_code == 0 ? &fake_state : nullptr;
    return fake_state.create_code;
  };
  ops.Destroy = [](void*) noexcept {
    ++fake_state.destroy_count;
    return fake_state.destroy_code;
  };
  return ops;
}
}  // namespace

TEST(ScopedTestOperatorTest, FailedCreateDoesNotDestroy) {
  fake_state = {};
  fake_state.create_code = -2;
  {
    ScopedTestOperator instance(FakeOperatorTable());
    EXPECT_EQ(instance.Create("unused.conf"), -2);
    EXPECT_EQ(instance.get(), nullptr);
  }
  EXPECT_EQ(fake_state.destroy_count, 0);
}

TEST(ScopedTestOperatorTest, EarlyReturnCleansUpAndPreservesDefaultDepth) {
  fake_state = {};
  EXPECT_FATAL_FAILURE(([] {
                         ScopedTestOperator instance(FakeOperatorTable());
                         ASSERT_EQ(instance.Create("unused.conf"), 0);
                         EXPECT_EQ(fake_state.depth, 25u);
                         ASSERT_TRUE(false) << "intentional early return";
                       }()),
                       "intentional early return");
  EXPECT_EQ(fake_state.destroy_count, 1);
}

TEST(ScopedTestOperatorTest, FailedCloseConsumesHandleWithoutRetry) {
  fake_state = {};
  fake_state.destroy_code = -3;
  {
    ScopedTestOperator instance(FakeOperatorTable());
    ASSERT_EQ(instance.Create("unused.conf", ".",
                              operator_api::ComputePlatform::kCpu, 0),
              0);
    EXPECT_EQ(fake_state.depth, 0u);
    EXPECT_EQ(instance.Close(), -3);
    EXPECT_EQ(instance.get(), nullptr);
    EXPECT_EQ(instance.Close(), 0);
  }
  EXPECT_EQ(fake_state.destroy_count, 1);
}

TEST(ScopedTestOperatorTest, DestructorReportsCleanupFailure) {
  fake_state = {};
  fake_state.destroy_code = -3;
  EXPECT_NONFATAL_FAILURE(
      {
        ScopedTestOperator instance(FakeOperatorTable());
        (void)instance.Create("unused.conf");
      },
      "code");
  EXPECT_EQ(fake_state.destroy_count, 1);
}

}  // namespace llm_edgeflow
