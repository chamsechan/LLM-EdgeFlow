#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/session_context.h"
#include "core/thread_pool.h"
#include "dev_support/inference/test_biz_models.h"
#include "dev_support/inference/test_tensor_backend.h"
#include "engine/fixed_batch_executor.h"
#include "engine/model_interface.h"
#include "tests/support/model_registration.h"
#include "tests/support/pipeline_test_utils.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {

TEST(PipelineTest, ThreadPoolConstructionFailureCleansUpAndAllowsRetry) {
  bool completed = false;
  for (std::ptrdiff_t fail_after = 0; fail_after < 64; ++fail_after) {
    bool threw = false;
    bool triggered = false;
    {
      test_support::ScopedAllocationFailure failure(fail_after);
      try {
        ThreadPool pool(4);
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      triggered = failure.Triggered();
      failure.DisableFailure();
    }
    EXPECT_EQ(threw, triggered) << "allocation position " << fail_after;
    // 每次构造失败后，新建的线程池仍必须能执行任务。
    ThreadPool recovered(2);
    EXPECT_EQ(recovered.Submit([] { return 42; }).get(), 42);
    if (!triggered) {
      completed = true;
      break;
    }
  }
  EXPECT_TRUE(completed);
}

// 1. 测试 AlgContext 黑板基础功能与类型安全
// 5. 测试 RuntimeOptions 与 Model/Backend 新方言构建
TEST(PipelineTest, RuntimeOptionsWithModelBackendDialect) {
  const auto boundary = MakeTestBoundary(
      {{"input.text", "TextBatch"}},
      {{"chunk_input.chunks", "TextBatch", true, "1:N", "generate_sub_id"}});
  Pipeline pipe;
  RuntimeOptions opts;
  opts.device_id = 2;
  opts.has_device_id = true;
  pipe.GetSessionContext().SetRuntimeOptions(opts);

  nlohmann::json root_cfg = {
      {"models",
       {{{"name", "test_mock_llm"},
         {"type", "llm"},
         {"backend",
          {{"type", "test_causal_lm_backend"},
           {"params", nlohmann::json::object()}}},
         {"file", "./models/qwen.bin"},
         {"params", nlohmann::json::object()}}}},
      {"pipeline",
       {{{"name", "chunk_input"},
         {"type", "text_chunk"},
         {"inputs", {{"text", "input.text"}}},
         {"depends_on", nlohmann::json::array()}},
        {{"name", "consume_llm"},
         {"type", "llm_generate"},
         {"inputs", {{"input", "input.text"}}},
         {"params",
          {{"bind_model", "test_mock_llm"},
           {"endpoints", {{"answer", nlohmann::json::object()}}}}}}}}};

  PipelineDiagnostic diag;
  bool ok = BuildTestPipeline(pipe, root_cfg, boundary, &diag);
  EXPECT_TRUE(ok) << "Build failed: " << diag.message
                  << " (code: " << static_cast<int>(diag.code)
                  << ", path: " << diag.path << ")";

  auto model = pipe.GetSessionContext().GetModelManager().GetModel<ILlmModel>(
      "test_mock_llm");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->ImplName(), "test_biz_llm");
  const auto metadata =
      pipe.GetSessionContext().GetModelManager().GetModelRegistration(
          "test_mock_llm");
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->backend_type, "test_causal_lm_backend");
}

}  // namespace llm_edgeflow
