#include <gtest/gtest.h>

#include <set>
#include <string>

#include "contracts/error_codes.h"
#include "tests/contract/operator_shell/operator_shell_checks.h"

namespace llm_edgeflow::operator_shell_test {
namespace {

TEST(OperatorShellErrorCodeManifestTest,
     EachCategoryHasOneEntryMatchingTheShellMapping) {
  const auto driver = MakeOperatorShellDriver();
  const auto& manifest = ErrorCodeManifest();
  EXPECT_EQ(manifest.size(), std::size(kAllCategories));
  for (const auto category : kAllCategories) {
    SCOPED_TRACE(CategoryName(category));
    size_t entries = 0;
    for (const auto& entry : manifest) {
      if (entry.category != category) continue;
      ++entries;
      EXPECT_NE(std::string(entry.symbol), "");
      EXPECT_NE(std::string(entry.basis), "");
      EXPECT_EQ(driver->PlatformCode(category), entry.value) << entry.symbol;
    }
    EXPECT_EQ(entries, 1u);
  }
}

TEST(OperatorShellErrorCodeManifestTest,
     PublicCodesAreDisjointFromDeclaredInternalFailures) {
  std::set<int> internal;
#define LLM_EDGEFLOW_COLLECT_FAILURE(name, value, text) \
  if (value != error::kOk) internal.insert(value);
  LLM_EDGEFLOW_ERROR_CODES(LLM_EDGEFLOW_COLLECT_FAILURE)
#undef LLM_EDGEFLOW_COLLECT_FAILURE
  ASSERT_FALSE(internal.empty());
  for (const auto& entry : ErrorCodeManifest()) {
    SCOPED_TRACE(entry.symbol);
    if (entry.category == Category::kSuccess) {
      EXPECT_EQ(entry.value, 0);
      continue;
    }
    EXPECT_NE(entry.value, 0);
    EXPECT_EQ(internal.count(entry.value), 0u)
        << "public code collides with a declared internal failure code";
  }
}

}  // namespace
}  // namespace llm_edgeflow::operator_shell_test
