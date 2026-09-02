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
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "dev_support/inference/bge_model_test_support.h"
#include "engine/backend_interface.h"
#include "engine/backend_registry.h"
#include "engine/backends/onnxruntime/onnxruntime_backend.h"
#include "engine/fixed_batch_executor.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "engine/model_runtime_factory.h"
#include "engine/models/bge_embedding/bert_wordpiece_tokenizer.h"
#include "engine/models/bge_reranker/bge_reranker_model.h"

#ifndef EDGEFLOW_RERANK_ONNX_FIXTURE
#define EDGEFLOW_RERANK_ONNX_FIXTURE "models/rerank_fixture.onnx"
#endif
#ifndef EDGEFLOW_VOCAB_FIXTURE
#define EDGEFLOW_VOCAB_FIXTURE "models/vocab.txt"
#endif

namespace llm_edgeflow {

class OnnxAndRerankerModelTest : public BgeModelTestBase {};

// =============================================================================
// 1. BertWordPieceTokenizer Pair Encoding 单元测试 (S4-01)
// =============================================================================

TEST_F(OnnxAndRerankerModelTest, TokenizerPairEncodingBasicAndWordPiece) {
  BertWordPieceTokenizer tokenizer;
  std::vector<std::string> tokens = {
      "[PAD]",  "[UNK]", "[CLS]", "[SEP]", "query", "cand", "em", "##bed",
      "##ding", "北",    "京",    "大",    "学",    "!",    "?",
  };

  std::string diag;
  ASSERT_TRUE(tokenizer.LoadFromTokens(tokens, /*do_lower_case=*/true, &diag));

  // 1. 标准 pair
  std::vector<int64_t> ids, mask, types;
  EXPECT_TRUE(tokenizer.EncodePair("query embedding", "cand", 10, &ids, &mask,
                                   &types, &diag));
  ASSERT_EQ(ids.size(), 10u);
  ASSERT_EQ(mask.size(), 10u);
  ASSERT_EQ(types.size(), 10u);

  // [CLS](2), query(4), em(6), ##bed(7), ##ding(8), [SEP](3), cand(5),
  // [SEP](3), [PAD](0), [PAD](0)
  EXPECT_EQ(ids[0], 2);
  EXPECT_EQ(ids[1], 4);
  EXPECT_EQ(ids[2], 6);
  EXPECT_EQ(ids[3], 7);
  EXPECT_EQ(ids[4], 8);
  EXPECT_EQ(ids[5], 3);
  EXPECT_EQ(ids[6], 5);
  EXPECT_EQ(ids[7], 3);
  EXPECT_EQ(ids[8], 0);
  EXPECT_EQ(ids[9], 0);

  // mask: 1 for first 8, 0 for last 2
  for (size_t i = 0; i < 8; ++i) EXPECT_EQ(mask[i], 1);
  EXPECT_EQ(mask[8], 0);
  EXPECT_EQ(mask[9], 0);

  // token_type_ids: 0 for query side & first [SEP] (indices 0..5), 1 for cand &
  // second [SEP] (indices 6..7), 0 for padding
  for (size_t i = 0; i <= 5; ++i) EXPECT_EQ(types[i], 0);
  EXPECT_EQ(types[6], 1);
  EXPECT_EQ(types[7], 1);
  EXPECT_EQ(types[8], 0);
  EXPECT_EQ(types[9], 0);

  // 2. 空 pair
  EXPECT_TRUE(tokenizer.EncodePair("", "", 5, &ids, &mask, &types, &diag));
  ASSERT_EQ(ids.size(), 5u);
  // [CLS](2), [SEP](3), [SEP](3), [PAD](0), [PAD](0)
  EXPECT_EQ(ids[0], 2);
  EXPECT_EQ(ids[1], 3);
  EXPECT_EQ(ids[2], 3);
  EXPECT_EQ(ids[3], 0);
  EXPECT_EQ(ids[4], 0);
  EXPECT_EQ(types[0], 0);
  EXPECT_EQ(types[1], 0);
  EXPECT_EQ(types[2], 1);
  EXPECT_EQ(types[3], 0);
  EXPECT_EQ(types[4], 0);

  // 3. 截断机制：longest-first 优先截断较长侧，长度相同时优先截断 candidate
  EXPECT_TRUE(
      tokenizer.EncodePair("北京大学", "query", 6, &ids, &mask, &types, &diag));
  ASSERT_EQ(ids.size(), 6u);
  // [CLS](2), 北(9), 京(10), [SEP](3), query(4), [SEP](3)
  EXPECT_EQ(ids[0], 2);
  EXPECT_EQ(ids[1], 9);
  EXPECT_EQ(ids[2], 10);
  EXPECT_EQ(ids[3], 3);
  EXPECT_EQ(ids[4], 4);
  EXPECT_EQ(ids[5], 3);

  // 4. 长度相等时优先截断 candidate
  EXPECT_TRUE(
      tokenizer.EncodePair("北京", "大学", 6, &ids, &mask, &types, &diag));
  ASSERT_EQ(ids.size(), 6u);
  // [CLS](2), 北(9), 京(10), [SEP](3), 大(11), [SEP](3)
  EXPECT_EQ(ids[0], 2);
  EXPECT_EQ(ids[1], 9);
  EXPECT_EQ(ids[2], 10);
  EXPECT_EQ(ids[3], 3);
  EXPECT_EQ(ids[4], 11);
  EXPECT_EQ(ids[5], 3);

  // 5. max_length == 3
  EXPECT_TRUE(
      tokenizer.EncodePair("北京", "大学", 3, &ids, &mask, &types, &diag));
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], 2);
  EXPECT_EQ(ids[1], 3);
  EXPECT_EQ(ids[2], 3);
  EXPECT_EQ(types[0], 0);
  EXPECT_EQ(types[1], 0);
  EXPECT_EQ(types[2], 1);

  // 6. max_length < 3 必须 fail-closed
  EXPECT_FALSE(
      tokenizer.EncodePair("query", "cand", 2, &ids, &mask, &types, &diag));
  EXPECT_TRUE(ids.empty());
  EXPECT_TRUE(mask.empty());
  EXPECT_TRUE(types.empty());

  // 7. 非法 UTF-8 严格 fail-closed
  std::string bad_utf8 = "valid\xFF\xFEinvalid";
  EXPECT_FALSE(
      tokenizer.EncodePair(bad_utf8, "cand", 8, &ids, &mask, &types, &diag));
  EXPECT_TRUE(ids.empty());

  // 8. Null output pointers
  EXPECT_FALSE(
      tokenizer.EncodePair("query", "cand", 8, nullptr, &mask, &types, &diag));
}

// =============================================================================
// 2. FakeTensorGraphSession 与 BgeRerankerModel 单元与边界测试
// =============================================================================

class FakeRerankTensorGraphSession : public ITensorGraphSession {
 public:
  FakeRerankTensorGraphSession(bool is_2d = true, size_t fixed_batch = 0,
                               size_t max_batch = 4)
      : is_2d_(is_2d), policy_{max_batch, fixed_batch} {
    TensorSpec in_ids{"input_ids", ElementType::kInt64, {-1, 16}};
    TensorSpec in_mask{"attention_mask", ElementType::kInt64, {-1, 16}};
    TensorSpec in_types{"token_type_ids", ElementType::kInt64, {-1, 16}};
    inputs_ = {in_ids, in_mask, in_types};

    if (is_2d_) {
      TensorSpec out_logits{"logits", ElementType::kFloat32, {-1, 1}};
      outputs_ = {out_logits};
    } else {
      TensorSpec out_logits{"logits", ElementType::kFloat32, {-1}};
      outputs_ = {out_logits};
    }
  }

  void SetInputs(std::vector<TensorSpec> inputs) {
    inputs_ = std::move(inputs);
  }
  void SetOutputs(std::vector<TensorSpec> outputs) {
    outputs_ = std::move(outputs);
  }
  void SetBatchPolicy(BatchPolicy policy) { policy_ = policy; }

  const std::string& BackendType() const noexcept override {
    static const std::string type = "fake_ort_rerank";
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
    observed_batch_sizes_.push_back(static_cast<size_t>(batch_size));

    TensorDesc out_desc;
    out_desc.element_type = ElementType::kFloat32;
    if (corrupt_dtype_) {
      out_desc.element_type = ElementType::kInt32;
    }

    if (is_2d_) {
      out_desc.shape = {batch_size, 1};
    } else {
      out_desc.shape = {batch_size};
    }

    if (corrupt_batch_) {
      out_desc.shape[0] = batch_size + 1;
    }
    if (corrupt_second_dim_ && is_2d_) {
      out_desc.shape[1] = 2;
    }
    if (corrupt_rank_) {
      out_desc.shape = {batch_size, 1, 1};
    }
    if (corrupt_zero_dim_) {
      out_desc.shape = {0};
    }
    if (corrupt_negative_dim_) {
      out_desc.shape = {-1};
    }
    if (corrupt_overflow_) {
      out_desc.shape = {batch_size, std::numeric_limits<int64_t>::max()};
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
          if (inject_nan_) {
            data[i] = std::numeric_limits<float>::quiet_NaN();
          } else if (inject_inf_) {
            data[i] = std::numeric_limits<float>::infinity();
          } else if (inject_extreme_values_) {
            data[i] = (i % 2 == 0) ? 100.0f : -100.0f;
          } else {
            data[i] = static_cast<float>(i + 1) * 0.5f;
          }
        }
      }
    }

    (*outputs)["logits"] = std::move(out_tensor);
    return 0;
  }

  bool is_2d_ = true;
  BatchPolicy policy_;
  int run_count_ = 0;

  bool fail_run_ = false;
  bool corrupt_dtype_ = false;
  bool corrupt_batch_ = false;
  bool corrupt_second_dim_ = false;
  bool corrupt_rank_ = false;
  bool corrupt_zero_dim_ = false;
  bool corrupt_negative_dim_ = false;
  bool corrupt_overflow_ = false;
  int corrupt_byte_delta_ = 0;
  bool corrupt_misalignment_ = false;
  bool corrupt_null_data_ = false;
  bool inject_nan_ = false;
  bool inject_inf_ = false;
  bool inject_extreme_values_ = false;
  int fail_on_run_ = 0;
  InferenceConcurrency concurrency_ = InferenceConcurrency::kConcurrent;
  std::vector<size_t> observed_batch_sizes_;

  void ResetFaults() noexcept {
    fail_run_ = false;
    corrupt_dtype_ = false;
    corrupt_batch_ = false;
    corrupt_second_dim_ = false;
    corrupt_rank_ = false;
    corrupt_zero_dim_ = false;
    corrupt_negative_dim_ = false;
    corrupt_overflow_ = false;
    corrupt_byte_delta_ = 0;
    corrupt_misalignment_ = false;
    corrupt_null_data_ = false;
    inject_nan_ = false;
    inject_inf_ = false;
    inject_extreme_values_ = false;
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

TEST_F(OnnxAndRerankerModelTest, ModelSidecarSecurityAndCreation) {
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
  context.backend_session =
      std::make_shared<FakeRerankTensorGraphSession>(true);
  context.model_resource_root = model_root.string();
  context.model_config = {
      {"tokenizer_file", "vocab.txt"}, {"max_length", 16},
      {"output_name", "logits"},       {"score_activation", "sigmoid"},
      {"max_batch_size", 4},
  };

  std::string diag;
  EXPECT_NE(BgeRerankerModel::Create(context, &diag), nullptr) << diag;

  // 1. 相对路径词法逃逸拦截
  context.model_config["tokenizer_file"] = "../outside/vocab.txt";
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("cannot escape root"), std::string::npos);

  // 2. 同前缀兄弟目录逃逸拦截
  context.model_config["tokenizer_file"] = "../model-escape/vocab.txt";
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("cannot escape root"), std::string::npos);

  // 3. 非法 score_activation
  context.model_config["tokenizer_file"] = "vocab.txt";
  context.model_config["score_activation"] = "invalid_act";
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("score_activation"), std::string::npos);

  // 4. 非法 max_length
  context.model_config["score_activation"] = "sigmoid";
  context.model_config["max_length"] = 2;  // < 3
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("max_length"), std::string::npos);
}

TEST_F(OnnxAndRerankerModelTest,
       ModelCreationMetadataValidationNegativeAndPositive) {
  auto model_root = temp_dir_ / "model_meta_test";
  std::filesystem::create_directories(model_root);
  WriteTestVocab(model_root / "vocab.txt");

  auto fake_session = std::make_shared<FakeRerankTensorGraphSession>(true);

  ModelCreateContext context;
  context.backend_session = fake_session;
  context.model_resource_root = model_root.string();
  context.model_config = {
      {"tokenizer_file", "vocab.txt"}, {"max_length", 16},
      {"output_name", "logits"},       {"score_activation", "sigmoid"},
      {"max_batch_size", 4},
  };

  std::string diag;
  // 0. 基准合法配置
  EXPECT_NE(BgeRerankerModel::Create(context, &diag), nullptr) << diag;

  // 1. 空输入 metadata
  fake_session->SetInputs({});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("input metadata cannot be empty"), std::string::npos);

  // 2. 缺失必需输入 (缺少 attention_mask)
  fake_session->SetInputs({{"input_ids", ElementType::kInt64, {-1, 16}}});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("missing required inputs"), std::string::npos);

  // 3. 未知必需输入
  fake_session->SetInputs({
      {"input_ids", ElementType::kInt64, {-1, 16}},
      {"attention_mask", ElementType::kInt64, {-1, 16}},
      {"unknown_tensor", ElementType::kInt64, {-1, 16}},
  });
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("unrecognized required input"), std::string::npos);

  // 4. 输入 dtype 错误 (int64 期望, 传入 float32)
  fake_session->SetInputs({
      {"input_ids", ElementType::kFloat32, {-1, 16}},
      {"attention_mask", ElementType::kInt64, {-1, 16}},
  });
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("dtype must be int64"), std::string::npos);

  // 5. 输入 rank 错误 (2 期望, 传入 3)
  fake_session->SetInputs({
      {"input_ids", ElementType::kInt64, {-1, 16, 1}},
      {"attention_mask", ElementType::kInt64, {-1, 16}},
  });
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("rank must be 2"), std::string::npos);

  // 6. 静态 sequence 维度与 max_length 不一致
  fake_session->SetInputs({
      {"input_ids", ElementType::kInt64, {-1, 32}},
      {"attention_mask", ElementType::kInt64, {-1, 32}},
  });
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("does not match configured max_length"),
            std::string::npos);

  // 7. batch/sequence 的 0 维不是动态维度，必须拒绝
  fake_session->SetInputs({
      {"input_ids", ElementType::kInt64, {0, 16}},
      {"attention_mask", ElementType::kInt64, {-1, 16}},
  });
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("batch dimension cannot be 0"), std::string::npos);

  fake_session->SetInputs({
      {"input_ids", ElementType::kInt64, {-1, 0}},
      {"attention_mask", ElementType::kInt64, {-1, 16}},
  });
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("sequence dimension cannot be 0"), std::string::npos);

  // 8. 静态 batch 维度与 fixed_batch_size 不一致
  fake_session->SetBatchPolicy({4, 4});
  fake_session->SetInputs({
      {"input_ids", ElementType::kInt64, {2, 16}},
      {"attention_mask", ElementType::kInt64, {2, 16}},
  });
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("does not match fixed_batch_size"), std::string::npos);

  // 9. 动态策略下，静态 batch 仍不得超过 Session 最大批次
  fake_session->SetBatchPolicy({4, 0});
  fake_session->SetInputs({
      {"input_ids", ElementType::kInt64, {8, 16}},
      {"attention_mask", ElementType::kInt64, {8, 16}},
  });
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("exceeds session max_batch_size"), std::string::npos);

  // 恢复合法输入
  fake_session->SetBatchPolicy({4, 0});
  fake_session->SetInputs({
      {"input_ids", ElementType::kInt64, {-1, 16}},
      {"attention_mask", ElementType::kInt64, {-1, 16}},
      {"token_type_ids", ElementType::kInt64, {-1, 16}},
  });

  // 10. 空输出 metadata
  fake_session->SetOutputs({});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("output metadata cannot be empty"), std::string::npos);

  // 11. 缺少目标输出
  fake_session->SetOutputs({{"logits_other", ElementType::kFloat32, {-1, 1}}});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("missing expected output tensor"), std::string::npos);

  // 12. 输出 dtype 错误 (float32 期望, 传入 int32)
  fake_session->SetOutputs({{"logits", ElementType::kInt32, {-1, 1}}});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("dtype must be float32"), std::string::npos);

  // 13. 输出 rank 错误 (1 或 2 期望, 传入 3)
  fake_session->SetOutputs({{"logits", ElementType::kFloat32, {-1, 1, 1}}});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("rank must be 1 or 2"), std::string::npos);

  // 14. 输出 2D 第二维不为 1 (例如 [B, 2])
  fake_session->SetOutputs({{"logits", ElementType::kFloat32, {-1, 2}}});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("second dimension must be 1"), std::string::npos);

  // 15. 输出 batch 或第二维为 0 均必须拒绝
  fake_session->SetOutputs({{"logits", ElementType::kFloat32, {0, 1}}});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("batch dimension cannot be 0"), std::string::npos);

  fake_session->SetOutputs({{"logits", ElementType::kFloat32, {-1, 0}}});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("second dimension cannot be 0"), std::string::npos);

  // 16. 输出静态 batch 与 fixed_batch_size 不一致
  fake_session->SetBatchPolicy({4, 4});
  fake_session->SetOutputs({{"logits", ElementType::kFloat32, {2, 1}}});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("does not match fixed_batch_size"), std::string::npos);

  // 17. 动态策略下，输出静态 batch 不得超过 Session 最大批次
  fake_session->SetBatchPolicy({4, 0});
  fake_session->SetOutputs({{"logits", ElementType::kFloat32, {8, 1}}});
  diag.clear();
  EXPECT_EQ(BgeRerankerModel::Create(context, &diag), nullptr);
  EXPECT_NE(diag.find("exceeds session max_batch_size"), std::string::npos);

  // 18. 正例：动态 Shape [-1, -1] 与 1D logits [-1]
  fake_session->SetBatchPolicy({4, 0});
  fake_session->SetInputs({
      {"input_ids", ElementType::kInt64, {-1, -1}},
      {"attention_mask", ElementType::kInt64, {-1, -1}},
  });
  fake_session->SetOutputs({{"logits", ElementType::kFloat32, {-1}}});
  diag.clear();
  EXPECT_NE(BgeRerankerModel::Create(context, &diag), nullptr) << diag;

  // 19. 正例：合法静态 Shape [4, 16] 与 2D logits [4, 1]
  fake_session->SetBatchPolicy({4, 4});
  fake_session->SetInputs({
      {"input_ids", ElementType::kInt64, {4, 16}},
      {"attention_mask", ElementType::kInt64, {4, 16}},
  });
  fake_session->SetOutputs({{"logits", ElementType::kFloat32, {4, 1}}});
  diag.clear();
  EXPECT_NE(BgeRerankerModel::Create(context, &diag), nullptr) << diag;
}

TEST_F(OnnxAndRerankerModelTest, ModelScoringActivationAndNumericalStability) {
  auto fake_session_2d = std::make_shared<FakeRerankTensorGraphSession>(true);
  BertWordPieceTokenizer tokenizer;
  std::vector<std::string> tokens = {"[PAD]", "[UNK]", "[CLS]", "[SEP]",
                                     "hello", "world", "bge",   "model"};
  ASSERT_TRUE(tokenizer.LoadFromTokens(tokens, true));

  // 1. Sigmoid 激活与极值数值稳定性
  BgeRerankerModel model_sigmoid(fake_session_2d, tokenizer, 16, "logits",
                                 "sigmoid", 4);
  QueryCandidatesBatch inputs = {
      {1001, 0, QueryCandidatePair("hello", "world")},
      {1002, 1, QueryCandidatePair("bge", "model")},
  };
  ScoreBatch outputs;

  fake_session_2d->inject_extreme_values_ = true;
  EXPECT_EQ(model_sigmoid.Score(inputs, &outputs), 0);
  ASSERT_EQ(outputs.size(), 2u);
  EXPECT_EQ(outputs[0].req_id, 1001);
  EXPECT_EQ(outputs[0].sub_id, 0);
  EXPECT_EQ(outputs[1].req_id, 1002);
  EXPECT_EQ(outputs[1].sub_id, 1);
  EXPECT_NEAR(outputs[0].data, 1.0f, 1e-4f);
  EXPECT_NEAR(outputs[1].data, 0.0f, 1e-4f);

  // 2. Identity 激活
  fake_session_2d->inject_extreme_values_ = false;
  BgeRerankerModel model_identity(fake_session_2d, tokenizer, 16, "logits",
                                  "identity", 4);
  EXPECT_EQ(model_identity.Score(inputs, &outputs), 0);
  ASSERT_EQ(outputs.size(), 2u);
  EXPECT_FLOAT_EQ(outputs[0].data, 0.5f);
  EXPECT_FLOAT_EQ(outputs[1].data, 1.0f);

  // 3. 1D 结构 [batch]
  auto fake_session_1d = std::make_shared<FakeRerankTensorGraphSession>(false);
  BgeRerankerModel model_1d(fake_session_1d, tokenizer, 16, "logits", "sigmoid",
                            4);
  EXPECT_EQ(model_1d.Score(inputs, &outputs), 0);
  ASSERT_EQ(outputs.size(), 2u);
}

TEST_F(OnnxAndRerankerModelTest, BgeRerankerConcurrencyReflectsModelSemantics) {
  auto session = std::make_shared<FakeRerankTensorGraphSession>(true);
  session->concurrency_ = InferenceConcurrency::kSerialized;
  BertWordPieceTokenizer tokenizer;
  ASSERT_TRUE(
      tokenizer.LoadFromTokens({"[PAD]", "[UNK]", "[CLS]", "[SEP]"}, true));

  BgeRerankerModel model(session, std::move(tokenizer), 16, "logits", "sigmoid",
                         4);
  EXPECT_EQ(model.Concurrency(), InferenceConcurrency::kConcurrent);
}

TEST_F(OnnxAndRerankerModelTest, ModelStrictTensorBoundaryFailures) {
  auto fake_session = std::make_shared<FakeRerankTensorGraphSession>(true);
  BertWordPieceTokenizer tokenizer;
  std::vector<std::string> tokens = {"[PAD]", "[UNK]", "[CLS]", "[SEP]",
                                     "hello"};
  ASSERT_TRUE(tokenizer.LoadFromTokens(tokens, true));

  BgeRerankerModel model(fake_session, tokenizer, 16, "logits", "sigmoid", 4);
  QueryCandidatesBatch inputs = {{1, 0, QueryCandidatePair("hello", "hello")}};
  ScoreBatch outputs;

  // 1. Session Run 失败
  fake_session->fail_run_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 2. Dtype 错误
  fake_session->fail_run_ = false;
  fake_session->corrupt_dtype_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 3. Batch 不匹配
  fake_session->corrupt_dtype_ = false;
  fake_session->corrupt_batch_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 4. 第二维不为 1
  fake_session->corrupt_batch_ = false;
  fake_session->corrupt_second_dim_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 5. Rank > 2
  fake_session->corrupt_second_dim_ = false;
  fake_session->corrupt_rank_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 6. Zero Dim
  fake_session->corrupt_rank_ = false;
  fake_session->corrupt_zero_dim_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 7. NaN / Inf 拒绝
  fake_session->ResetFaults();
  fake_session->inject_nan_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->inject_inf_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 8. 负维度、溢出、过短/过长 Buffer、错位和空数据全部 fail-closed
  fake_session->ResetFaults();
  fake_session->corrupt_negative_dim_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_overflow_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_byte_delta_ = -1;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_byte_delta_ = 1;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_misalignment_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  fake_session->ResetFaults();
  fake_session->corrupt_null_data_ = true;
  EXPECT_NE(model.Score(inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());

  // 9. 跨批第二批失败时不得暴露第一批的部分结果
  fake_session->ResetFaults();
  fake_session->ResetMetrics();
  fake_session->fail_on_run_ = 2;
  QueryCandidatesBatch multi_inputs = {
      {1, 0, QueryCandidatePair("a", "b")},
      {2, 0, QueryCandidatePair("a", "b")},
      {3, 0, QueryCandidatePair("a", "b")},
  };
  outputs = {{999, 999, 1.0f}};
  BgeRerankerModel small_batch_model(fake_session, tokenizer, 16, "logits",
                                     "sigmoid", 2);
  EXPECT_NE(small_batch_model.Score(multi_inputs, &outputs), 0);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(fake_session->run_count_, 2);
}

TEST_F(OnnxAndRerankerModelTest, FixedAndDynamicBatchScheduling) {
  // 1. 固定 Batch = 2
  auto fake_fixed = std::make_shared<FakeRerankTensorGraphSession>(
      true, /*fixed_batch=*/2, 2);
  BertWordPieceTokenizer tokenizer;
  std::vector<std::string> tokens = {"[PAD]", "[UNK]", "[CLS]", "[SEP]",
                                     "a",     "b",     "c"};
  ASSERT_TRUE(tokenizer.LoadFromTokens(tokens, true));

  BgeRerankerModel model_fixed(fake_fixed, tokenizer, 16, "logits", "sigmoid",
                               2);

  // 输入 3 条样本 -> 切分为 2 个批次 (每批执行 2 条，第 2 批自动 pad 1
  // 条并剥离)
  QueryCandidatesBatch inputs = {
      {1, 10, QueryCandidatePair("a", "b")},
      {2, 20, QueryCandidatePair("a", "c")},
      {3, 30, QueryCandidatePair("b", "c")},
  };
  ScoreBatch outputs;

  EXPECT_EQ(model_fixed.Score(inputs, &outputs), 0);
  ASSERT_EQ(outputs.size(), 3u);
  EXPECT_EQ(outputs[0].req_id, 1);
  EXPECT_EQ(outputs[0].sub_id, 10);
  EXPECT_EQ(outputs[1].req_id, 2);
  EXPECT_EQ(outputs[1].sub_id, 20);
  EXPECT_EQ(outputs[2].req_id, 3);
  EXPECT_EQ(outputs[2].sub_id, 30);
  EXPECT_EQ(fake_fixed->run_count_, 2);
  ASSERT_EQ(fake_fixed->observed_batch_sizes_.size(), 2u);
  EXPECT_EQ(fake_fixed->observed_batch_sizes_[0], 2u);
  EXPECT_EQ(fake_fixed->observed_batch_sizes_[1], 2u);

  // 2. 动态 batch：最后一批不得 padding
  auto fake_dynamic = std::make_shared<FakeRerankTensorGraphSession>(
      true, /*fixed_batch=*/0, 2);
  BgeRerankerModel model_dynamic(fake_dynamic, tokenizer, 16, "logits",
                                 "sigmoid", 3);
  fake_dynamic->ResetMetrics();
  EXPECT_EQ(model_dynamic.Score(inputs, &outputs), 0);
  ASSERT_EQ(outputs.size(), 3u);
  ASSERT_EQ(fake_dynamic->observed_batch_sizes_.size(), 2u);
  EXPECT_EQ(fake_dynamic->observed_batch_sizes_[0], 2u);
  EXPECT_EQ(fake_dynamic->observed_batch_sizes_[1], 1u);

  // 3. 模型语义上限冲突拒绝创建
  ModelCreateContext mctx;
  mctx.backend_session = fake_fixed;
  mctx.model_resource_root = temp_dir_.string();
  auto vocab_path = temp_dir_ / "vocab.txt";
  std::ofstream out(vocab_path);
  out << "[PAD]\n[UNK]\n[CLS]\n[SEP]\nword\n";
  out.close();

  mctx.model_config = {
      {"tokenizer_file", "vocab.txt"},
      {"max_batch_size", 1},  // 小于 fixed_batch_size (2) -> 必须明确拒绝
  };
  std::string diag;
  auto rejected_model =
      ModelRegistry::Instance().Create("bge_reranker", mctx, &diag);
  EXPECT_EQ(rejected_model, nullptr);
  EXPECT_TRUE(diag.find("cannot be smaller than Session fixed_batch_size") !=
              std::string::npos);
}

// =============================================================================
// 3. Catalog 注册自省测试
// =============================================================================

TEST_F(OnnxAndRerankerModelTest, CatalogRegistrations) {
  auto mdef_opt = ModelRegistry::Instance().Find("bge_reranker");
  ASSERT_TRUE(mdef_opt.has_value());
  EXPECT_EQ(mdef_opt->model_type, "bge_reranker");
  EXPECT_EQ(mdef_opt->capability, "rerank");
  EXPECT_EQ(mdef_opt->required_protocol, ExecutionProtocol::kTensorGraph);
  EXPECT_EQ(mdef_opt->concurrency, InferenceConcurrency::kConcurrent);

  // 确认旧组合型名称未被伪装成 Model 注册。
  EXPECT_FALSE(PipelineCatalog::FindModel("onnx_rerank").has_value());
}

// =============================================================================
// 4. 真实 ONNX Runtime Fixture 端到端测试
// =============================================================================

#ifdef HAVE_ONNXRUNTIME

TEST_F(OnnxAndRerankerModelTest, RealOnnxRuntimeRerankFixtureExecution) {
  auto onnx_path = GetFixturePath("LLM_EDGEFLOW_TEST_RERANK_ONNX",
                                  EDGEFLOW_RERANK_ONNX_FIXTURE);
  auto vocab_path =
      GetFixturePath("LLM_EDGEFLOW_TEST_RERANK_VOCAB", EDGEFLOW_VOCAB_FIXTURE);

  std::error_code ec;
  ASSERT_TRUE(std::filesystem::exists(onnx_path, ec))
      << "Missing rerank ONNX fixture: " << onnx_path;
  ASSERT_TRUE(std::filesystem::exists(vocab_path, ec))
      << "Missing vocab fixture: " << vocab_path;

  // 1. 创建 Backend
  auto backend = BackendRegistry::Instance().Create("onnxruntime");
  ASSERT_NE(backend, nullptr);

  BackendLoadSpec bspec;
  bspec.model_path = onnx_path.string();
  bspec.backend_config = {
      {"max_batch_size", 4},
      {"intra_op_num_threads", 1},
      {"inter_op_num_threads", 1},
  };
  std::string diag;
  auto session = backend->Load(bspec, &diag);
  ASSERT_NE(session, nullptr) << diag;

  auto tensor_session = std::dynamic_pointer_cast<ITensorGraphSession>(session);
  ASSERT_NE(tensor_session, nullptr);

  // 2. 创建 Model
  ModelCreateContext mctx;
  mctx.backend_session = tensor_session;
  mctx.model_resource_root = vocab_path.parent_path().string();
  mctx.model_config = {
      {"tokenizer_file", vocab_path.filename().string()},
      {"do_lower_case", true},
      {"max_length", 32},
      {"output_name", "logits"},
      {"score_activation", "sigmoid"},
      {"max_batch_size", 4},
  };
  auto model = ModelRegistry::Instance().Create("bge_reranker", mctx, &diag);
  ASSERT_NE(model, nullptr) << diag;

  auto rerank_model = std::dynamic_pointer_cast<IRerankModel>(model);
  ASSERT_NE(rerank_model, nullptr);

  // 3. 执行真实打分
  QueryCandidatesBatch inputs = {
      {101, 1, QueryCandidatePair("hello world", "bge embedding test")},
      {101, 2, QueryCandidatePair("hello world", "北京大学 智能问答系统")},
      {102, 1, QueryCandidatePair("hello world", "bge embedding test")},
  };
  ScoreBatch outputs;
  int ret = rerank_model->Score(inputs, &outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 3u);

  EXPECT_EQ(outputs[0].req_id, 101u);
  EXPECT_EQ(outputs[0].sub_id, 1u);
  EXPECT_EQ(outputs[1].req_id, 101u);
  EXPECT_EQ(outputs[1].sub_id, 2u);
  EXPECT_EQ(outputs[2].req_id, 102u);
  EXPECT_EQ(outputs[2].sub_id, 1u);

  // 相同输入必须获得相同打分
  EXPECT_FLOAT_EQ(outputs[0].data, outputs[2].data);
  // 不同 candidate 必须获得不同有限分值
  EXPECT_NE(outputs[0].data, outputs[1].data);
  EXPECT_TRUE(std::isfinite(outputs[0].data));
  EXPECT_TRUE(std::isfinite(outputs[1].data));
  EXPECT_GE(outputs[0].data, 0.0f);
  EXPECT_LE(outputs[0].data, 1.0f);
}

TEST_F(OnnxAndRerankerModelTest, RealPipelineBuildAndExecuteSmoke) {
  auto onnx_path = GetFixturePath("LLM_EDGEFLOW_TEST_RERANK_ONNX",
                                  EDGEFLOW_RERANK_ONNX_FIXTURE);
  auto vocab_path =
      GetFixturePath("LLM_EDGEFLOW_TEST_RERANK_VOCAB", EDGEFLOW_VOCAB_FIXTURE);

  std::error_code ec;
  ASSERT_TRUE(std::filesystem::exists(onnx_path, ec))
      << "Missing fixture: " << onnx_path;
  ASSERT_TRUE(std::filesystem::exists(vocab_path, ec))
      << "Missing fixture: " << vocab_path;

  // 1. 读取生产 pipeline_cross_rerank.json 配置模板
  std::ifstream cfg_in("configs/pipeline_cross_rerank.json");
  ASSERT_TRUE(cfg_in.good())
      << "Failed to open configs/pipeline_cross_rerank.json";
  nlohmann::json pipe_json;
  cfg_in >> pipe_json;
  cfg_in.close();

  // 2. 注入真实构建期 fixture 路径和测试参数 (top_k=2)
  pipe_json["models"][0]["model_path"] = onnx_path.string();
  pipe_json["models"][0]["model_config"]["tokenizer_file"] =
      vocab_path.string();
  pipe_json["models"][0]["model_config"]["max_length"] = 32;
  pipe_json["pipeline"][0]["config"]["top_k"] = 2;

  auto smoke_cfg_path = temp_dir_ / "pipeline_cross_rerank_smoke.json";
  std::ofstream cfg_out(smoke_cfg_path);
  cfg_out << pipe_json.dump(2);
  cfg_out.close();

  // 3. PipelineValidator Validate/Plan
  auto planned_plan = PipelineValidator::ValidateAndPlan(
      pipe_json, ValidationPolicy::kPrivateExtensionCompatible);
  ASSERT_TRUE(planned_plan.report.ok) << planned_plan.report.ToJson().dump();
  ASSERT_EQ(planned_plan.topological_order.size(), 1u);
  EXPECT_EQ(planned_plan.topological_order[0], "node_0_TextRerankNode");

  // 4. Pipeline Build
  Pipeline pipeline;
  PipelineDiagnostic build_diag;
  bool build_ok = pipeline.BuildFromConfigFile(
      smoke_cfg_path.string(), &build_diag,
      ValidationPolicy::kPrivateExtensionCompatible);
  ASSERT_TRUE(build_ok) << build_diag.message << " at " << build_diag.path;
  EXPECT_TRUE(pipeline.IsReady());

  // 5. 构造 AlgContext 输入 (多 Request 批量候选)
  AlgContext ctx;
  TextBatch queries = {
      {1001, 0, "hello world"},
      {1002, 0, "edgeflow test"},
  };
  RankedTextBatch candidates = {
      {1001, 0, RankedCandidate("北京大学 智能问答系统", 0.0f)},
      {1001, 1, RankedCandidate("bge embedding test", 0.0f)},
      {1001, 2, RankedCandidate("hello world exact match", 0.0f)},
      {1002, 0, RankedCandidate("edgeflow high performance pipeline", 0.0f)},
      {1002, 1, RankedCandidate("unrelated candidate document", 0.0f)},
  };
  ctx.Publish("rerank_queries", queries);
  ctx.Publish("rerank_candidates", candidates);

  // 6. Pipeline Execute
  int exec_ret = pipeline.Execute(&ctx);
  EXPECT_EQ(exec_ret, 0);

  // 7. 校验 RankedTextBatch 输出
  const auto* ranked = ctx.Read<RankedTextBatch>("ranked_results");
  ASSERT_NE(ranked, nullptr);
  // top_k=2，每个 request 截取 top 2，总共 4 条
  ASSERT_EQ(ranked->size(), 4u);

  // 校验 Provenance (req_id, sub_id)
  EXPECT_EQ((*ranked)[0].req_id, 1001u);
  EXPECT_EQ((*ranked)[0].sub_id, 0u);
  EXPECT_EQ((*ranked)[1].req_id, 1001u);
  EXPECT_EQ((*ranked)[1].sub_id, 1u);

  EXPECT_EQ((*ranked)[2].req_id, 1002u);
  EXPECT_EQ((*ranked)[2].sub_id, 0u);
  EXPECT_EQ((*ranked)[3].req_id, 1002u);
  EXPECT_EQ((*ranked)[3].sub_id, 1u);

  // 校验打分有效性与降序排列
  EXPECT_TRUE(std::isfinite((*ranked)[0].data.score));
  EXPECT_TRUE(std::isfinite((*ranked)[1].data.score));
  EXPECT_TRUE(std::isfinite((*ranked)[2].data.score));
  EXPECT_TRUE(std::isfinite((*ranked)[3].data.score));

  EXPECT_GE((*ranked)[0].data.score, (*ranked)[1].data.score);
  EXPECT_GE((*ranked)[2].data.score, (*ranked)[3].data.score);
}

#endif  // HAVE_ONNXRUNTIME

}  // namespace llm_edgeflow
