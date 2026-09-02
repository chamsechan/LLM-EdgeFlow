#include <gtest/gtest.h>

#include <string>

#define COMPANY_ALG_LOG_NAME "EDGEFLOW_OVERRIDE"
#include "company_alg_log.h"

TEST(CompanyAlgLogNameOverrideTest, PublicMacroUsesCompileTimeNameOverride) {
  const int saved_level = AlgBase_getLogLevelByName(COMPANY_ALG_LOG_NAME);
  ASSERT_EQ(AlgBase_setLogLevelByName(COMPANY_ALG_LOG_NAME,
                                      E_ALG_BASE_LOG_LEVEL_INFO),
            0);

  testing::internal::CaptureStderr();
  ALG_LOG_INFO("override\n");
  const std::string output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(output, "[Info][EDGEFLOW_OVERRIDE] override\n");
  EXPECT_EQ(AlgBase_setLogLevelByName(COMPANY_ALG_LOG_NAME, saved_level), 0);
}
