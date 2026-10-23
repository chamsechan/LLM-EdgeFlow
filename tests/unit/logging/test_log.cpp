#include <gtest/gtest.h>

#include <limits>

#include "edgeflow/log.h"

namespace {
class MockLogLevelTest : public ::testing::Test {
 protected:
  void SetUp() override { saved_ = AlgBase_getLogLevelByName(nullptr); }
  void TearDown() override { AlgBase_setLogLevelByName(nullptr, saved_); }
  int saved_ = 0;
};

TEST_F(MockLogLevelTest, SetterReturnsSuccessAndGetterReturnsTheLevel) {
  for (int level = E_ALG_BASE_LOG_LEVEL_SILENT;
       level <= E_ALG_BASE_LOG_LEVEL_VERBOSE; ++level) {
    SCOPED_TRACE(level);
    EXPECT_EQ(AlgBase_setLogLevelByName("host", level), 0);
    EXPECT_EQ(AlgBase_getLogLevelByName("host"), level);
  }
}

TEST_F(MockLogLevelTest, InvalidLevelReturnsFailureAndPreservesTheLevel) {
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_WARNING),
            0);
  for (int level : {std::numeric_limits<int>::min(), -1, 7,
                    std::numeric_limits<int>::max()}) {
    SCOPED_TRACE(level);
    EXPECT_EQ(AlgBase_setLogLevelByName(nullptr, level), -1);
    EXPECT_EQ(AlgBase_getLogLevelByName(nullptr), E_ALG_BASE_LOG_LEVEL_WARNING);
  }
}
}  // namespace
