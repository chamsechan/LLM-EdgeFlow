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
TEST(ModelManagerTest, TypedModels) {
  ModelManager manager;
  auto session = std::make_shared<test::TestTensorSession>("fixture.bin",
                                                           BatchPolicy{4, 4});
  ASSERT_TRUE(RegisterTestModel(
      manager, "my_embed",
      std::make_shared<test::TestBizEmbeddingModel>(session, 128), "test-v1"));
  ASSERT_TRUE(RegisterTestModel(
      manager, "my_rerank", std::make_shared<test::TestBizRerankModel>(session),
      "test-v1"));

  EXPECT_TRUE(manager.HasModel("my_embed"));
  EXPECT_TRUE(manager.HasModel("my_rerank"));
  EXPECT_FALSE(manager.HasModel("unknown_model"));

  auto retrieved = manager.GetModel<IRerankModel>("my_rerank");
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->ModelType(), "rerank");
  EXPECT_EQ(manager.GetModel<IEmbeddingModel>("my_rerank"), nullptr);
}

TEST(SessionContextTest, TypedDynamicResourcesRejectMismatchedAccess) {
  SessionContext context;
  SessionResourceKey<std::string> string_key("dynamic-resource");
  SessionResourceKey<int> int_key("dynamic-resource");
  context.SetResource(string_key, std::make_shared<std::string>("value"));

  const auto stored = context.GetResource(string_key);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(*stored, "value");
  EXPECT_THROW(context.GetResource(int_key), std::logic_error);
  EXPECT_THROW(context.SetResource(int_key, std::make_shared<int>(7)),
               std::logic_error);
  EXPECT_EQ(*context.GetResource(string_key), "value");
}

TEST(SessionContextTest, SingleFlightCreatesOneTypedResource) {
  SessionContext context;
  SessionResourceKey<std::string> key("single-flight");
  std::atomic<int> factory_calls{0};
  std::vector<std::shared_ptr<std::string>> results(8);
  std::vector<std::thread> workers;
  workers.reserve(results.size());
  for (size_t i = 0; i < results.size(); ++i) {
    workers.emplace_back([&, i]() {
      results[i] = context.GetOrCreateResource<std::string>(key, [&]() {
        ++factory_calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return std::make_shared<std::string>("shared");
      });
    });
  }
  for (auto& worker : workers) worker.join();

  EXPECT_EQ(factory_calls.load(), 1);
  ASSERT_NE(results.front(), nullptr);
  for (const auto& result : results) {
    EXPECT_EQ(result, results.front());
  }
}

TEST(SessionContextTest, SingleFlightSharesFactoryFailureAndAllowsRetry) {
  SessionContext context;
  SessionResourceKey<std::string> key("failed-single-flight");
  std::atomic<int> factory_calls{0};
  std::promise<void> start;
  const auto ready = start.get_future().share();
  std::vector<std::future<std::string>> workers;
  for (int i = 0; i < 8; ++i) {
    workers.push_back(std::async(std::launch::async, [&] {
      if (ready.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        return std::string("start timeout");
      try {
        context.GetOrCreateResource<std::string>(
            key, [&]() -> std::shared_ptr<std::string> {
              ++factory_calls;
              // 让 flight 保持足够长的时间，以便并发调用方加入。
              std::this_thread::sleep_for(std::chrono::milliseconds(50));
              throw std::runtime_error("resource factory unavailable");
            });
        return std::string("missing exception");
      } catch (const std::runtime_error& error) {
        return std::string(error.what());
      }
    }));
  }
  start.set_value();
  for (auto& worker : workers) {
    ASSERT_EQ(worker.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(worker.get(), "resource factory unavailable");
  }
  // 至少有一个调用方是作为等待者 (而非工厂所有者) 收到异常的。
  EXPECT_LT(factory_calls.load(), 8);
  EXPECT_EQ(context.GetResource(key), nullptr);
  auto recovered = context.GetOrCreateResource<std::string>(key, [&] {
    ++factory_calls;
    return std::make_shared<std::string>("recovered");
  });
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(*recovered, "recovered");
  EXPECT_EQ(context.GetResource(key), recovered);
  const int calls_after_retry = factory_calls.load();
  EXPECT_EQ(context.GetOrCreateResource<std::string>(
                key,
                [&] {
                  ++factory_calls;
                  return std::make_shared<std::string>("unexpected recreation");
                }),
            recovered);
  EXPECT_EQ(factory_calls.load(), calls_after_retry);
}

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
