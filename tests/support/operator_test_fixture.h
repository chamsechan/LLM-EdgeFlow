#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "edgeflow/operator/interface.h"

namespace llm_edgeflow::test_support {

class OperatorTestFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    ops_ = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
    ASSERT_EQ(ops_.Init(), 0) << operator_api::GetOperatorLastError();
  }

  void TearDown() override {
    EXPECT_EQ(ops_.DeInit(), 0) << operator_api::GetOperatorLastError();
  }

  operator_api::OperatorFunc ops_{};
};

// Declare before output containers so their leases are returned before cleanup,
// including when a fatal assertion returns early from a test.
class ScopedTestOperator {
 public:
  explicit ScopedTestOperator(operator_api::OperatorFunc ops) : ops_(ops) {}
  ScopedTestOperator(const ScopedTestOperator&) = delete;
  ScopedTestOperator& operator=(const ScopedTestOperator&) = delete;

  ~ScopedTestOperator() {
    if (handle_) {
      const int code = Close();
      EXPECT_EQ(code, 0) << close_diagnostic_;
    }
  }

  int Create(const std::string& conf, const std::string& root = ".",
             operator_api::ComputePlatform platform =
                 operator_api::ComputePlatform::kCpu,
             uint32_t depth = 25, int32_t device = 0) {
    if (handle_) {
      ADD_FAILURE() << "Close the current Operator before creating another";
      return -1;
    }
    operator_api::CreateParam param{};
    param.cfg_file_name = conf.c_str();
    param.model_path = root.c_str();
    param.compute_platform = platform;
    param.max_frame_depth = depth;
    param.device_id = device;
    const int code = ops_.Create(&handle_, &param);
    create_diagnostic_ = operator_api::GetOperatorLastError();
    return code;
  }

  int Close() {
    if (!handle_) return 0;
    // Destroy consumes a handle even when it reports an error.
    void* handle = std::exchange(handle_, nullptr);
    const int code = ops_.Destroy(handle);
    close_diagnostic_ = operator_api::GetOperatorLastError();
    return code;
  }

  void* get() const { return handle_; }
  const std::string& create_diagnostic() const { return create_diagnostic_; }
  const std::string& close_diagnostic() const { return close_diagnostic_; }

 private:
  operator_api::OperatorFunc ops_;
  void* handle_ = nullptr;
  std::string create_diagnostic_;
  std::string close_diagnostic_;
};

}  // namespace llm_edgeflow::test_support
