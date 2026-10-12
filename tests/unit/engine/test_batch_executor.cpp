#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "engine/fixed_batch_executor.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {
namespace {

using Items = std::vector<TraceableItem<int>>;
const Items kInputs{{7, 2, 10}, {7, 9, 20}, {1, 4, 30}, {9, 3, 40}};

TEST(FixedBatchExecutorTest, SlicesFixedAndVariableBatchesAndRemovesPadding) {
  for (const size_t fixed : {3U, 0U}) {
    SCOPED_TRACE(fixed);
    std::vector<std::tuple<size_t, size_t, size_t>> slices;
    Items outputs{{0, 0, -1}};
    std::string diagnostic = "prior error";
    const int code = FixedBatchExecutor::Execute<int, int>(
        kInputs, {3, fixed},
        [&](const BatchSlice& slice, std::vector<int>* batch) {
          slices.emplace_back(slice.offset, slice.valid_count,
                              slice.execution_count);
          *batch = slice.offset == 0 ? std::vector<int>{101, 102, 103}
                                     : std::vector<int>{104};
          if (fixed && slice.offset == 3)
            batch->insert(batch->end(), {999, 998});
          return 0;
        },
        &outputs, &diagnostic);
    ASSERT_EQ(code, 0);
    EXPECT_TRUE(diagnostic.empty());
    EXPECT_EQ(slices, (std::vector<std::tuple<size_t, size_t, size_t>>{
                          {0, 3, 3}, {3, 1, fixed ? 3U : 1U}}));
    ASSERT_EQ(outputs.size(), 4U);
    const Items expected{{7, 2, 101}, {7, 9, 102}, {1, 4, 103}, {9, 3, 104}};
    for (size_t i = 0; i < outputs.size(); ++i) {
      EXPECT_EQ(outputs[i].req_id, expected[i].req_id);
      EXPECT_EQ(outputs[i].sub_id, expected[i].sub_id);
      EXPECT_EQ(outputs[i].data, expected[i].data);
    }
  }
}

TEST(FixedBatchExecutorTest, ValidatesPolicyBeforeCallingBackend) {
  int calls = 0;
  const auto run = [&](const BatchSlice&, std::vector<int>*) {
    ++calls;
    return 0;
  };
  std::string diagnostic;
  EXPECT_EQ((FixedBatchExecutor::Execute<int, int>(kInputs, {3, 0}, run,
                                                   nullptr, &diagnostic)),
            -1);
  for (const BatchPolicy policy : {BatchPolicy{0, 0}, BatchPolicy{3, 2}}) {
    Items outputs{{0, 0, -1}};
    EXPECT_EQ((FixedBatchExecutor::Execute<int, int>(kInputs, policy, run,
                                                     &outputs, &diagnostic)),
              -2);
    EXPECT_TRUE(outputs.empty());
    EXPECT_FALSE(diagnostic.empty());
  }
  Items outputs{{0, 0, -1}};
  EXPECT_EQ((FixedBatchExecutor::Execute<int, int>({}, {0, 5}, run, &outputs,
                                                   &diagnostic)),
            0);
  EXPECT_TRUE(outputs.empty());
  EXPECT_TRUE(diagnostic.empty());
  EXPECT_EQ(calls, 0);
}

TEST(FixedBatchExecutorTest, AnyLaterFailureRollsBackTheSuccessfulPrefix) {
  enum class Failure { kCount, kCode, kDetail, kStandard, kUnknown };
  for (const Failure failure :
       {Failure::kCount, Failure::kCode, Failure::kDetail, Failure::kStandard,
        Failure::kUnknown}) {
    SCOPED_TRACE(static_cast<int>(failure));
    Items outputs;
    std::string diagnostic;
    int calls = 0;
    const int code = FixedBatchExecutor::Execute<int, int>(
        kInputs, {3, 3},
        [&](const BatchSlice& slice, std::vector<int>* batch) {
          ++calls;
          if (slice.offset == 0) {
            *batch = {1, 2, 3};
            return 0;
          }
          switch (failure) {
            case Failure::kCount:
              *batch = {4};
              return 0;
            case Failure::kCode:
              return -37;
            case Failure::kDetail:
              diagnostic = "device refused row";
              return -37;
            case Failure::kStandard:
              throw std::runtime_error("backend exception");
            case Failure::kUnknown:
              throw 17;
          }
          return 0;
        },
        &outputs, &diagnostic);
    EXPECT_EQ(code, failure == Failure::kCount ? -3
                    : failure == Failure::kCode || failure == Failure::kDetail
                        ? -37
                        : -4);
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(outputs.empty());
    EXPECT_FALSE(diagnostic.empty());
    if (failure == Failure::kDetail) {
      EXPECT_EQ(diagnostic, "device refused row");
    }
    if (failure == Failure::kCode) {
      EXPECT_EQ(diagnostic, "Batch execution failed");
    }
    if (failure == Failure::kStandard) {
      EXPECT_EQ(diagnostic, "backend exception");
    }
  }
}

TEST(FixedBatchExecutorTest, ItemAdapterPreservesValuesAndCustomExceptionCode) {
  Items outputs;
  std::vector<std::tuple<uint32_t, uint32_t, int>> observed;
  const auto run = [&](const TraceableItem<int>& item, int* output) {
    observed.emplace_back(item.req_id, item.sub_id, item.data);
    *output = item.data + 1;
    return 0;
  };
  ASSERT_EQ((FixedBatchExecutor::ExecuteItems<int, int>(kInputs, {3, 0}, run,
                                                        &outputs)),
            0);
  EXPECT_EQ(observed, (std::vector<std::tuple<uint32_t, uint32_t, int>>{
                          {7, 2, 10}, {7, 9, 20}, {1, 4, 30}, {9, 3, 40}}));
  ASSERT_EQ(outputs.size(), 4U);
  EXPECT_EQ(outputs.back().data, 41);
  EXPECT_EQ(outputs.back().req_id, 9U);
  EXPECT_EQ(outputs.back().sub_id, 3U);
  observed.clear();
  EXPECT_EQ((FixedBatchExecutor::ExecuteItems<int, int>(kInputs, {3, 3}, run,
                                                        &outputs)),
            -2);
  EXPECT_TRUE(outputs.empty());
  EXPECT_TRUE(observed.empty());
  std::string diagnostic;
  const int code = FixedBatchExecutor::ExecuteItems<int, int>(
      kInputs, {3, 0},
      [](const TraceableItem<int>& item, int* output) {
        if (item.data == 40) throw std::runtime_error("item failure");
        *output = item.data;
        return 0;
      },
      &outputs, &diagnostic, -71);
  EXPECT_EQ(code, -71);
  EXPECT_EQ(diagnostic, "item failure");
  EXPECT_TRUE(outputs.empty());
}

TEST(FixedBatchExecutorTest, DynamicTailRejectsExtraResultsAndRollsBackPrefix) {
  Items outputs;
  int calls = 0;
  std::string diagnostic;
  const int code = FixedBatchExecutor::Execute<int, int>(
      kInputs, {3, 0},
      [&](const BatchSlice& slice, std::vector<int>* batch) {
        ++calls;
        *batch = slice.offset == 0 ? std::vector<int>{1, 2, 3}
                                   : std::vector<int>{4, 5};
        return 0;
      },
      &outputs, &diagnostic);
  EXPECT_EQ(code, -3);
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(diagnostic, "Batch output count mismatch");
}

TEST(FixedBatchExecutorTest, OutputAllocationFailureIsContained) {
  Items outputs;
  std::string diagnostic;
  int calls = 0, code;
  bool injected;
  {
    test_support::ScopedAllocationFailure fault(0);
    code = FixedBatchExecutor::Execute<int, int>(
        kInputs, {3, 0},
        [&](const BatchSlice&, std::vector<int>*) {
          ++calls;
          return 0;
        },
        &outputs, &diagnostic);
    injected = fault.Triggered();
  }
  EXPECT_TRUE(injected);
  EXPECT_EQ(code, -5);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(outputs.empty());
  EXPECT_FALSE(diagnostic.empty());
}

}  // namespace
}  // namespace llm_edgeflow
