#include "tests/contract/operator_shell/operator_shell_checks.h"

#include <string>
#include <utility>

#include "edgeflow/operator/interface.h"

namespace llm_edgeflow::operator_shell_test {
namespace {

const ErrorCodeManifestEntry* Find(Category category) {
  for (const auto& entry : ErrorCodeManifest()) {
    if (entry.category == category) return &entry;
  }
  return nullptr;
}

}  // namespace

const char* CategoryName(Category category) noexcept {
  switch (category) {
    case Category::kSuccess:
      return "success";
    case Category::kInvalidHandle:
      return "invalid handle";
    case Category::kInvalidParam:
      return "invalid param";
    case Category::kInvalidInput:
      return "invalid input";
    case Category::kOutputCapacity:
      return "output capacity";
    case Category::kUnsupportedBiz:
      return "unsupported biz";
    case Category::kRegistryConflict:
      return "registry conflict";
    case Category::kUnsupportedControl:
      return "unsupported control";
    case Category::kException:
      return "exception";
    case Category::kUnknown:
      break;
  }
  return "unknown";
}

int RegisteredCode(Category category) {
  const auto* entry = Find(category);
  EXPECT_NE(entry, nullptr)
      << "Manifest has no entry for " << CategoryName(category);
  return entry ? entry->value : 0;
}

::testing::AssertionResult ReturnsCategory(int actual, Category expected) {
  bool registered = false;
  for (const auto& entry : ErrorCodeManifest()) {
    registered = registered || entry.value == actual;
  }
  if (!registered) {
    return ::testing::AssertionFailure()
           << "returned " << actual
           << ", which is not a registered public code (expected "
           << CategoryName(expected)
           << "); last error: " << operator_api::GetOperatorLastError();
  }
  const auto* entry = Find(expected);
  if (!entry) {
    return ::testing::AssertionFailure()
           << "manifest has no entry for " << CategoryName(expected);
  }
  if (entry->value != actual) {
    return ::testing::AssertionFailure()
           << "returned " << actual << ", expected " << CategoryName(expected)
           << " (" << entry->symbol << " = " << entry->value
           << "); last error: " << operator_api::GetOperatorLastError();
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult RejectedBatchesPublishNothing(
    OperatorShellDriver& driver, void* handle) {
  const std::pair<const char*, ProcessRequest> rejected[] = {
      {"empty inputs", {{}, 1}},
      {"unpaired rows", {{"初始化", "初始化"}, 1}},
  };
  for (const auto& [name, request] : rejected) {
    auto status = ReturnsCategory(driver.Process(handle, request),
                                  Category::kInvalidInput);
    if (!status) return status << " [" << name << "]";
    if (driver.PublishedOutputs() != 0) {
      return ::testing::AssertionFailure()
             << name << " published " << driver.PublishedOutputs()
             << " output(s) after a failed Process";
    }
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult DestroySucceedsAfterOutputsAreReleased(
    OperatorShellDriver& driver, void** handle) {
  auto processed = ReturnsCategory(driver.Process(*handle, {{"初始化"}, 1}),
                                   Category::kSuccess);
  driver.ReleaseOutputs();
  void* consumed = std::exchange(*handle, nullptr);
  auto destroyed =
      ReturnsCategory(driver.Destroy(consumed), Category::kSuccess);
  if (!processed) return processed << " [Process]";
  if (!destroyed) return destroyed << " [Destroy]";
  return ::testing::AssertionSuccess();
}

}  // namespace llm_edgeflow::operator_shell_test
