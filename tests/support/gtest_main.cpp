#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>

namespace {
class DeliveryGuard final : public ::testing::EmptyTestEventListener {
 public:
  void OnTestEnd(const ::testing::TestInfo& test) override {
    ++executed;
    if (!test.result()->Skipped()) return;
    for (int i = 0; i < test.result()->test_property_count(); ++i) {
      const auto& property = test.result()->GetTestProperty(i);
      if (std::strcmp(property.key(), "optional_runtime_asset") == 0 &&
          property.value()[0] != '\0') {
        ++optional_skips;
        return;
      }
    }
    std::fprintf(stderr, "Required test skipped: %s.%s\n",
                 test.test_suite_name(), test.name());
    required_skip = true;
  }

  int executed = 0;
  int optional_skips = 0;
  bool required_skip = false;
};
}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  // 无论继承了哪些标志，CTest 发现都必须看到全部已编译用例。
  if (GTEST_FLAG_GET(list_tests)) GTEST_FLAG_SET(filter, "*");
  auto* guard = new DeliveryGuard;
  ::testing::UnitTest::GetInstance()->listeners().Append(guard);
  const int result = RUN_ALL_TESTS();
  if (GTEST_FLAG_GET(list_tests)) return result;
  if (guard->executed == 0) {
    std::fprintf(stderr, "No tests executed; filter=%s, repeat=%d\n",
                 GTEST_FLAG_GET(filter).c_str(), GTEST_FLAG_GET(repeat));
    return 1;
  }
  if (guard->required_skip) return 1;
  if (result != 0) return result;
  return guard->optional_skips == guard->executed ? 77 : result;
}
