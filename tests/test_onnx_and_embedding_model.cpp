#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include "engine/models/bge_embedding/bert_wordpiece_tokenizer.h"
#include "engine/models/bge_embedding/bge_embedding_model.h"

namespace alg_framework {

class OnnxAndEmbeddingModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Alg_Init();
    temp_dir_ = std::filesystem::temp_directory_path() /
                ("test_onnx_bge_" + std::to_string(rand()));
    std::filesystem::create_directories(temp_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(temp_dir_, ec);
    Alg_DeInit();
  }

  std::filesystem::path temp_dir_;
};

// =============================================================================
// 1. BertWordPieceTokenizer 单元测试
// =============================================================================

TEST_F(OnnxAndEmbeddingModelTest, TokenizerBasicAndWordPiece) {
  BertWordPieceTokenizer tokenizer;
  std::vector<std::string> tokens = {
      "[PAD]",  "[UNK]", "[CLS]", "[SEP]", "hello", "world", "em", "##bed",
      "##ding", "北",    "京",    "大",    "学",    "!",     "?",
  };

  std::string diag;
  ASSERT_TRUE(tokenizer.LoadFromTokens(tokens, /*do_lower_case=*/true, &diag));
  EXPECT_EQ(tokenizer.PadTokenId(), 0);
  EXPECT_EQ(tokenizer.UnkTokenId(), 1);
  EXPECT_EQ(tokenizer.ClsTokenId(), 2);
  EXPECT_EQ(tokenizer.SepTokenId(), 3);

  // 1. 英文与 WordPiece
  std::vector<int64_t> ids, mask;
  tokenizer.Encode("Hello embedding", 8, &ids, &mask);
  ASSERT_EQ(ids.size(), 8u);
  ASSERT_EQ(mask.size(), 8u);

  // [CLS](2), hello(4), em(6), ##bed(7), ##ding(8), [SEP](3), [PAD](0),
  // [PAD](0)
  EXPECT_EQ(ids[0], 2);  // [CLS]
  EXPECT_EQ(ids[1], 4);  // hello
  EXPECT_EQ(ids[2], 6);  // em
  EXPECT_EQ(ids[3], 7);  // ##bed
  EXPECT_EQ(ids[4], 8);  // ##ding
  EXPECT_EQ(ids[5], 3);  // [SEP]
  EXPECT_EQ(ids[6], 0);  // [PAD]
  EXPECT_EQ(ids[7], 0);  // [PAD]

  EXPECT_EQ(mask[0], 1);
  EXPECT_EQ(mask[1], 1);
  EXPECT_EQ(mask[2], 1);
  EXPECT_EQ(mask[3], 1);
  EXPECT_EQ(mask[4], 1);
  EXPECT_EQ(mask[5], 1);
  EXPECT_EQ(mask[6], 0);
  EXPECT_EQ(mask[7], 0);

  // 2. 中文 CJK 逐字切分
  tokenizer.Encode("北京大学", 6, &ids, &mask);
  ASSERT_EQ(ids.size(), 6u);
  EXPECT_EQ(ids[0], 2);   // [CLS]
  EXPECT_EQ(ids[1], 9);   // 北
  EXPECT_EQ(ids[2], 10);  // 京
  EXPECT_EQ(ids[3], 11);  // 大
  EXPECT_EQ(ids[4], 12);  // 学
  EXPECT_EQ(ids[5], 3);   // [SEP]

  // 3. 截断保留 [SEP]
  tokenizer.Encode("北京大学", 4, &ids, &mask);
  ASSERT_EQ(ids.size(), 4u);
  EXPECT_EQ(ids[0], 2);   // [CLS]
  EXPECT_EQ(ids[1], 9);   // 北
  EXPECT_EQ(ids[2], 10);  // 京
  EXPECT_EQ(ids[3], 3);   // [SEP]
  EXPECT_EQ(mask[0], 1);
  EXPECT_EQ(mask[3], 1);

  // 4. 未知词回退到 [UNK]
  tokenizer.Encode("foobar", 4, &ids, &mask);
  EXPECT_EQ(ids[0], 2);  // [CLS]
  EXPECT_EQ(ids[1], 1);  // [UNK]
  EXPECT_EQ(ids[2], 3);  // [SEP]
  EXPECT_EQ(ids[3], 0);  // [PAD]
}

TEST_F(OnnxAndEmbeddingModelTest, TokenizerVocabValidation) {
  BertWordPieceTokenizer tokenizer;
  std::string diag;

  // 缺少 [PAD]
  std::vector<std::string> bad_tokens_1 = {"[UNK]", "[CLS]", "[SEP]", "hello"};
  EXPECT_FALSE(tokenizer.LoadFromTokens(bad_tokens_1, true, &diag));
  EXPECT_TRUE(diag.find("missing") != std::string::npos);

  // 包含重复词表项
  std::vector<std::string> bad_tokens_2 = {"[PAD]", "[UNK]", "[CLS]",
                                           "[SEP]", "hello", "hello"};
  EXPECT_FALSE(tokenizer.LoadFromTokens(bad_tokens_2, true, &diag));
  EXPECT_TRUE(diag.find("Duplicate") != std::string::npos);

  // 词表为空
  std::vector<std::string> empty_tokens;
  EXPECT_FALSE(tokenizer.LoadFromTokens(empty_tokens, true, &diag));

  // 文件加载测试与路径安全防逃逸
  auto vocab_file = temp_dir_ / "vocab.txt";
  std::ofstream out(vocab_file);
  out << "[PAD]\n[UNK]\n[CLS]\n[SEP]\ntest\n";
  out.close();

  EXPECT_TRUE(tokenizer.Load(vocab_file.string(), true, &diag));
  EXPECT_EQ(tokenizer.VocabSize(), 5u);

  // 不存在的文件
  EXPECT_FALSE(tokenizer.Load("/non/existent/path/vocab.txt", true, &diag));
}

// =============================================================================
// 2. BgeEmbeddingModel 维度校验与 FakeSession 测试
// =============================================================================

class FakeTensorGraphSession : public ITensorGraphSession {
 public:
  FakeTensorGraphSession(int64_t hidden_dim = 4, bool is_3d = true)
      : hidden_dim_(hidden_dim), is_3d_(is_3d) {
    TensorSpec in_ids{"input_ids", ElementType::kInt64, {-1, 16}};
    TensorSpec in_mask{"attention_mask", ElementType::kInt64, {-1, 16}};
    inputs_ = {in_ids, in_mask};

    if (is_3d_) {
      TensorSpec out_emb{
          "last_hidden_state", ElementType::kFloat32, {-1, 16, hidden_dim}};
      outputs_ = {out_emb};
    } else {
      TensorSpec out_emb{
          "last_hidden_state", ElementType::kFloat32, {-1, hidden_dim}};
      outputs_ = {out_emb};
    }
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
    if (fail_run_) {
      if (diagnostic) *diagnostic = "Forced run failure";
      return -1;
    }
    if (!outputs) return -1;
    outputs->clear();

    auto it_ids = inputs.find("input_ids");
    if (it_ids == inputs.end()) return -1;

    int64_t batch_size = it_ids->second.desc.shape[0];
    int64_t seq_len = it_ids->second.desc.shape[1];

    TensorDesc out_desc;
    out_desc.element_type = ElementType::kFloat32;

    if (corrupt_dtype_) {
      out_desc.element_type = ElementType::kInt32;
    }

    if (is_3d_) {
      out_desc.shape = {batch_size, seq_len, hidden_dim_};
    } else {
      out_desc.shape = {batch_size, hidden_dim_};
    }

    if (corrupt_batch_) {
      out_desc.shape[0] = batch_size - 1;
    }

    Tensor out_tensor;
    CreateHostTensor(out_desc, &out_tensor, nullptr);
    float* data = static_cast<float*>(out_tensor.buffer->MutableData());

    size_t total_elements = 1;
    for (int64_t d : out_desc.shape) total_elements *= static_cast<size_t>(d);

    for (size_t i = 0; i < total_elements; ++i) {
      data[i] = static_cast<float>(i % 10 + 1) * 0.1f;
    }

    (*outputs)["last_hidden_state"] = std::move(out_tensor);
    return 0;
  }

  int64_t hidden_dim_ = 4;
  bool is_3d_ = true;
  bool fail_run_ = false;
  bool corrupt_dtype_ = false;
  bool corrupt_batch_ = false;

 private:
  std::vector<TensorSpec> inputs_;
  std::vector<TensorSpec> outputs_;
};

TEST_F(OnnxAndEmbeddingModelTest, BgeEmbeddingModelCLSAndMeanPooling) {
  auto fake_session_3d = std::make_shared<FakeTensorGraphSession>(4, true);

  BertWordPieceTokenizer tokenizer;
  std::vector<std::string> tokens = {"[PAD]", "[UNK]", "[CLS]", "[SEP]",
                                     "hello", "world", "bge",   "model"};
  ASSERT_TRUE(tokenizer.LoadFromTokens(tokens, true));

  // 1. CLS Pooling
  BgeEmbeddingModel model_cls(fake_session_3d, tokenizer, /*max_length=*/16,
                              /*pooling_strategy=*/"cls", /*normalize=*/true,
                              /*output_name=*/"last_hidden_state",
                              /*embedding_dim=*/4, /*max_batch_size=*/2);

  TextBatch inputs = {
      {1001, 0, "hello world"},
      {1002, 0, "bge model"},
  };

  EmbeddingOptions opts;
  opts.normalize = true;
  EmbeddingBatch outputs;

  EXPECT_EQ(model_cls.Embed(inputs, opts, &outputs), 0);
  ASSERT_EQ(outputs.size(), 2u);
  EXPECT_EQ(outputs[0].req_id, 1001);
  EXPECT_EQ(outputs[1].req_id, 1002);
  ASSERT_EQ(outputs[0].data.size(), 4u);

  // 验证 L2 归一化后范数为 1
  float sum_sq = 0.0f;
  for (float v : outputs[0].data) sum_sq += v * v;
  EXPECT_NEAR(std::sqrt(sum_sq), 1.0f, 1e-4f);

  // 2. Mean Pooling
  BgeEmbeddingModel model_mean(fake_session_3d, tokenizer, 16, "mean", true,
                               "last_hidden_state", 4, 2);
  EXPECT_EQ(model_mean.Embed(inputs, opts, &outputs), 0);
  ASSERT_EQ(outputs.size(), 2u);

  // 3. 2D 输出 Direct Embedding
  auto fake_session_2d = std::make_shared<FakeTensorGraphSession>(4, false);
  BgeEmbeddingModel model_2d(fake_session_2d, tokenizer, 16, "cls", true,
                             "last_hidden_state", 4, 2);
  EXPECT_EQ(model_2d.Embed(inputs, opts, &outputs), 0);
  ASSERT_EQ(outputs.size(), 2u);
}

TEST_F(OnnxAndEmbeddingModelTest, BgeEmbeddingModelOutputValidationFailure) {
  auto fake_session = std::make_shared<FakeTensorGraphSession>(4, true);

  BertWordPieceTokenizer tokenizer;
  std::vector<std::string> tokens = {"[PAD]", "[UNK]", "[CLS]", "[SEP]",
                                     "hello"};
  ASSERT_TRUE(tokenizer.LoadFromTokens(tokens, true));

  BgeEmbeddingModel model(fake_session, tokenizer, 16, "cls", true,
                          "last_hidden_state", 4, 2);

  TextBatch inputs = {{1, 0, "hello"}};
  EmbeddingOptions opts;
  EmbeddingBatch outputs;

  // Session Run 失败
  fake_session->fail_run_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // Output Dtype 错误
  fake_session->fail_run_ = false;
  fake_session->corrupt_dtype_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // Output Batch 不匹配
  fake_session->corrupt_dtype_ = false;
  fake_session->corrupt_batch_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());
}

// =============================================================================
// 3. Catalog 注册与 Backend/Model 契约自省测试
// =============================================================================

TEST_F(OnnxAndEmbeddingModelTest, CatalogRegistrations) {
#ifdef HAVE_ONNXRUNTIME
  auto bdef_opt = BackendRegistry::Instance().Find("onnxruntime");
  ASSERT_TRUE(bdef_opt.has_value());
  EXPECT_EQ(bdef_opt->backend_type, "onnxruntime");
  EXPECT_EQ(bdef_opt->concurrency, InferenceConcurrency::kConcurrent);
  ASSERT_EQ(bdef_opt->supported_protocols.size(), 1u);
  EXPECT_EQ(bdef_opt->supported_protocols[0], ExecutionProtocol::kTensorGraph);
#endif

  auto mdef_opt = ModelRegistry::Instance().Find("bge_embedding");
  ASSERT_TRUE(mdef_opt.has_value());
  EXPECT_EQ(mdef_opt->model_type, "bge_embedding");
  EXPECT_EQ(mdef_opt->capability, "embedding");
  EXPECT_EQ(mdef_opt->required_protocol, ExecutionProtocol::kTensorGraph);
  EXPECT_EQ(mdef_opt->concurrency, InferenceConcurrency::kConcurrent);
}

// =============================================================================
// 4. TextEmbeddingNode 端到端绑定与执行测试
// =============================================================================

TEST_F(OnnxAndEmbeddingModelTest, TextEmbeddingNodeBoundToModel) {
  SessionContext session_ctx;
  auto fake_session = std::make_shared<FakeTensorGraphSession>(4, true);

  BertWordPieceTokenizer tokenizer;
  std::vector<std::string> tokens = {"[PAD]", "[UNK]", "[CLS]",
                                     "[SEP]", "query", "doc"};
  ASSERT_TRUE(tokenizer.LoadFromTokens(tokens, true));

  auto model = std::make_shared<BgeEmbeddingModel>(
      fake_session, tokenizer, /*max_length=*/16, "mean", /*normalize=*/true,
      "last_hidden_state", /*embedding_dim=*/4, /*max_batch_size=*/2);

  session_ctx.GetModelManager().RegisterModel(
      "test_bge", model, "v1", "bge_embedding", "embedding", "fake_ort");

  auto node = NodeFactory::Instance().Create("TextEmbeddingNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json node_cfg = {{"bind_model", "test_bge"}, {"normalize", true}};
  bool init_ok = node->Init(node_cfg, &session_ctx);
  ASSERT_TRUE(init_ok);

  AlgContext ctx;
  TextBatch texts = {{101, 0, "query"}, {102, 0, "doc"}};
  ctx.Set("text", texts);

  int proc_ret = node->Process(&ctx);
  EXPECT_EQ(proc_ret, 0);

  const auto* result = ctx.Get<EmbeddingBatch>("embedding");
  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->size(), 2u);
  EXPECT_EQ((*result)[0].req_id, 101);
  EXPECT_EQ((*result)[1].req_id, 102);
}

// =============================================================================
// 5. 真实 ONNX Artifact 条件测试 (LLM_EDGEFLOW_TEST_BGE_ONNX / VOCAB)
// =============================================================================

TEST_F(OnnxAndEmbeddingModelTest, RealOnnxArtifactConditionalTest) {
#ifndef HAVE_ONNXRUNTIME
  GTEST_SKIP() << "ONNX Runtime not compiled into this build.";
#else
  const char* onnx_path = std::getenv("LLM_EDGEFLOW_TEST_BGE_ONNX");
  const char* vocab_path = std::getenv("LLM_EDGEFLOW_TEST_BGE_VOCAB");

  if (!onnx_path || !vocab_path) {
    // 检查默认本地路径
    if (!std::filesystem::exists("models/bge_base_zh_v1.5.onnx") ||
        !std::filesystem::exists("models/vocab.txt")) {
      GTEST_SKIP() << "Real BGE ONNX/vocab artifact not found. Set "
                      "LLM_EDGEFLOW_TEST_BGE_ONNX and "
                      "LLM_EDGEFLOW_TEST_BGE_VOCAB to enable.";
    }
    onnx_path = "models/bge_base_zh_v1.5.onnx";
    vocab_path = "models/vocab.txt";
  }

  auto backend = BackendRegistry::Instance().Create("onnxruntime");
  ASSERT_NE(backend, nullptr);

  BackendLoadSpec bspec;
  bspec.model_path = onnx_path;
  bspec.backend_config = {{"max_batch_size", 2}};
  std::string diag;
  auto session = backend->Load(bspec, &diag);
  ASSERT_NE(session, nullptr) << diag;

  ModelCreateContext mctx;
  mctx.backend_session = session;
  mctx.model_resource_root =
      std::filesystem::path(vocab_path).parent_path().string();
  mctx.model_config = {
      {"tokenizer_file", std::filesystem::path(vocab_path).filename().string()},
      {"do_lower_case", true},
      {"max_length", 128},
      {"pooling_strategy", "cls"},
      {"normalize", true},
      {"output_name", "last_hidden_state"},
      {"embedding_dim", 768},
      {"max_batch_size", 2},
  };

  auto model = ModelRegistry::Instance().Create("bge_embedding", mctx, &diag);
  ASSERT_NE(model, nullptr) << diag;

  auto emb_model = std::dynamic_pointer_cast<IEmbeddingModel>(model);
  ASSERT_NE(emb_model, nullptr);

  TextBatch inputs = {{1, 0, "测试真实ONNX向量推理"}, {2, 0, "EdgeFlow框架"}};
  EmbeddingOptions opts;
  opts.normalize = true;
  EmbeddingBatch outputs;

  int ret = emb_model->Embed(inputs, opts, &outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 2u);
  EXPECT_EQ(outputs[0].req_id, 1);
  EXPECT_EQ(outputs[1].req_id, 2);
  ASSERT_EQ(outputs[0].data.size(), 768u);
  ASSERT_EQ(outputs[1].data.size(), 768u);

  float norm1 = 0.0f, norm2 = 0.0f;
  for (float v : outputs[0].data) norm1 += v * v;
  for (float v : outputs[1].data) norm2 += v * v;
  EXPECT_NEAR(std::sqrt(norm1), 1.0f, 1e-3f);
  EXPECT_NEAR(std::sqrt(norm2), 1.0f, 1e-3f);
#endif
}

}  // namespace alg_framework
