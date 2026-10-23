#define EDGEFLOW_LOG_NAME "HOST_COMPONENT"
#include <gtest/gtest.h>

#include "edgeflow/log.h"

TEST(LoggingNameOverrideTest,
     MacroUsesTranslationUnitNameWithExternalLoggerLinkage) {
  const int saved = AlgBase_getLogLevelByName(nullptr);
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_DEBUG), 0);
  testing::internal::CaptureStderr();
  ALG_LOG_DEBUG("record=%d", 42);
  const auto record = testing::internal::GetCapturedStderr();
  EXPECT_EQ(AlgBase_setLogLevelByName(nullptr, saved), 0);
  EXPECT_EQ(record, "[Debug][HOST_COMPONENT] record=42");
}
