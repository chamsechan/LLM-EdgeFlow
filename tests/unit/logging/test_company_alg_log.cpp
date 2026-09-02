#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "company_alg_log.h"

namespace {

class LogLevelGuard {
 public:
  LogLevelGuard()
      : saved_level_(AlgBase_getLogLevelByName(COMPANY_ALG_LOG_NAME)) {}
  ~LogLevelGuard() {
    (void)AlgBase_setLogLevelByName(COMPANY_ALG_LOG_NAME, saved_level_);
  }

 private:
  int saved_level_;
};

}  // namespace

TEST(CompanyAlgLogTest, LevelMappingAndInvalidSetAreDeterministic) {
  LogLevelGuard guard;
  EXPECT_EQ(E_ALG_BASE_LOG_LEVEL_FATAL, 0);
  EXPECT_EQ(E_ALG_BASE_LOG_LEVEL_ERROR, 1);
  EXPECT_EQ(E_ALG_BASE_LOG_LEVEL_WARNING, 2);
  EXPECT_EQ(E_ALG_BASE_LOG_LEVEL_INFO, 3);
  EXPECT_EQ(E_ALG_BASE_LOG_LEVEL_DEBUG, 4);
  EXPECT_EQ(E_ALG_BASE_LOG_LEVEL_VERBOSE, 5);

  ASSERT_EQ(AlgBase_setLogLevelByName("ignored-scope", 2), 0);
  EXPECT_EQ(AlgBase_getLogLevelByName(nullptr), 2);
  EXPECT_EQ(AlgBase_setLogLevelByName(nullptr, -1), -1);
  EXPECT_EQ(AlgBase_setLogLevelByName(nullptr, 6), -1);
  EXPECT_EQ(AlgBase_getLogLevelByName("LLM_EDGEFLOW"), 2);
}

TEST(CompanyAlgLogTest, FilteredArgumentsAreNotEvaluated) {
  LogLevelGuard guard;
  ASSERT_EQ(AlgBase_setLogLevelByName(COMPANY_ALG_LOG_NAME,
                                      E_ALG_BASE_LOG_LEVEL_WARNING),
            0);
  int side_effect = 0;

  testing::internal::CaptureStderr();
  ALG_LOG_INFO("hidden=%d\n", ++side_effect);
  ALG_LOG_WARNING("visible=%d\n", 7);
  const std::string output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(side_effect, 0);
  EXPECT_EQ(output, "[Warning][LLM_EDGEFLOW] visible=7\n");
}

TEST(CompanyAlgLogTest, AllLevelsUseExpectedLabelsAndFatalDoesNotAbort) {
  LogLevelGuard guard;
  ASSERT_EQ(AlgBase_setLogLevelByName(COMPANY_ALG_LOG_NAME,
                                      E_ALG_BASE_LOG_LEVEL_VERBOSE),
            0);

  testing::internal::CaptureStderr();
  ALG_LOG_FATAL("fatal\n");
  ALG_LOG_ERROR("error\n");
  ALG_LOG_WARNING("warning\n");
  ALG_LOG_INFO("info\n");
  ALG_LOG_DEBUG("debug\n");
  ALG_LOG_VERBOSE("verbose\n");
  const std::string output = testing::internal::GetCapturedStderr();

  EXPECT_NE(output.find("[Fatal][LLM_EDGEFLOW] fatal\n"), std::string::npos);
  EXPECT_NE(output.find("[Error][LLM_EDGEFLOW] error\n"), std::string::npos);
  EXPECT_NE(output.find("[Warning][LLM_EDGEFLOW] warning\n"),
            std::string::npos);
  EXPECT_NE(output.find("[Info][LLM_EDGEFLOW] info\n"), std::string::npos);
  EXPECT_NE(output.find("[Debug][LLM_EDGEFLOW] debug\n"), std::string::npos);
  EXPECT_NE(output.find("[Verbose][LLM_EDGEFLOW] verbose\n"),
            std::string::npos);
}

TEST(CompanyAlgLogTest, PreservesFormatAndSupportsExplicitName) {
  LogLevelGuard guard;
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_INFO), 0);

  testing::internal::CaptureStderr();
  ALG_BASE_LOG_INFO("CUSTOM", "value=%d", 42);
  const std::string output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(output, "[Info][CUSTOM] value=42");
}

TEST(CompanyAlgLogTest, ConcurrentRecordsRemainWhole) {
  LogLevelGuard guard;
  ASSERT_EQ(AlgBase_setLogLevelByName(nullptr, E_ALG_BASE_LOG_LEVEL_WARNING),
            0);
  constexpr int kThreadCount = 4;
  constexpr int kRecordsPerThread = 20;

  testing::internal::CaptureStderr();
  std::vector<std::thread> workers;
  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    workers.emplace_back([thread_id]() {
      for (int record = 0; record < kRecordsPerThread; ++record) {
        ALG_LOG_WARNING("thread=%d record=%d\n", thread_id, record);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  const std::string output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(std::count(output.begin(), output.end(), '\n'),
            kThreadCount * kRecordsPerThread);
  const std::string prefix = "[Warning][LLM_EDGEFLOW] ";
  size_t line_start = 0;
  while (line_start < output.size()) {
    EXPECT_EQ(output.compare(line_start, prefix.size(), prefix), 0);
    line_start = output.find('\n', line_start);
    ASSERT_NE(line_start, std::string::npos);
    ++line_start;
  }
}

TEST(CompanyAlgLogTest, ConcurrentLevelAccessStaysInRange) {
  LogLevelGuard guard;
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  for (int thread_id = 0; thread_id < 8; ++thread_id) {
    workers.emplace_back([thread_id, &failed]() {
      for (int iteration = 0; iteration < 1000; ++iteration) {
        const int level = (thread_id + iteration) % 6;
        if (AlgBase_setLogLevelByName(nullptr, level) != 0) failed = true;
        const int observed = AlgBase_getLogLevelByName(nullptr);
        if (observed < 0 || observed > 5) failed = true;
      }
    });
  }
  for (auto& worker : workers) worker.join();
  EXPECT_FALSE(failed.load());
}
