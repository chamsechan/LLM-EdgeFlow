#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "company_alg_interface.h"
#include "contracts/inference_payloads.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "engine/backend_interface.h"
#include "engine/backend_registry.h"
#include "engine/backends/onnxruntime/onnxruntime_backend.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "engine/model_runtime_factory.h"
#include "engine/models/bge_embedding/bge_embedding_model.h"

namespace alg_framework {

class OnnxAndEmbeddingModelTest : public ::testing::Test {
 protected:
  void SetUp() override { Alg_Init(); }

  void TearDown() override { Alg_DeInit(); }
};

// 1. 测试 BackendRegistry 中生产 onnxruntime 后端定义已注册
TEST_F(OnnxAndEmbeddingModelTest, OnnxRuntimeBackendRegisteredInCatalog) {
  auto bdef_opt = BackendRegistry::Instance().Find("onnxruntime");
  ASSERT_TRUE(bdef_opt.has_value());
  EXPECT_EQ(bdef_opt->backend_type, "onnxruntime");
  EXPECT_EQ(bdef_opt->concurrency, InferenceConcurrency::kConcurrent);
  ASSERT_EQ(bdef_opt->supported_protocols.size(), 1u);
  EXPECT_EQ(bdef_opt->supported_protocols[0], ExecutionProtocol::kTensorGraph);
}

// 2. 测试 ModelRegistry 中生产 bge_embedding 模型定义已注册
TEST_F(OnnxAndEmbeddingModelTest, BgeEmbeddingModelRegisteredInCatalog) {
  auto mdef_opt = ModelRegistry::Instance().Find("bge_embedding");
  ASSERT_TRUE(mdef_opt.has_value());
  EXPECT_EQ(mdef_opt->model_type, "bge_embedding");
  EXPECT_EQ(mdef_opt->capability, "embedding");
  EXPECT_EQ(mdef_opt->required_protocol, ExecutionProtocol::kTensorGraph);
  EXPECT_EQ(mdef_opt->concurrency, InferenceConcurrency::kConcurrent);
}

// 3. 测试 OnnxRuntimeBackend 在无效路径下 fail-closed
TEST_F(OnnxAndEmbeddingModelTest, OnnxRuntimeBackendFailsOnInvalidModelPath) {
  auto backend = BackendRegistry::Instance().Create("onnxruntime");
  ASSERT_NE(backend, nullptr);

  BackendLoadSpec spec;
  spec.model_path = "/invalid/path/to/nonexistent_model.onnx";
  std::string diag;
  auto session = backend->Load(spec, &diag);
  EXPECT_EQ(session, nullptr);
  EXPECT_FALSE(diag.empty());
  EXPECT_TRUE(diag.find("does not exist") != std::string::npos ||
              diag.find("cannot open") != std::string::npos ||
              diag.find("Invalid") != std::string::npos);
}

// 4. 测试 Fake TensorGraphSession 驱动 BgeEmbeddingModel 的端到端推理
class FakeTensorGraphSession : public ITensorGraphSession {
 public:
  FakeTensorGraphSession() {
    TensorSpec in_ids{"input_ids", ElementType::kInt64, {-1, 16}};
    TensorSpec in_mask{"attention_mask", ElementType::kInt64, {-1, 16}};
    inputs_ = {in_ids, in_mask};

    TensorSpec out_emb{"last_hidden_state", ElementType::kFloat32, {-1, 16, 4}};
    outputs_ = {out_emb};
  }

  const std::string& BackendType() const noexcept override {
    static const std::string type = "fake_ort";
    return type;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kTensorGraph;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  BatchPolicy GetBatchPolicy() const noexcept override {
    return BatchPolicy{4, 0};
  }
  const std::vector<TensorSpec>& Inputs() const noexcept override {
    return inputs_;
  }
  const std::vector<TensorSpec>& Outputs() const noexcept override {
    return outputs_;
  }

  int Run(const TensorMap& inputs, TensorMap* outputs,
          std::string* diagnostic = nullptr) noexcept override {
    (void)diagnostic;
    if (!outputs) return -1;
    outputs->clear();

    auto it_ids = inputs.find("input_ids");
    if (it_ids == inputs.end()) return -1;

    int64_t batch_size = it_ids->second.desc.shape[0];
    int64_t seq_len = it_ids->second.desc.shape[1];
    int64_t hidden_dim = 4;

    TensorDesc out_desc;
    out_desc.element_type = ElementType::kFloat32;
    out_desc.shape = {batch_size, seq_len, hidden_dim};

    Tensor out_tensor;
    CreateHostTensor(out_desc, &out_tensor, nullptr);
    float* data = static_cast<float*>(out_tensor.buffer->MutableData());

    for (int64_t b = 0; b < batch_size; ++b) {
      for (int64_t s = 0; s < seq_len; ++s) {
        for (int64_t d = 0; d < hidden_dim; ++d) {
          data[(b * seq_len + s) * hidden_dim + d] =
              static_cast<float>(b + 1) * 1.0f + static_cast<float>(d) * 0.1f;
        }
      }
    }

    (*outputs)["last_hidden_state"] = std::move(out_tensor);
    return 0;
  }

 private:
  std::vector<TensorSpec> inputs_;
  std::vector<TensorSpec> outputs_;
};

TEST_F(OnnxAndEmbeddingModelTest, BgeEmbeddingModelInferenceWithFakeSession) {
  auto fake_session = std::make_shared<FakeTensorGraphSession>();

  BgeEmbeddingModel model(fake_session, /*max_length=*/16,
                          /*pooling_strategy=*/"cls", /*normalize=*/true,
                          /*max_batch_size=*/2, /*embedding_dim=*/4);

  EXPECT_EQ(model.ModelType(), "bge_embedding");
  EXPECT_EQ(model.Capability(), "embedding");
  EXPECT_EQ(model.GetMaxBatchSize(), 2u);

  TextBatch inputs;
  inputs.push_back({1001, 0, "Hello edgeflow"});
  inputs.push_back({1002, 0, "BGE embedding model test"});
  inputs.push_back({1003, 0, "Third input for batching"});

  EmbeddingOptions options;
  options.normalize = true;

  EmbeddingBatch outputs;
  int ret = model.Embed(inputs, options, &outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 3u);

  // 验证 Provenance 传递
  EXPECT_EQ(outputs[0].req_id, 1001);
  EXPECT_EQ(outputs[1].req_id, 1002);
  EXPECT_EQ(outputs[2].req_id, 1003);

  // 验证 Embedding 向量维度与 L2 归一化
  for (size_t i = 0; i < outputs.size(); ++i) {
    ASSERT_EQ(outputs[i].data.size(), 4u);
    float sum_sq = 0.0f;
    for (float v : outputs[i].data) {
      sum_sq += v * v;
    }
    EXPECT_NEAR(std::sqrt(sum_sq), 1.0f, 1e-4f);
  }
}

// 5. 测试 TextEmbeddingNode 绑定 BgeEmbeddingModel 的端到端 Pipeline 调度
TEST_F(OnnxAndEmbeddingModelTest, TextEmbeddingNodeBoundToModel) {
  SessionContext session_ctx;
  auto fake_session = std::make_shared<FakeTensorGraphSession>();
  auto model = std::make_shared<BgeEmbeddingModel>(
      fake_session, /*max_length=*/16, "mean", /*normalize=*/true, 2, 4);

  // 在 Session 中注册 Model
  session_ctx.GetModelManager().RegisterModel(
      "test_bge", model, "v1", "bge_embedding", "embedding", "fake_ort");

  auto node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json node_cfg = {{"bind_model", "test_bge"}, {"normalize", true}};
  bool init_ok = node->Init(node_cfg, &session_ctx);
  ASSERT_TRUE(init_ok);

  AlgContext ctx;
  TextBatch texts = {{101, 0, "Query text A"}, {102, 0, "Doc text B"}};
  ctx.Set("text", texts);

  int proc_ret = node->Process(&ctx);
  EXPECT_EQ(proc_ret, 0);

  const auto* result = ctx.Get<EmbeddingBatch>("embedding");
  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->size(), 2u);
  EXPECT_EQ((*result)[0].req_id, 101);
  EXPECT_EQ((*result)[1].req_id, 102);
}

}  // namespace alg_framework
