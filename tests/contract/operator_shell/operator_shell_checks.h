#pragma once

#include <gtest/gtest.h>

#include "tests/contract/operator_shell/operator_shell_driver.h"

namespace llm_edgeflow::operator_shell_test {

const char* CategoryName(Category category) noexcept;

// 清单中该类别的登记值；清单缺项时测试失败并返回 0。
int RegisteredCode(Category category);

// 实际返回值属于登记集合，且等于期望类别的登记值。
::testing::AssertionResult ReturnsCategory(int actual, Category expected);

// 以下场景被正式测试断言成功，被违约自测断言失败。

// 空输入与行数不配对均被拒绝，且不发布任何输出。
::testing::AssertionResult RejectedBatchesPublishNothing(
    OperatorShellDriver& driver, void* handle);

// 单行 Process 成功并清空输出后 Destroy 成功。*handle 无论结果都被消费。
::testing::AssertionResult DestroySucceedsAfterOutputsAreReleased(
    OperatorShellDriver& driver, void** handle);

}  // namespace llm_edgeflow::operator_shell_test
