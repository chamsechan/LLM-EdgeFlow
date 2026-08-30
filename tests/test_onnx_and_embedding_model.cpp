#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
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
#include "engine/models/bge_common/bert_model_support.h"
#include "engine/models/bge_embedding/bert_wordpiece_tokenizer.h"
#include "engine/models/bge_embedding/bge_embedding_model.h"
#include "tests/support/inference/bge_model_test_support.h"

#ifndef EDGEFLOW_STAGE3_ONNX_FIXTURE
#define EDGEFLOW_STAGE3_ONNX_FIXTURE "models/embedding_fixture.onnx"
#endif
#ifndef EDGEFLOW_STAGE3_VOCAB_FIXTURE
#define EDGEFLOW_STAGE3_VOCAB_FIXTURE "models/vocab.txt"
#endif

namespace alg_framework {

class OnnxAndEmbeddingModelTest : public BgeModelTestBase {};

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

TEST_F(OnnxAndEmbeddingModelTest,
       BertInputTensorsAllocateOnlyDeclaredOptionalInput) {
  std::string diag;
  BertInputTensors without_types;
  ASSERT_TRUE(without_types.Create(2, 16, false, &diag)) << diag;
  EXPECT_EQ(without_types.types, nullptr);
  EXPECT_EQ(without_types.token_type_ids.buffer, nullptr);
  TensorMap without_types_map = without_types.ReleaseToMap();
  EXPECT_EQ(without_types_map.count("token_type_ids"), 0u);

  BertInputTensors with_types;
  ASSERT_TRUE(with_types.Create(2, 16, true, &diag)) << diag;
  EXPECT_NE(with_types.types, nullptr);
  EXPECT_NE(with_types.token_type_ids.buffer, nullptr);
  TensorMap with_types_map = with_types.ReleaseToMap();
  EXPECT_EQ(with_types_map.count("token_type_ids"), 1u);
}

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
    return concurrency_;
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
    if (fail_run_ || (fail_on_run_ > 0 && run_count_ == fail_on_run_)) {
      if (diagnostic) *diagnostic = "Forced run failure";
      return -1;
    }
    if (!outputs) return -1;
    outputs->clear();

    auto it_ids = inputs.find("input_ids");
    if (it_ids == inputs.end()) return -1;

    int64_t batch_size = it_ids->second.desc.shape[0];
    int64_t seq_len = it_ids->second.desc.shape[1];
    observed_batch_sizes_.push_back(static_cast<size_t>(batch_size));

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
    if (corrupt_sequence_delta_ != 0 && is_3d_) {
      out_desc.shape[1] += corrupt_sequence_delta_;
    }
    if (corrupt_negative_dim_ && is_3d_) {
      out_desc.shape[1] = -1;
    }
    if (corrupt_overflow_ && is_3d_) {
      out_desc.shape = {batch_size, std::numeric_limits<int64_t>::max(),
                        std::numeric_limits<int64_t>::max()};
    }

    Tensor out_tensor;
    const bool inject_buffer = corrupt_byte_delta_ != 0 ||
                               corrupt_misalignment_ || corrupt_null_data_ ||
                               corrupt_negative_dim_ || corrupt_overflow_;
    if (inject_buffer) {
      size_t expected_bytes = 16;
      std::string ignored;
      inference_detail::ComputeTensorByteSize(
          out_desc, ElementTypeByteSize(out_desc.element_type), &expected_bytes,
          &ignored);
      if (corrupt_byte_delta_ < 0) {
        const size_t reduction = static_cast<size_t>(-corrupt_byte_delta_);
        expected_bytes =
            reduction > expected_bytes ? 0 : expected_bytes - reduction;
      } else {
        expected_bytes += static_cast<size_t>(corrupt_byte_delta_);
      }
      out_tensor =
          MakeFaultTensor(out_desc, expected_bytes,
                          corrupt_misalignment_ ? 1 : 0, corrupt_null_data_);
    } else {
      CreateHostTensor(out_desc, &out_tensor, nullptr);
    }
    if (out_desc.element_type == ElementType::kFloat32) {
      float* data = GetMutableTensorData<float>(&out_tensor, nullptr);
      if (data) {
        size_t total_elements = out_tensor.buffer->ByteSize() / sizeof(float);
        for (size_t i = 0; i < total_elements; ++i) {
          data[i] = static_cast<float>(i % 10 + 1) * 0.1f;
        }
        if (non_finite_output_ && total_elements > 0) {
          data[0] = std::numeric_limits<float>::infinity();
        }
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
  int64_t corrupt_sequence_delta_ = 0;
  bool corrupt_negative_dim_ = false;
  bool corrupt_overflow_ = false;
  int corrupt_byte_delta_ = 0;
  bool corrupt_misalignment_ = false;
  bool corrupt_null_data_ = false;
  bool non_finite_output_ = false;
  int fail_on_run_ = 0;
  InferenceConcurrency concurrency_ = InferenceConcurrency::kConcurrent;
  std::vector<size_t> observed_batch_sizes_;

  void SetInputs(std::vector<TensorSpec> inputs) {
    inputs_ = std::move(inputs);
  }
  void SetOutputs(std::vector<TensorSpec> outputs) {
    outputs_ = std::move(outputs);
  }

  void ResetFaults() noexcept {
    fail_run_ = false;
    corrupt_dtype_ = false;
    corrupt_batch_ = false;
    corrupt_dim_ = false;
    corrupt_rank_ = false;
    corrupt_zero_dim_ = false;
    corrupt_sequence_delta_ = 0;
    corrupt_negative_dim_ = false;
    corrupt_overflow_ = false;
    corrupt_byte_delta_ = 0;
    corrupt_misalignment_ = false;
    corrupt_null_data_ = false;
    non_finite_output_ = false;
    fail_on_run_ = 0;
  }

  void ResetMetrics() {
    run_count_ = 0;
    observed_batch_sizes_.clear();
  }

 private:
  std::vector<TensorSpec> inputs_;
  std::vector<TensorSpec> outputs_;
};

TEST_F(OnnxAndEmbeddingModelTest, ModelSidecarContainmentSecurity) {
  auto model_root = temp_dir_ / "model";
  auto sibling_root = temp_dir_ / "model-escape";
  auto outside_root = temp_dir_ / "outside";
  std::filesystem::create_directories(model_root);
  std::filesystem::create_directories(sibling_root);
  std::filesystem::create_directories(outside_root);
  WriteTestVocab(model_root / "vocab.txt");
  WriteTestVocab(sibling_root / "vocab.txt");
  WriteTestVocab(outside_root / "vocab.txt");

  ModelCreateContext context;
  context.backend_session = std::make_shared<FakeTensorGraphSession>(4, true);
  context.model_resource_root = model_root.string();
  context.model_config = {
      {"tokenizer_file", "vocab.txt"},
      {"embedding_dim", 4},
      {"max_length", 16},
  };

  std::string diag;
  EXPECT_NE(BgeEmbeddingModel::Create(context, &diag), nullptr) << diag;

  // 普通词法逃逸必须失败。
  context.model_config["tokenizer_file"] = "../outside/vocab.txt";
  diag.clear();
  EXPECT_EQ(BgeEmbeddingModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("cannot escape root"), std::string::npos);

  // 同前缀兄弟目录不能通过字符串前缀混淆逃逸。
  context.model_config["tokenizer_file"] = "../model-escape/vocab.txt";
  diag.clear();
  EXPECT_EQ(BgeEmbeddingModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("cannot escape root"), std::string::npos);

  // 已存在的 symlink 指向根外时也必须失败。
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside_root,
                                            model_root / "outside_link", ec);
  ASSERT_FALSE(ec) << ec.message();
  context.model_config["tokenizer_file"] = "outside_link/vocab.txt";
  diag.clear();
  EXPECT_EQ(BgeEmbeddingModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("cannot escape root"), std::string::npos);

  // RFC 明确允许显式绝对路径。
  context.model_config["tokenizer_file"] =
      (sibling_root / "vocab.txt").string();
  diag.clear();
  EXPECT_NE(BgeEmbeddingModel::Create(context, &diag), nullptr) << diag;
}

TEST_F(OnnxAndEmbeddingModelTest,
       BgeEmbeddingModelValidatesSessionMetadataAtCreation) {
  WriteTestVocab(temp_dir_ / "vocab.txt");
  auto session = std::make_shared<FakeTensorGraphSession>(4, true);

  ModelCreateContext context;
  context.backend_session = session;
  context.model_resource_root = temp_dir_.string();
  context.model_config = {
      {"tokenizer_file", "vocab.txt"},
      {"embedding_dim", 4},
      {"max_length", 16},
  };

  std::string diag;
  ASSERT_NE(BgeEmbeddingModel::Create(context, &diag), nullptr) << diag;

  context.model_config["max_batch_size"] = 0;
  EXPECT_EQ(BgeEmbeddingModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("max_batch_size must be at least 1"), std::string::npos);
  context.model_config["max_batch_size"] = 4;

  session->SetInputs({
      {"input_ids", ElementType::kInt64, {-1, 16}},
      {"unexpected", ElementType::kInt64, {-1, 16}},
  });
  EXPECT_EQ(BgeEmbeddingModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("unrecognized required input"), std::string::npos);

  session->SetInputs({
      {"input_ids", ElementType::kInt64, {-1, 16}},
      {"attention_mask", ElementType::kInt64, {-1, 16}},
  });
  session->SetOutputs(
      {{"last_hidden_state", ElementType::kFloat32, {-1, 16, 8}}});
  EXPECT_EQ(BgeEmbeddingModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("does not match configured embedding_dim"),
            std::string::npos);
}

TEST_F(OnnxAndEmbeddingModelTest,
       BgeEmbeddingConcurrencyReflectsModelSemantics) {
  auto session = std::make_shared<FakeTensorGraphSession>(4, true);
  session->concurrency_ = InferenceConcurrency::kSerialized;
  BertWordPieceTokenizer tokenizer;
  ASSERT_TRUE(
      tokenizer.LoadFromTokens({"[PAD]", "[UNK]", "[CLS]", "[SEP]"}, true));

  BgeEmbeddingModel model(session, std::move(tokenizer), 16, "cls", true,
                          "last_hidden_state", 4, 2);
  EXPECT_EQ(model.Concurrency(), InferenceConcurrency::kConcurrent);
}

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

  // 7. 3D sequence 必须与输入 Tensor 的 max_length 完全一致。
  fake_session->ResetFaults();
  fake_session->corrupt_sequence_delta_ = -1;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_sequence_delta_ = 1;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 8. 负维度、溢出、过短/过长 Buffer、错位和空数据全部 fail closed。
  fake_session->ResetFaults();
  fake_session->corrupt_negative_dim_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_overflow_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_byte_delta_ = -1;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_byte_delta_ = 1;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_misalignment_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_null_data_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 9. 非有限输出必须在池化和归一化之前拒绝。
  fake_session->ResetFaults();
  fake_session->non_finite_output_ = true;
  EXPECT_NE(model.Embed(inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 10. 跨批第二批失败时不得暴露第一批的部分结果。
  fake_session->ResetFaults();
  fake_session->ResetMetrics();
  fake_session->fail_on_run_ = 2;
  TextBatch multi_batch_inputs = {
      {1, 0, "hello"}, {2, 0, "hello"}, {3, 0, "hello"}};
  outputs = {{999, 999, {1.0f}}};
  EXPECT_NE(model.Embed(multi_batch_inputs, opts, &outputs), 0);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(fake_session->run_count_, 2);
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
  ASSERT_EQ(fake_fixed->observed_batch_sizes_.size(), 2u);
  EXPECT_EQ(fake_fixed->observed_batch_sizes_[0], 2u);
  EXPECT_EQ(fake_fixed->observed_batch_sizes_[1], 2u);

  // 固定 batch：单条和满批均以固定 execution_count 执行，且保留 sub_id。
  fake_fixed->ResetMetrics();
  TextBatch one_input = {{11, 7, "a"}};
  EXPECT_EQ(model_fixed.Embed(one_input, opts, &outputs), 0);
  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0].req_id, 11);
  EXPECT_EQ(outputs[0].sub_id, 7);
  ASSERT_EQ(fake_fixed->observed_batch_sizes_.size(), 1u);
  EXPECT_EQ(fake_fixed->observed_batch_sizes_[0], 2u);

  fake_fixed->ResetMetrics();
  TextBatch full_inputs = {{21, 1, "a"}, {22, 2, "b"}};
  EXPECT_EQ(model_fixed.Embed(full_inputs, opts, &outputs), 0);
  ASSERT_EQ(outputs.size(), 2u);
  ASSERT_EQ(fake_fixed->observed_batch_sizes_.size(), 1u);
  EXPECT_EQ(fake_fixed->observed_batch_sizes_[0], 2u);

  // 动态 batch：最后一批不得 padding，覆盖单条、3 条和跨批。
  auto fake_dynamic =
      std::make_shared<FakeTensorGraphSession>(4, true, /*fixed_batch=*/0, 2);
  BgeEmbeddingModel model_dynamic(fake_dynamic, tokenizer, 16, "cls", true,
                                  "last_hidden_state", 4, 3);
  EXPECT_EQ(model_dynamic.Embed(one_input, opts, &outputs), 0);
  ASSERT_EQ(fake_dynamic->observed_batch_sizes_.size(), 1u);
  EXPECT_EQ(fake_dynamic->observed_batch_sizes_[0], 1u);

  fake_dynamic->ResetMetrics();
  TextBatch dynamic_inputs = {{31, 3, "a"}, {32, 4, "b"}, {33, 5, "c"}};
  EXPECT_EQ(model_dynamic.Embed(dynamic_inputs, opts, &outputs), 0);
  ASSERT_EQ(outputs.size(), 3u);
  EXPECT_EQ(outputs[0].sub_id, 3);
  EXPECT_EQ(outputs[2].sub_id, 5);
  ASSERT_EQ(fake_dynamic->observed_batch_sizes_.size(), 2u);
  EXPECT_EQ(fake_dynamic->observed_batch_sizes_[0], 2u);
  EXPECT_EQ(fake_dynamic->observed_batch_sizes_[1], 1u);

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
#else
  EXPECT_FALSE(BackendRegistry::Instance().Find("onnxruntime").has_value());
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
// 4. ONNX Backend 中性边界与真实 Runtime fixture (R3-011, R3-013)
// =============================================================================

TEST_F(OnnxAndEmbeddingModelTest, OnnxBackendNeutralTensorValidation) {
  TensorSpec input_spec{"input_ids", ElementType::kInt64, {-1, 16}};
  BatchPolicy policy{/*max_batch_size=*/2, /*fixed_batch_size=*/0};
  TensorDesc input_desc{ElementType::kInt64, {1, 16}};
  Tensor valid_input;
  std::string diag;
  ASSERT_TRUE(CreateHostTensor(input_desc, &valid_input, &diag)) << diag;
  EXPECT_TRUE(onnxruntime_detail::ValidateInputTensor(valid_input, input_spec,
                                                      policy, &diag));

  Tensor short_input = MakeFaultTensor(input_desc, 16 * sizeof(int64_t) - 1);
  EXPECT_FALSE(onnxruntime_detail::ValidateInputTensor(short_input, input_spec,
                                                       policy, &diag));
  Tensor long_input = MakeFaultTensor(input_desc, 16 * sizeof(int64_t) + 1);
  EXPECT_FALSE(onnxruntime_detail::ValidateInputTensor(long_input, input_spec,
                                                       policy, &diag));

  TensorDesc overflow_desc{ElementType::kInt64,
                           {1, std::numeric_limits<int64_t>::max(),
                            std::numeric_limits<int64_t>::max()}};
  Tensor overflow_input = MakeFaultTensor(overflow_desc, 16);
  TensorSpec overflow_spec{"input_ids", ElementType::kInt64, {-1, -1, -1}};
  EXPECT_FALSE(onnxruntime_detail::ValidateInputTensor(
      overflow_input, overflow_spec, policy, &diag));

  TensorDesc wrong_static_desc{ElementType::kInt64, {1, 15}};
  Tensor wrong_static;
  ASSERT_TRUE(CreateHostTensor(wrong_static_desc, &wrong_static, &diag));
  EXPECT_FALSE(onnxruntime_detail::ValidateInputTensor(wrong_static, input_spec,
                                                       policy, &diag));

  TensorSpec output_spec{
      "last_hidden_state", ElementType::kFloat32, {-1, 16, 128}};
  constexpr size_t kElementCount = 1 * 16 * 128;
  EXPECT_TRUE(onnxruntime_detail::ValidateOutputMetadata(
      ElementType::kFloat32, {1, 16, 128}, kElementCount, output_spec, 1,
      &diag));
  EXPECT_FALSE(onnxruntime_detail::ValidateOutputMetadata(
      ElementType::kInt32, {1, 16, 128}, kElementCount, output_spec, 1, &diag));
  EXPECT_FALSE(onnxruntime_detail::ValidateOutputMetadata(
      ElementType::kFloat32, {1, 16}, 16, output_spec, 1, &diag));
  EXPECT_FALSE(onnxruntime_detail::ValidateOutputMetadata(
      ElementType::kFloat32, {1, 15, 128}, 1 * 15 * 128, output_spec, 1,
      &diag));
  EXPECT_FALSE(onnxruntime_detail::ValidateOutputMetadata(
      ElementType::kFloat32, {2, 16, 128}, 2 * kElementCount, output_spec, 1,
      &diag));
  EXPECT_FALSE(onnxruntime_detail::ValidateOutputMetadata(
      ElementType::kFloat32, {1, 16, 128}, kElementCount - 1, output_spec, 1,
      &diag));
  EXPECT_FALSE(onnxruntime_detail::ValidateOutputMetadata(
      ElementType::kFloat32,
      {1, std::numeric_limits<int64_t>::max(),
       std::numeric_limits<int64_t>::max()},
      0, TensorSpec{"overflow", ElementType::kFloat32, {-1, -1, -1}}, 1,
      &diag));
}

TEST_F(OnnxAndEmbeddingModelTest, OnnxBatchPolicyUsesAllTensorMetadata) {
  using onnxruntime_detail::InferBatchPolicy;

  const TensorSpec dynamic_input{"input_ids", ElementType::kInt64, {-1, 16}};
  const TensorSpec dynamic_output{
      "embeddings", ElementType::kFloat32, {-1, 384}};
  BatchPolicy policy;
  std::string diag;

  ASSERT_TRUE(
      InferBatchPolicy({dynamic_input}, {dynamic_output}, 4, &policy, &diag));
  EXPECT_EQ(policy.max_batch_size, 4u);
  EXPECT_EQ(policy.fixed_batch_size, 0u);

  const TensorSpec fixed_input{"attention_mask", ElementType::kInt64, {2, 16}};
  ASSERT_TRUE(InferBatchPolicy({dynamic_input, fixed_input}, {dynamic_output},
                               4, &policy, &diag));
  EXPECT_EQ(policy.max_batch_size, 2u);
  EXPECT_EQ(policy.fixed_batch_size, 2u);

  const TensorSpec fixed_output{"embeddings", ElementType::kFloat32, {3, 384}};
  ASSERT_TRUE(
      InferBatchPolicy({dynamic_input}, {fixed_output}, 4, &policy, &diag));
  EXPECT_EQ(policy.max_batch_size, 3u);
  EXPECT_EQ(policy.fixed_batch_size, 3u);

  EXPECT_FALSE(
      InferBatchPolicy({fixed_input}, {fixed_output}, 4, &policy, &diag));
  EXPECT_NE(diag.find("Conflicting static ONNX batch dimensions"),
            std::string::npos);

  EXPECT_FALSE(
      InferBatchPolicy({TensorSpec{"zero", ElementType::kInt64, {0, 16}}},
                       {dynamic_output}, 4, &policy, &diag));
  EXPECT_FALSE(InferBatchPolicy({TensorSpec{"scalar", ElementType::kInt64, {}}},
                                {dynamic_output}, 4, &policy, &diag));
}

TEST_F(OnnxAndEmbeddingModelTest,
       OnnxBackendRejectsUnsupportedRequestedProtocolBeforeLoading) {
  OnnxRuntimeBackend backend;
  BackendLoadSpec spec;
  spec.model_path = "./models/does-not-exist.onnx";
  spec.requested_protocol = ExecutionProtocol::kCausalLm;
  std::string diag;
  EXPECT_EQ(backend.Load(spec, &diag), nullptr);
  EXPECT_NE(diag.find("requested protocol"), std::string::npos);
}

TEST_F(OnnxAndEmbeddingModelTest, OnnxRuntimeFixturePassEvidence) {
#ifndef HAVE_ONNXRUNTIME
  GTEST_SKIP() << "ONNX Runtime not compiled into this build.";
#else
  const auto onnx_path = GetFixturePath("LLM_EDGEFLOW_TEST_BGE_ONNX",
                                        EDGEFLOW_STAGE3_ONNX_FIXTURE);
  const auto vocab_path = GetFixturePath("LLM_EDGEFLOW_TEST_BGE_VOCAB",
                                         EDGEFLOW_STAGE3_VOCAB_FIXTURE);

  if (!std::filesystem::exists(onnx_path) ||
      !std::filesystem::exists(vocab_path)) {
    GTEST_SKIP() << "ONNX/vocab fixture not found on disk.";
  }

  // 1. Backend Load 真实自省元数据
  auto backend = BackendRegistry::Instance().Create("onnxruntime");
  ASSERT_NE(backend, nullptr);

  BackendLoadSpec bspec;
  bspec.model_path = onnx_path.string();
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
  mctx.model_resource_root = vocab_path.parent_path().string();
  mctx.model_config = {
      {"tokenizer_file", vocab_path.filename().string()},
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

  // 3. 使用同一 fixture 完成 Pipeline Build 与 Execute。测试副本只替换
  // artifact 路径和序列长度，不修改生产配置。
  std::ifstream config_in("configs/pipeline_doc_qa_onnx.json");
  ASSERT_TRUE(config_in.good());
  nlohmann::json pipeline_config;
  config_in >> pipeline_config;
  pipeline_config["models"][0]["model_path"] = onnx_path.string();
  pipeline_config["models"][0]["model_config"]["tokenizer_file"] =
      vocab_path.string();
  pipeline_config["models"][0]["model_config"]["max_length"] = 32;
  // This test proves the ONNX embedding path and must not depend on an
  // external GGUF asset. Keep the same LLM node, but replace only its test
  // model registration with an explicit typed Model/Backend fixture.
  pipeline_config["models"][1] = {
      {"model_id", "llm_model_llamacpp"},
      {"capability", "llm"},
      {"model_type", "test_business_llm"},
      {"backend", "test_causal_lm_backend"},
      {"model_path", "./models/test-qwen-mock.bin"},
      {"model_config", {{"max_batch_size", 2}, {"max_seq_len", 512}}},
      {"backend_config", nlohmann::json::object()}};
  const auto smoke_config_path = temp_dir_ / "pipeline_fixture_smoke.json";
  std::ofstream config_out(smoke_config_path);
  config_out << pipeline_config.dump(2);
  config_out.close();

  Pipeline pipeline;
  PipelineDiagnostic pdiag;
  bool build_ok =
      pipeline.BuildFromConfigFile(smoke_config_path.string(), &pdiag);
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
  const auto onnx_path = GetFixturePath("LLM_EDGEFLOW_TEST_BGE_ONNX",
                                        EDGEFLOW_STAGE3_ONNX_FIXTURE);
  if (!std::filesystem::exists(onnx_path)) {
    GTEST_SKIP() << "ONNX fixture not found: " << onnx_path;
  }

  bspec.model_path = onnx_path.string();
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

  // 2.4 输入 Buffer 必须精确匹配，过短和过长都失败。
  TensorDesc valid_desc{ElementType::kInt64, {1, 16}};
  Tensor valid_tensor;
  ASSERT_TRUE(CreateHostTensor(valid_desc, &valid_tensor, &diag));
  Tensor short_tensor = MakeFaultTensor(valid_desc, 16 * sizeof(int64_t) - 1);
  inputs["input_ids"] = short_tensor;
  inputs["attention_mask"] = valid_tensor;
  EXPECT_NE(tensor_session->Run(inputs, &outputs, &diag), 0);
  EXPECT_TRUE(outputs.empty());

  Tensor long_tensor = MakeFaultTensor(valid_desc, 16 * sizeof(int64_t) + 1);
  inputs["input_ids"] = long_tensor;
  EXPECT_NE(tensor_session->Run(inputs, &outputs, &diag), 0);
  EXPECT_TRUE(outputs.empty());

  // 2.5 多输入的运行时 batch 必须一致。
  TensorDesc batch_two_desc{ElementType::kInt64, {2, 16}};
  Tensor batch_two_tensor;
  ASSERT_TRUE(CreateHostTensor(batch_two_desc, &batch_two_tensor, &diag));
  inputs["input_ids"] = valid_tensor;
  inputs["attention_mask"] = batch_two_tensor;
  EXPECT_NE(tensor_session->Run(inputs, &outputs, &diag), 0);
  EXPECT_TRUE(outputs.empty());

  // 2.6 shape 元素数乘法溢出必须在进入 ORT 前失败。
  TensorDesc overflow_desc{ElementType::kInt64,
                           {1, std::numeric_limits<int64_t>::max(),
                            std::numeric_limits<int64_t>::max()}};
  Tensor overflow_tensor = MakeFaultTensor(overflow_desc, 16);
  inputs["input_ids"] = overflow_tensor;
  inputs["attention_mask"] = overflow_tensor;
  EXPECT_NE(tensor_session->Run(inputs, &outputs, &diag), 0);
  EXPECT_TRUE(outputs.empty());
#endif
}

}  // namespace alg_framework
