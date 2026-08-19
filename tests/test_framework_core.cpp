#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/session_context.h"
#include "core/traceable_item.h"
#include "engine/engine_registry.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

// 1. 测试 AlgContext 黑板基础功能与类型安全
TEST(AlgContextTest, BasicAndTypeSafety) {
  AlgContext ctx;
  EXPECT_FALSE(ctx.Has("non_existent_key"));
  EXPECT_EQ(ctx.Get<int>("non_existent_key"), nullptr);

  // 写入基础类型
  ctx.Set("user_id", 12345);
  ctx.Set("user_name", std::string("Alice"));
  EXPECT_TRUE(ctx.Has("user_id"));
  EXPECT_EQ(*ctx.Get<int>("user_id"), 12345);
  EXPECT_EQ(*ctx.Get<std::string>("user_name"), "Alice");

  // 类型不匹配时的安全性检查（不能崩溃，必须返回 nullptr）
  EXPECT_EQ(ctx.Get<double>("user_id"), nullptr);

  // 复杂结构测试
  std::vector<std::string> tags = {"NLP", "NPU", "LLM"};
  ctx.Set("tags", tags);
  auto* retrieved_tags = ctx.Get<std::vector<std::string>>("tags");
  ASSERT_NE(retrieved_tags, nullptr);
  EXPECT_EQ(retrieved_tags->size(), 3);
  EXPECT_EQ((*retrieved_tags)[1], "NPU");

  // 错误码设置与检查
  ctx.SetError(-5001, "Simulated error");
  EXPECT_EQ(ctx.GetErrorCode(), -5001);
  EXPECT_EQ(ctx.GetErrorMessage(), "Simulated error");

  // 清空黑板
  ctx.Clear();
  EXPECT_FALSE(ctx.Has("user_id"));
  EXPECT_EQ(ctx.GetErrorCode(), 0);
}

// 2. 测试 TraceableItem 样本溯源机制
TEST(TraceableItemTest, ProvenanceTracking) {
  TraceableItem<std::string> item1(101, 0, "Chunk 0 of Req 101");
  TraceableItem<std::string> item2(101, 1, "Chunk 1 of Req 101");
  TraceableItem<std::string> item3(102, 0, "Chunk 0 of Req 102");

  EXPECT_EQ(item1.req_id, 101);
  EXPECT_EQ(item1.sub_id, 0);
  EXPECT_EQ(item2.req_id, 101);
  EXPECT_EQ(item2.sub_id, 1);
  EXPECT_EQ(item3.req_id, 102);
  EXPECT_EQ(item3.sub_id, 0);
  EXPECT_EQ(item1.data, "Chunk 0 of Req 101");
}

// 3. 测试 NodeFactory 动态反射与注册机制
TEST(NodeRegistryTest, DynamicReflection) {
  auto& factory = NodeFactory::Instance();

  // 验证已注册的核心算子
  auto node1 = factory.Create("DocChunkPreNode");
  ASSERT_NE(node1, nullptr);
  EXPECT_EQ(node1->Name(), "DocChunkPreNode");

  auto node2 = factory.Create("KeywordMatcherNode");
  ASSERT_NE(node2, nullptr);

  auto node3 = factory.Create("SafetyRulePreNode");
  ASSERT_NE(node3, nullptr);

  auto node4 = factory.Create("OcrInferNode");
  ASSERT_NE(node4, nullptr);

  auto node5 = factory.Create("AsrInferNode");
  ASSERT_NE(node5, nullptr);

  auto node6 = factory.Create("CrossRerankBatchNode");
  ASSERT_NE(node6, nullptr);

  // 不存在的算子名应当安全返回 nullptr
  auto invalid_node = factory.Create("NonExistentNode123");
  EXPECT_EQ(invalid_node, nullptr);
}

// 4. 测试 EngineFactory 与 ModelManager 多模型管理机制
TEST(EngineRegistryTest, ModelManagerAndEngines) {
  ModelManager manager;
  auto& engine_factory = EngineFactory::Instance();

  auto embed_engine = engine_factory.Create("mock_npu_embedding");
  ASSERT_NE(embed_engine, nullptr);

  auto rerank_engine = engine_factory.Create("mock_npu_rerank");
  ASSERT_NE(rerank_engine, nullptr);

  auto llm_engine = engine_factory.Create("mock_npu_llm");
  ASSERT_NE(llm_engine, nullptr);

  auto ocr_engine = engine_factory.Create("mock_npu_ocr");
  ASSERT_NE(ocr_engine, nullptr);

  auto asr_engine = engine_factory.Create("mock_npu_asr");
  ASSERT_NE(asr_engine, nullptr);

  auto onnx_engine = engine_factory.Create("onnx_embedding");
  ASSERT_NE(onnx_engine, nullptr);
  EXPECT_EQ(onnx_engine->EngineType(), "onnx_embedding");

  auto onnx_rerank = engine_factory.Create("onnx_rerank");
  ASSERT_NE(onnx_rerank, nullptr);
  EXPECT_EQ(onnx_rerank->EngineType(), "onnx_rerank");

  auto llama_engine = engine_factory.Create("llama_cpp");
  ASSERT_NE(llama_engine, nullptr);
  EXPECT_EQ(llama_engine->EngineType(), "llama_cpp");

  // 模型装载与提取
  nlohmann::json cfg = {{"max_batch_size", 4}, {"embedding_dim", 128}};
  embed_engine->Load("./models/test_embed.bin", cfg);
  manager.RegisterModel("my_embed_v1",
                        std::shared_ptr<IModelEngine>(std::move(embed_engine)));

  nlohmann::json onnx_cfg = {{"max_batch_size", 4}, {"embedding_dim", 128}};
  onnx_engine->Load("./models/bge_base.onnx", onnx_cfg);
  manager.RegisterModel("onnx_embed_model",
                        std::shared_ptr<IModelEngine>(std::move(onnx_engine)));

  EXPECT_TRUE(manager.HasModel("my_embed_v1"));
  EXPECT_TRUE(manager.HasModel("onnx_embed_model"));
  EXPECT_FALSE(manager.HasModel("unknown_model"));

  auto retrieved = manager.GetModel<IEmbeddingEngine>("onnx_embed_model");
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->GetMaxBatchSize(), 4);
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

// 6. 测试 RuntimeOptions 路径规范化与设备 ID 优先级渗透 (REV2-004)
TEST(PipelineTest, RuntimeOptionsPropagationAndPrecedence) {
  Pipeline pipe;
  RuntimeOptions opts;
  opts.model_root_dir = "/opt/custom_models";
  opts.device_id = 2;
  opts.has_device_id = true;
  pipe.GetSessionContext().SetRuntimeOptions(opts);

  nlohmann::json root_cfg = {{"business_name", "test_runtime_opts"},
                             {"execution_mode", "sequential"},
                             {"models",
                              {{{"model_id", "test_mock_llm"},
                                {"engine_type", "mock_npu_llm"},
                                {"model_path", "./models/qwen.bin"},
                                {"config", {{"max_batch_size", 2}}}}}},
                             {"pipeline", nlohmann::json::array()}};

  bool ok = pipe.BuildFromJson(root_cfg);
  EXPECT_TRUE(ok);

  auto model_engine =
      pipe.GetSessionContext().GetModelManager().GetModel<IModelEngine>(
          "test_mock_llm");
  ASSERT_NE(model_engine, nullptr);

  // 断言 1: 设备 ID 正确透传 (REV2-004)
  EXPECT_EQ(model_engine->GetDeviceId(), 2);

  // 断言 2: 模型相对路径被 /opt/custom_models 规范化拼接 (REV2-004)
  EXPECT_EQ(model_engine->GetLoadedModelPath(), "/opt/custom_models/qwen.bin");
}

}  // namespace alg_framework
