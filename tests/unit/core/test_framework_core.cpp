#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/session_context.h"
#include "dev_support/inference/test_business_models.h"
#include "engine/fixed_batch_executor.h"
#include "engine/model_interface.h"

namespace llm_edgeflow {

// 1. 测试 AlgContext 黑板基础功能与类型安全
TEST(AlgContextTest, BasicAndTypeSafety) {
  AlgContext ctx;
  EXPECT_FALSE(ctx.Has("non_existent_key"));
  EXPECT_EQ(ctx.Read<int>("non_existent_key"), nullptr);

  // 写入基础类型
  ASSERT_TRUE(ctx.Publish("user_id", 12345));
  ASSERT_TRUE(ctx.Publish("user_name", std::string("Alice")));
  EXPECT_TRUE(ctx.Has("user_id"));
  EXPECT_EQ(*ctx.Read<int>("user_id"), 12345);
  EXPECT_EQ(*ctx.Read<std::string>("user_name"), "Alice");

  // 类型不匹配时的安全性检查（不能崩溃，必须返回 nullptr）
  EXPECT_EQ(ctx.Read<double>("user_id"), nullptr);

  // 复杂结构测试
  std::vector<std::string> tags = {"NLP", "NPU", "LLM"};
  ASSERT_TRUE(ctx.Publish("tags", tags));
  auto* retrieved_tags = ctx.Read<std::vector<std::string>>("tags");
  ASSERT_NE(retrieved_tags, nullptr);
  EXPECT_EQ(retrieved_tags->size(), 3U);
  EXPECT_EQ((*retrieved_tags)[1], "NPU");

  // 错误码设置与检查
  ctx.SetError(-5001, "Simulated error");
  EXPECT_EQ(ctx.GetErrorCode(), -5001);
  EXPECT_EQ(ctx.GetErrorMessage(), "Simulated error");

  // 请求值和诊断在本请求上下文生命周期内保持稳定。
  EXPECT_TRUE(ctx.Has("user_id"));
  EXPECT_EQ(ctx.GetErrorCode(), -5001);
}

// 2. 测试 TraceableItem 样本溯源机制
TEST(TraceableItemTest, ProvenanceTracking) {
  TraceableItem<std::string> item1(101, 0, "Chunk 0 of Req 101");
  TraceableItem<std::string> item2(101, 1, "Chunk 1 of Req 101");
  TraceableItem<std::string> item3(102, 0, "Chunk 0 of Req 102");

  EXPECT_EQ(item1.req_id, 101U);
  EXPECT_EQ(item1.sub_id, 0U);
  EXPECT_EQ(item2.req_id, 101U);
  EXPECT_EQ(item2.sub_id, 1U);
  EXPECT_EQ(item3.req_id, 102U);
  EXPECT_EQ(item3.sub_id, 0U);
  EXPECT_EQ(item1.data, "Chunk 0 of Req 101");
}

// 3. 测试 NodeFactory 动态反射与注册机制
TEST(NodeRegistryTest, DynamicReflection) {
  auto& factory = NodeFactory::Instance();

  // 验证已注册的核心算子
  auto node1 = factory.Create("TextChunkNode");
  ASSERT_NE(node1, nullptr);
  EXPECT_EQ(node1->Name(), "TextChunkNode");

  auto node2 = factory.Create("TextRuleMatchNode");
  ASSERT_NE(node2, nullptr);

  auto node3 = factory.Create("TextTemplateNode");
  ASSERT_NE(node3, nullptr);

  auto node4 = factory.Create("OcrDetectNode");
  ASSERT_NE(node4, nullptr);

  auto node5 = factory.Create("AsrTranscribeNode");
  ASSERT_NE(node5, nullptr);

  auto node6 = factory.Create("TextRerankNode");
  ASSERT_NE(node6, nullptr);

  // 不存在的算子名应当安全返回 nullptr
  auto invalid_node = factory.Create("NonExistentNode123");
  EXPECT_EQ(invalid_node, nullptr);
}

// 4. 测试 ModelManager 的强类型多模型管理机制
TEST(ModelManagerTest, TypedModels) {
  ModelManager manager;
  ASSERT_TRUE(manager.RegisterModel(
      "my_embed_v1", std::make_shared<test::TestBusinessEmbeddingModel>(128, 4),
      "test-v1"));
  ASSERT_TRUE(manager.RegisterModel(
      "my_rerank_v1", std::make_shared<test::TestBusinessRerankModel>(4),
      "test-v1"));

  EXPECT_TRUE(manager.HasModel("my_embed_v1"));
  EXPECT_TRUE(manager.HasModel("my_rerank_v1"));
  EXPECT_FALSE(manager.HasModel("unknown_model"));

  auto retrieved = manager.GetModel<IRerankModel>("my_rerank_v1");
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->GetMaxBatchSize(), 4);
  EXPECT_EQ(retrieved->Capability(), "rerank");
  EXPECT_EQ(manager.GetModel<IEmbeddingModel>("my_rerank_v1"), nullptr);
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

// 5. 测试 Pipeline 解析异常与健壮性拦截
TEST(PipelineTest, ErrorHandlingAndRobustness) {
  Pipeline pipe;

  // 传入不存在的配置文件
  bool ok = pipe.BuildFromConfigFile("non_existent_config_file_999.json");
  EXPECT_FALSE(ok);

  // 传入缺少必要字段的畸形 JSON
  nlohmann::json malformed_json = {{"unrelated_key", 123}};
  ok = pipe.BuildFromJson(malformed_json);
  EXPECT_FALSE(ok);
}

// 6. 测试 RuntimeOptions 与 Model/Backend 新方言构建
TEST(PipelineTest, RuntimeOptionsWithModelBackendDialect) {
  Pipeline pipe;
  RuntimeOptions opts;
  opts.device_id = 2;
  opts.has_device_id = true;
  pipe.GetSessionContext().SetRuntimeOptions(opts);

  nlohmann::json root_cfg = {{"biz_name", "test_runtime_opts"},
                             {"execution_mode", "sequential"},
                             {"models",
                              {{{"model_id", "test_mock_llm"},
                                {"capability", "llm"},
                                {"model_type", "test_business_llm"},
                                {"backend", "test_causal_lm_backend"},
                                {"model_path", "./models/qwen.bin"},
                                {"model_config", {{"max_batch_size", 2}}},
                                {"backend_config", nlohmann::json::object()}}}},
                             {"pipeline",
                              {{{"id", "node_0_TextChunkNode"},
                                {"node_type", "TextChunkNode"},
                                {"depends_on", nlohmann::json::array()}}}}};

  PipelineDiagnostic diag;
  bool ok = pipe.BuildFromJson(root_cfg, &diag,
                               ValidationPolicy::kPrivateExtensionCompatible);
  EXPECT_TRUE(ok) << "Build failed: " << diag.message
                  << " (code: " << static_cast<int>(diag.code)
                  << ", path: " << diag.path << ")";

  auto model = pipe.GetSessionContext().GetModelManager().GetModel<ILlmModel>(
      "test_mock_llm");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->ModelType(), "test_business_llm");
  const auto metadata =
      pipe.GetSessionContext().GetModelManager().GetModelRegistration(
          "test_mock_llm");
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->backend_type, "test_causal_lm_backend");
}

}  // namespace llm_edgeflow
