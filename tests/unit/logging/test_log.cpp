#include <gtest/gtest.h>

#include <string>

#include "contracts/error_codes.h"
#include "edgeflow/log.h"

namespace {
class LoggingRecordTest : public ::testing::Test {
 protected:
  void SetUp() override { saved = AlgBase_getLogLevelByName(nullptr); }
  void TearDown() override { AlgBase_setLogLevelByName(nullptr, saved); }
  int saved = 0;
};

TEST_F(LoggingRecordTest,
       ThresholdSuppressesFormattingArgumentsAndInvalidLevelsPreserveIt) {
  ASSERT_EQ(AlgBase_setLogLevelByName("host", E_ALG_BASE_LOG_LEVEL_WARNING), 0);
  int evaluations = 0;
  testing::internal::CaptureStderr();
  ALG_LOG_INFO("hidden %d", ++evaluations);
  ALG_LOG_WARNING("visible %d", ++evaluations);
  const auto record = testing::internal::GetCapturedStderr();
  EXPECT_EQ(evaluations, 1);
  EXPECT_EQ(record, "[Warning][LLM_EDGEFLOW] visible 1");
  EXPECT_EQ(AlgBase_setLogLevelByName("other", -1),
            llm_edgeflow::error::kInvalidArgument);
  EXPECT_EQ(AlgBase_setLogLevelByName("other", 6),
            llm_edgeflow::error::kInvalidArgument);
  EXPECT_EQ(AlgBase_getLogLevelByName("other"), E_ALG_BASE_LOG_LEVEL_WARNING);
}

TEST_F(LoggingRecordTest,
       DirectRecordUsesNullFallbacksAndFatalFlushDoesNotTerminate) {
  testing::internal::CaptureStderr();
  AlgBase_logPrint(nullptr, nullptr, "literal %s:%d", "中文", 7);
  AlgBase_logPrint("Fatal", "", ";last");
  AlgBase_logPrint("Info", "ignored", nullptr);
  const auto records = testing::internal::GetCapturedStderr();
  EXPECT_EQ(
      records,
      "[Unknown][LLM_EDGEFLOW] literal 中文:7[Fatal][LLM_EDGEFLOW] ;last");
}
}  // namespace
