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
#include "engine/fixed_batch_executor.h"
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
// 1. BertWordPieceTokenizer 单元测试 (R3-014)
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
  EXPECT_TRUE(tokenizer.Encode("Hello embedding", 8, &ids, &mask, &diag));
  ASSERT_EQ(ids.size(), 8u);
  ASSERT_EQ(mask.size(), 8u);

  // [CLS](2), hello(4), em(6), ##bed(7), ##ding(8), [SEP](3), [PAD](0),
  // [PAD](0)
  EXPECT_EQ(ids[0], 2);
  EXPECT_EQ(ids[1], 4);
  EXPECT_EQ(ids[2], 6);
  EXPECT_EQ(ids[3], 7);
  EXPECT_EQ(ids[4], 8);
  EXPECT_EQ(ids[5], 3);
  EXPECT_EQ(ids[6], 0);
  EXPECT_EQ(ids[7], 0);

  EXPECT_EQ(mask[0], 1);
  EXPECT_EQ(mask[1], 1);
  EXPECT_EQ(mask[2], 1);
  EXPECT_EQ(mask[3], 1);
  EXPECT_EQ(mask[4], 1);
  EXPECT_EQ(mask[5], 1);
  EXPECT_EQ(mask[6], 0);
  EXPECT_EQ(mask[7], 0);

  // 2. 中文 CJK 逐字切分
  EXPECT_TRUE(tokenizer.Encode("北京大学", 6, &ids, &mask, &diag));
  ASSERT_EQ(ids.size(), 6u);
  EXPECT_EQ(ids[0], 2);
  EXPECT_EQ(ids[1], 9);
  EXPECT_EQ(ids[2], 10);
  EXPECT_EQ(ids[3], 11);
  EXPECT_EQ(ids[4], 12);
  EXPECT_EQ(ids[5], 3);

  // 3. 截断保留 [SEP]
  EXPECT_TRUE(tokenizer.Encode("北京大学", 4, &ids, &mask, &diag));
  ASSERT_EQ(ids.size(), 4u);
  EXPECT_EQ(ids[0], 2);
  EXPECT_EQ(ids[1], 9);
  EXPECT_EQ(ids[2], 10);
  EXPECT_EQ(ids[3], 3);
  EXPECT_EQ(mask[0], 1);
  EXPECT_EQ(mask[3], 1);

  // 4. 未知词回退到 [UNK]
  EXPECT_TRUE(tokenizer.Encode("foobar", 4, &ids, &mask, &diag));
  EXPECT_EQ(ids[0], 2);
  EXPECT_EQ(ids[1], 1);
  EXPECT_EQ(ids[2], 3);
  EXPECT_EQ(ids[3], 0);

  // 5. 非法 UTF-8 严格 Fail-Closed
  std::string bad_utf8 = "valid\xFF\xFEinvalid";
  EXPECT_FALSE(tokenizer.Encode(bad_utf8, 8, &ids, &mask, &diag));
  EXPECT_FALSE(diag.empty());
  EXPECT_TRUE(diag.find("Invalid UTF-8") != std::string::npos);
}

TEST_F(OnnxAndEmbeddingModelTest, TokenizerCaseAndSidecarSecurity) {
  BertWordPieceTokenizer tokenizer;
  std::vector<std::string> tokens = {
      "[PAD]", "[UNK]", "[CLS]", "[SEP]", "hello", "Hello",
  };

  std::string diag;
  // do_lower_case = false
  ASSERT_TRUE(tokenizer.LoadFromTokens(tokens, /*do_lower_case=*/false, &diag));
  std::vector<int64_t> ids, mask;
  EXPECT_TRUE(tokenizer.Encode("Hello", 4, &ids, &mask));
  EXPECT_EQ(ids[1], 5);  // Matches "Hello"

  // 词表文件存在性与路径安全
  auto vocab_file = temp_dir_ / "vocab.txt";
  std::ofstream out(vocab_file);
  out << "[PAD]\n[UNK]\n[CLS]\n[SEP]\nword\n";
  out.close();

  EXPECT_TRUE(tokenizer.Load(vocab_file.string(), true, &diag));
  EXPECT_EQ(tokenizer.VocabSize(), 5u);

  // 缺少特殊 token
  std::vector<std::string> missing_special = {"[PAD]", "word"};
  EXPECT_FALSE(tokenizer.LoadFromTokens(missing_special, true, &diag));

  // 重复 token
  std::vector<std::string> duplicate_tokens = {"[PAD]", "[UNK]", "[CLS]",
                                               "[SEP]", "word",  "word"};
  EXPECT_FALSE(tokenizer.LoadFromTokens(duplicate_tokens, true, &diag));
}

// =============================================================================
// 2. FakeTensorGraphSession 与 BgeEmbeddingModel 边界测试 (R3-010, R3-012)
// =============================================================================

class FakeTensorGraphSession : public ITensorGraphSession {
 public:
  FakeTensorGraphSession(int64_t hidden_dim = 4, bool is_3d = true,
                         size_t fixed_batch = 0, size_t max_batch = 4)
      : hidden_dim_(hidden_dim),
        is_3d_(is_3d),
        policy_{max_batch, fixed_batch} {
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
  BatchPolicy GetBatchPolicy() const noexcept override { return policy_; }
  const std::vector<TensorSpec>& Inputs() const noexcept override {
    return inputs_;
  }
  const std::vector<TensorSpec>& Outputs() const noexcept override {
    return outputs_;
  }

  int Run(const TensorMap& inputs, TensorMap* outputs,
          std::string* diagnostic = nullptr) noexcept override {
    run_count_++;
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
      out_desc.shape[0] = batch_size + 1;
    }
    if (corrupt_dim_) {
      if (is_3d_) {
        out_desc.shape[2] = hidden_dim_ + 1;
      } else {
        out_desc.shape[1] = hidden_dim_ + 1;
      }
    }
    if (corrupt_rank_) {
      out_desc.shape = {batch_size, seq_len, hidden_dim_, 2};
    }
    if (corrupt_zero_dim_) {
      out_desc.shape = {batch_size, 0, hidden_dim_};
    }

    Tensor out_tensor;
    CreateHostTensor(out_desc, &out_tensor, nullptr);
    if (out_tensor.buffer && out_tensor.buffer->MutableData()) {
      float* data = static_cast<float*>(out_tensor.buffer->MutableData());
      size_t total_elements = 1;
      for (int64_t d : out_desc.shape) {
        if (d > 0) total_elements *= static_cast<size_t>(d);
      }
      for (size_t i = 0; i < total_elements; ++i) {
        data[i] = static_cast<float>(i % 10 + 1) * 0.1f;
      }
    }

    (*outputs)["last_hidden_state"] = std::move(out_tensor);
    return 0;
  }

  int64_t hidden_dim_ = 4;
  bool is_3d_ = true;
  BatchPolicy policy_;
  int run_count_ = 0;

  bool fail_run_ = false;
  bool corrupt_dtype_ = false;
  bool corrupt_batch_ = false;
  bool corrupt_dim_ = false;
  bool corrupt_rank_ = false;
  bool corrupt_zero_dim_ = false;

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

TEST_F(OnnxAndEmbeddingModelTest,
       BgeEmbeddingModelStrictTensorBoundaryFailures) {
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

  // 1. Session Run 失败
  fake_session->fail_run_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 2. Output Dtype 错误 (R3-010)
  fake_session->fail_run_ = false;
  fake_session->corrupt_dtype_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 3. Output Batch 不匹配 (R3-010)
  fake_session->corrupt_dtype_ = false;
  fake_session->corrupt_batch_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 4. Output Dim 不匹配 (R3-010)
  fake_session->corrupt_batch_ = false;
  fake_session->corrupt_dim_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 5. Output Rank 错误 (R3-010)
  fake_session->corrupt_dim_ = false;
  fake_session->corrupt_rank_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 6. Zero Dimension 错误 (R3-010)
  fake_session->corrupt_rank_ = false;
  fake_session->corrupt_zero_dim_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());
}

TEST_F(OnnxAndEmbeddingModelTest, FixedAndDynamicBatchScheduling) {
  // 1. 固定 Batch = 2
  auto fake_fixed =
      std::make_shared<FakeTensorGraphSession>(4, true, /*fixed_batch=*/2, 2);
  BertWordPieceTokenizer tokenizer;
  std::vector<std::string> tokens = {"[PAD]", "[UNK]", "[CLS]", "[SEP]",
                                     "a",     "b",     "c"};
  ASSERT_TRUE(tokenizer.LoadFromTokens(tokens, true));

  BgeEmbeddingModel model_fixed(fake_fixed, tokenizer, 16, "cls", true,
                                "last_hidden_state", 4, 2);

  // 输入 3 条样本 -> 应切分为 2 个批次 (每批执行 2 条，第 2 批自动 pad 1
  // 条并剥离)
  TextBatch inputs = {{1, 0, "a"}, {2, 0, "b"}, {3, 0, "c"}};
  EmbeddingOptions opts;
  EmbeddingBatch outputs;

  EXPECT_EQ(model_fixed.Embed(inputs, opts, &outputs), 0);
  ASSERT_EQ(outputs.size(), 3u);
  EXPECT_EQ(outputs[0].req_id, 1);
  EXPECT_EQ(outputs[1].req_id, 2);
  EXPECT_EQ(outputs[2].req_id, 3);
  EXPECT_EQ(fake_fixed->run_count_, 2);

  // 2. 模型语义上限冲突拒绝创建 (R3-012)
  ModelCreateContext mctx;
  mctx.backend_session = fake_fixed;
  mctx.model_resource_root = temp_dir_.string();
  auto vocab_path = temp_dir_ / "vocab.txt";
  std::ofstream out(vocab_path);
  out << "[PAD]\n[UNK]\n[CLS]\n[SEP]\nword\n";
  out.close();

  mctx.model_config = {
      {"tokenizer_file", "vocab.txt"},
      {"embedding_dim", 4},
      {"max_batch_size", 1},  // 小于 fixed_batch_size (2) -> 必须明确拒绝
  };
  std::string diag;
  auto rejected_model =
      ModelRegistry::Instance().Create("bge_embedding", mctx, &diag);
  EXPECT_EQ(rejected_model, nullptr);
  EXPECT_TRUE(diag.find("cannot be smaller than Session fixed_batch_size") !=
              std::string::npos);
}

// =============================================================================
// 3. Catalog 注册自省与 TextEmbeddingNode 测试
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
// 4. 真实 ONNX Artifact 运行与 Pipeline Smoke 测试 (R3-013)
// =============================================================================

TEST_F(OnnxAndEmbeddingModelTest, RealOnnxArtifactPassEvidence) {
#ifndef HAVE_ONNXRUNTIME
  GTEST_SKIP() << "ONNX Runtime not compiled into this build.";
#else
  const char* onnx_path = "models/bge_base_zh_v1.5.onnx";
  const char* vocab_path = "models/vocab.txt";

  if (!std::filesystem::exists(onnx_path) ||
      !std::filesystem::exists(vocab_path)) {
    GTEST_SKIP() << "Real BGE ONNX/vocab artifact not found on disk.";
  }

  // 1. Backend Load 真实自省元数据
  auto backend = BackendRegistry::Instance().Create("onnxruntime");
  ASSERT_NE(backend, nullptr);

  BackendLoadSpec bspec;
  bspec.model_path = onnx_path;
  bspec.backend_config = {{"max_batch_size", 2}};
  std::string diag;
  auto session = backend->Load(bspec, &diag);
  ASSERT_NE(session, nullptr) << diag;

  auto tensor_session = std::dynamic_pointer_cast<ITensorGraphSession>(session);
  ASSERT_NE(tensor_session, nullptr);
  ASSERT_EQ(tensor_session->Inputs().size(), 2u);
  EXPECT_EQ(tensor_session->Inputs()[0].name, "input_ids");
  EXPECT_EQ(tensor_session->Inputs()[1].name, "attention_mask");
  ASSERT_EQ(tensor_session->Outputs().size(), 1u);
  EXPECT_EQ(tensor_session->Outputs()[0].name, "last_hidden_state");
  EXPECT_EQ(tensor_session->Outputs()[0].shape[2], 128);  // hidden_dim = 128

  // 2. Model 实例化与真实端到端推理
  ModelCreateContext mctx;
  mctx.backend_session = session;
  mctx.model_resource_root =
      std::filesystem::path(vocab_path).parent_path().string();
  mctx.model_config = {
      {"tokenizer_file", std::filesystem::path(vocab_path).filename().string()},
      {"do_lower_case", true},
      {"max_length", 32},
      {"pooling_strategy", "mean"},
      {"normalize", true},
      {"output_name", "last_hidden_state"},
      {"embedding_dim", 128},
      {"max_batch_size", 2},
  };

  auto model = ModelRegistry::Instance().Create("bge_embedding", mctx, &diag);
  ASSERT_NE(model, nullptr) << diag;

  auto emb_model = std::dynamic_pointer_cast<IEmbeddingModel>(model);
  ASSERT_NE(emb_model, nullptr);

  TextBatch inputs = {
      {101, 0, "hello world"},
      {102, 0, "edgeflow test"},
      {103, 0, "hello world"},  // 与第 1 条相同，用于验证输出稳定确定性
  };
  EmbeddingOptions opts;
  opts.normalize = true;
  EmbeddingBatch outputs;

  int ret = emb_model->Embed(inputs, opts, &outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 3u);

  // 验证 Provenance
  EXPECT_EQ(outputs[0].req_id, 101);
  EXPECT_EQ(outputs[1].req_id, 102);
  EXPECT_EQ(outputs[2].req_id, 103);

  // 验证维度与有效有限值 (No NaN or Inf)
  for (size_t i = 0; i < outputs.size(); ++i) {
    ASSERT_EQ(outputs[i].data.size(), 128u);
    float sum_sq = 0.0f;
    for (float v : outputs[i].data) {
      EXPECT_FALSE(std::isnan(v));
      EXPECT_FALSE(std::isinf(v));
      sum_sq += v * v;
    }
    // 验证 L2 归一化
    EXPECT_NEAR(std::sqrt(sum_sq), 1.0f, 1e-4f);
  }

  // 验证相同输入产出完全一致的向量 (确定性)
  for (size_t d = 0; d < 128; ++d) {
    EXPECT_FLOAT_EQ(outputs[0].data[d], outputs[2].data[d]);
  }

  // 验证不同输入产出不同的向量 (非伪固定模拟值)
  bool is_different = false;
  for (size_t d = 0; d < 128; ++d) {
    if (std::abs(outputs[0].data[d] - outputs[1].data[d]) > 1e-5f) {
      is_different = true;
      break;
    }
  }
  EXPECT_TRUE(is_different);

  // 3. 真实 Pipeline Build 与 Execute 完整验证
  Pipeline pipeline;
  PipelineDiagnostic pdiag;
  bool build_ok =
      pipeline.BuildFromConfigFile("configs/pipeline_doc_qa_onnx.json", &pdiag);
  ASSERT_TRUE(build_ok) << pdiag.message << " at " << pdiag.path;
  EXPECT_TRUE(pipeline.IsReady());

  AlgContext pipeline_ctx;
  TextBatch doc_texts = {{1, 0, "智能长文档问答系统设计与实现"}};
  TextBatch query_texts = {{1, 0, "系统设计"}};
  pipeline_ctx.Set("raw_docs", doc_texts);
  pipeline_ctx.Set("raw_queries", query_texts);

  int run_ret = pipeline.Execute(&pipeline_ctx);
  EXPECT_EQ(run_ret, 0);

  const auto* query_emb = pipeline_ctx.Get<EmbeddingBatch>("query_embeddings");
  ASSERT_NE(query_emb, nullptr);
  ASSERT_EQ(query_emb->size(), 1u);
  EXPECT_EQ((*query_emb)[0].data.size(), 128u);
#endif
}

TEST_F(OnnxAndEmbeddingModelTest, OnnxRuntimeBackendNegativeValidation) {
#ifndef HAVE_ONNXRUNTIME
  GTEST_SKIP() << "ONNX Runtime not compiled into this build.";
#else
  auto backend = BackendRegistry::Instance().Create("onnxruntime");
  ASSERT_NE(backend, nullptr);

  // 1. 空路径 / 不存在路径 / 目录路径
  BackendLoadSpec bspec;
  std::string diag;
  EXPECT_EQ(backend->Load(bspec, &diag), nullptr);

  bspec.model_path = "/non/existent/model.onnx";
  EXPECT_EQ(backend->Load(bspec, &diag), nullptr);

  bspec.model_path = temp_dir_.string();
  EXPECT_EQ(backend->Load(bspec, &diag), nullptr);

  // 2. 加载合法模型后测试 Run 输入负向校验 (R3-011)
  const char* onnx_path = "models/bge_base_zh_v1.5.onnx";
  if (!std::filesystem::exists(onnx_path)) {
    GTEST_SKIP() << "models/bge_base_zh_v1.5.onnx not found";
  }

  bspec.model_path = onnx_path;
  bspec.backend_config = {{"max_batch_size", 2}};
  auto session = backend->Load(bspec, &diag);
  ASSERT_NE(session, nullptr) << diag;
  auto tensor_session = std::dynamic_pointer_cast<ITensorGraphSession>(session);
  ASSERT_NE(tensor_session, nullptr);

  TensorMap inputs;
  TensorMap outputs;

  // 2.1 缺少必要输入
  EXPECT_NE(tensor_session->Run(inputs, &outputs, &diag), 0);
  EXPECT_TRUE(outputs.empty());

  // 2.2 输入 ElementType 错误
  TensorDesc bad_type_desc;
  bad_type_desc.element_type = ElementType::kFloat32;  // 期望 INT64
  bad_type_desc.shape = {1, 16};
  Tensor bad_type_tensor;
  CreateHostTensor(bad_type_desc, &bad_type_tensor, nullptr);
  inputs["input_ids"] = bad_type_tensor;
  inputs["attention_mask"] = bad_type_tensor;
  EXPECT_NE(tensor_session->Run(inputs, &outputs, &diag), 0);
  EXPECT_TRUE(outputs.empty());

  // 2.3 输入超出 max_batch_size (2)
  TensorDesc overflow_batch_desc;
  overflow_batch_desc.element_type = ElementType::kInt64;
  overflow_batch_desc.shape = {3, 16};  // 3 > 2
  Tensor overflow_batch_tensor;
  CreateHostTensor(overflow_batch_desc, &overflow_batch_tensor, nullptr);
  inputs["input_ids"] = overflow_batch_tensor;
  inputs["attention_mask"] = overflow_batch_tensor;
  EXPECT_NE(tensor_session->Run(inputs, &outputs, &diag), 0);
  EXPECT_TRUE(outputs.empty());
#endif
}

}  // namespace alg_framework
