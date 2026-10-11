#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/models/bge_reranker/bge_reranker_model.h"
#include "tests/support/model_probe.h"

namespace llm_edgeflow {
namespace {

class RerankGraph final : public ITensorGraphSession {
 public:
  const std::string& BackendType() const noexcept override { return backend; }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kTensorGraph;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return policy; }
  const std::vector<TensorSpec>& Inputs() const noexcept override {
    return inputs;
  }
  const std::vector<TensorSpec>& Outputs() const noexcept override {
    return outputs;
  }
  int Run(const TensorMap& input, TensorMap* output,
          std::string* diagnostic) noexcept override {
    ++calls;
    observed = input;
    *output = response;
    *diagnostic = reason;
    return code;
  }

  const std::string backend = "neutral_rerank_graph";
  BatchPolicy policy{3, 0};
  std::vector<TensorSpec> inputs{
      {"input_ids", ElementType::kInt64, {-1, 6}},
      {"attention_mask", ElementType::kInt64, {-1, 6}},
      {"token_type_ids", ElementType::kInt64, {-1, 6}}};
  std::vector<TensorSpec> outputs{{"logits", ElementType::kFloat32, {-1, 1}}};
  TensorMap observed;
  TensorMap response;
  int calls = 0;
  int code = 0;
  std::string reason;
};

Tensor Logits(std::vector<int64_t> shape, const std::vector<float>& values) {
  Tensor tensor;
  EXPECT_TRUE(
      CreateHostTensor({ElementType::kFloat32, std::move(shape)}, &tensor));
  float* data = GetMutableTensorData<float>(&tensor);
  EXPECT_NE(data, nullptr);
  if (data) std::copy(values.begin(), values.end(), data);
  return tensor;
}

std::vector<int64_t> EncodedPair(const TensorMap& tensors,
                                 const std::string& name, size_t count) {
  const auto found = tensors.find(name);
  EXPECT_NE(found, tensors.end());
  if (found == tensors.end()) return {};
  const auto* data = GetTensorData<int64_t>(found->second);
  EXPECT_NE(data, nullptr);
  return data ? std::vector<int64_t>(data, data + count)
              : std::vector<int64_t>{};
}

class BgeRerankerModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/edgeflow-reranker-vocab-XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    close(fd);
    vocab = path;
    std::ofstream file(vocab);
    file << "[PAD]\n[UNK]\n[CLS]\n[SEP]\nhello\nworld\n##s\n";
    ASSERT_TRUE(file.good());
  }
  void TearDown() override {
    if (!vocab.empty()) std::remove(vocab.c_str());
  }
  nlohmann::json Parameters() const { return {{"tokenizer_file", vocab}}; }
  std::string vocab;
};

TEST_F(BgeRerankerModelTest,
       PairEncodingAndSigmoidOrIdentityUseActualParameters) {
  const std::vector<float> raw = {0, 1.0986122886681098f, -1.0986122886681098f};
  for (const std::string& activation :
       {std::string("sigmoid"), std::string("identity")}) {
    SCOPED_TRACE(activation);
    auto session = std::make_shared<RerankGraph>();
    session->outputs[0].name = "scores";
    session->response["scores"] = Logits({3, 1}, raw);
    auto parameters = Parameters();
    parameters["score_activation"] = activation;
    parameters["output_name"] = "scores";
    std::string diagnostic;
    auto model = test_support::CreateModel<BgeRerankerModel>(
        session, parameters, &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    ScoreBatch output;
    const QueryCandidatesBatch input = {
        {0, 0, {"HELLO", "world"}},
        {0, 1, {"hello", "world"}},
        {0, 2, {"hello", "world"}},
    };
    ASSERT_EQ(model->Score(input, &output, &diagnostic), 0) << diagnostic;
    EXPECT_EQ(EncodedPair(session->observed, "input_ids", 18),
              (std::vector<int64_t>{2, 4, 3, 5, 3, 0, 2, 4, 3, 5, 3, 0, 2, 4, 3,
                                    5, 3, 0}));
    EXPECT_EQ(EncodedPair(session->observed, "attention_mask", 18),
              (std::vector<int64_t>{1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1,
                                    1, 1, 0}));
    EXPECT_EQ(EncodedPair(session->observed, "token_type_ids", 18),
              (std::vector<int64_t>{0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0,
                                    1, 1, 0}));
    ASSERT_EQ(output.size(), 3U);
    const std::vector<float> expected =
        activation == "sigmoid" ? std::vector<float>{0.5f, 0.75f, 0.25f} : raw;
    for (size_t i = 0; i < expected.size(); ++i) {
      EXPECT_NEAR(output[i].data, expected[i], 1e-6f);
    }
    auto& tensor = session->response.at("scores");
    GetMutableTensorData<float>(&tensor)[0] = 999;
    EXPECT_NEAR(output[0].data, expected[0], 1e-6f);
    EXPECT_TRUE(diagnostic.empty());
  }
}

TEST_F(BgeRerankerModelTest, TruncationRemovesLongestSideAndCandidateOnTies) {
  struct Case {
    const char* name;
    size_t length;
    QueryCandidatePair pair;
    std::vector<int64_t> ids;
    std::vector<int64_t> types;
  };
  const std::vector<Case> cases = {
      {"longer query",
       5,
       {"hello hello", "world"},
       {2, 4, 3, 5, 3},
       {0, 0, 0, 1, 1}},
      {"longer candidate",
       5,
       {"hello", "world world"},
       {2, 4, 3, 5, 3},
       {0, 0, 0, 1, 1}},
      {"equal lengths remove candidate first",
       6,
       {"hello hello", "world world"},
       {2, 4, 4, 3, 5, 3},
       {0, 0, 0, 0, 1, 1}},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto session = std::make_shared<RerankGraph>();
    for (auto& input : session->inputs) input.shape[1] = test.length;
    session->response["logits"] = Logits({1}, {0});
    auto parameters = Parameters();
    parameters["max_tokens"] = test.length;
    std::string diagnostic;
    auto model = test_support::CreateModel<BgeRerankerModel>(
        session, parameters, &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    ScoreBatch output;
    ASSERT_EQ(model->Score({{0, 0, test.pair}}, &output, &diagnostic), 0);
    EXPECT_EQ(EncodedPair(session->observed, "input_ids", test.length),
              test.ids);
    EXPECT_EQ(EncodedPair(session->observed, "token_type_ids", test.length),
              test.types);
    EXPECT_EQ(EncodedPair(session->observed, "attention_mask", test.length),
              std::vector<int64_t>(test.length, 1));
  }
}

TEST_F(BgeRerankerModelTest,
       FixedBatchDummyContainsBothSeparatorsAndSegmentOne) {
  auto session = std::make_shared<RerankGraph>();
  session->policy = {2, 2};
  session->response["logits"] = Logits({2, 1}, {0, 0});
  std::string diagnostic;
  auto model = test_support::CreateModel<BgeRerankerModel>(
      session, Parameters(), &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  ScoreBatch output;
  ASSERT_EQ(model->Score({{0, 0, {"hello", "world"}}}, &output, &diagnostic),
            0);
  EXPECT_EQ(EncodedPair(session->observed, "input_ids", 12),
            (std::vector<int64_t>{2, 4, 3, 5, 3, 0, 2, 3, 3, 0, 0, 0}));
  EXPECT_EQ(EncodedPair(session->observed, "attention_mask", 12),
            (std::vector<int64_t>{1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0}));
  EXPECT_EQ(EncodedPair(session->observed, "token_type_ids", 12),
            (std::vector<int64_t>{0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0}));
}

TEST_F(BgeRerankerModelTest, ScoresMustHaveOneColumnAndBeFinite) {
  struct Case {
    const char* name;
    TensorMap response;
    const char* detail;
    QueryCandidatesBatch input{{0, 0, {"hello", "world"}}};
  };
  Tensor short_buffer = Logits({1, 1}, {0});
  short_buffer.desc.shape = {2, 1};
  const std::vector<Case> cases = {
      {"two score columns",
       {{"logits", Logits({1, 2}, {0, 1})}},
       "second dimension must be 1"},
      {"NaN",
       {{"logits", Logits({1}, {std::numeric_limits<float>::quiet_NaN()})}},
       "score is NaN or Inf"},
      {"infinity",
       {{"logits", Logits({1}, {std::numeric_limits<float>::infinity()})}},
       "score is NaN or Inf"},
      {"short buffer under valid two-score shape",
       {{"logits", short_buffer}},
       "Tensor buffer byte size mismatch",
       {{0, 0, {"hello", "world"}}, {0, 1, {"hello", "world"}}}},
      {"missing logits", {}, "Expected output tensor is missing"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto session = std::make_shared<RerankGraph>();
    session->response = test.response;
    std::string diagnostic;
    auto model = test_support::CreateModel<BgeRerankerModel>(
        session, Parameters(), &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    ScoreBatch output;
    EXPECT_EQ(model->Score(test.input, &output, &diagnostic), -1);
    EXPECT_TRUE(output.empty());
    EXPECT_NE(diagnostic.find(test.detail), std::string::npos) << diagnostic;
  }
}

TEST_F(BgeRerankerModelTest,
       MetadataMustLeaveThreeSpecialTokensAndOneScoreColumn) {
  auto session = std::make_shared<RerankGraph>();
  for (auto& input : session->inputs) input.shape[1] = 2;
  std::string diagnostic;
  EXPECT_EQ(test_support::CreateModel<BgeRerankerModel>(session, Parameters(),
                                                        &diagnostic),
            nullptr);
  EXPECT_EQ(diagnostic, "Model max_tokens must be in [3, 4096]");
  session = std::make_shared<RerankGraph>();
  session->outputs[0].shape = {-1, 2};
  EXPECT_EQ(test_support::CreateModel<BgeRerankerModel>(session, Parameters(),
                                                        &diagnostic),
            nullptr);
  EXPECT_NE(diagnostic.find("second dimension must be 1"), std::string::npos);
}

TEST_F(BgeRerankerModelTest, SessionFailurePreservesCodeAndDetail) {
  auto session = std::make_shared<RerankGraph>();
  session->code = -719;
  session->reason = "cross encoder failed";
  std::string diagnostic;
  auto model = test_support::CreateModel<BgeRerankerModel>(
      session, Parameters(), &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  ScoreBatch output;
  EXPECT_EQ(model->Score({{0, 0, {"hello", "world"}}}, &output, &diagnostic),
            -719);
  EXPECT_EQ(diagnostic, "cross encoder failed");
  EXPECT_TRUE(output.empty());
}

}  // namespace
}  // namespace llm_edgeflow
