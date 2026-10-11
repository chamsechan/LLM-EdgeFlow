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

#include "engine/models/bge_embedding/bge_embedding_model.h"
#include "tests/support/model_probe.h"

namespace llm_edgeflow {
namespace {

class EmbeddingGraph final : public ITensorGraphSession {
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

  const std::string backend = "neutral_embedding_graph";
  BatchPolicy policy{1, 0};
  std::vector<TensorSpec> inputs{
      {"input_ids", ElementType::kInt64, {-1, 4}},
      {"attention_mask", ElementType::kInt64, {-1, 4}}};
  std::vector<TensorSpec> outputs{
      {"last_hidden_state", ElementType::kFloat32, {-1, 4, 2}}};
  TensorMap observed;
  TensorMap response;
  int calls = 0;
  int code = 0;
  std::string reason;
};

Tensor FloatOutput(std::vector<int64_t> shape,
                   const std::vector<float>& values) {
  Tensor tensor;
  EXPECT_TRUE(
      CreateHostTensor({ElementType::kFloat32, std::move(shape)}, &tensor));
  float* data = GetMutableTensorData<float>(&tensor);
  EXPECT_NE(data, nullptr);
  if (data) std::copy(values.begin(), values.end(), data);
  return tensor;
}

std::vector<int64_t> Encoded(const TensorMap& tensors, const std::string& name,
                             size_t count) {
  const auto found = tensors.find(name);
  EXPECT_NE(found, tensors.end());
  if (found == tensors.end()) return {};
  const auto* data = GetTensorData<int64_t>(found->second);
  EXPECT_NE(data, nullptr);
  return data ? std::vector<int64_t>(data, data + count)
              : std::vector<int64_t>{};
}

class BgeEmbeddingModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/edgeflow-embedding-vocab-XXXXXX";
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
  nlohmann::json Parameters() const {
    return {{"tokenizer_file", vocab}, {"normalize", false}};
  }
  std::string vocab;
};

TEST_F(BgeEmbeddingModelTest, WordPieceMaskAndPoolingHaveLiteralResults) {
  struct Case {
    const char* pooling;
    bool normalize;
    std::vector<float> expected;
  };
  const std::vector<Case> cases = {
      {"cls", false, {1, 2}},
      {"mean", false, {3, 4}},
      {"mean", true, {0.6f, 0.8f}},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.pooling);
    SCOPED_TRACE(test.normalize);
    auto session = std::make_shared<EmbeddingGraph>();
    session->response["last_hidden_state"] =
        FloatOutput({1, 4, 2}, {1, 2, 3, 4, 5, 6, 100, 100});
    auto parameters = Parameters();
    parameters["pooling"] = test.pooling;
    parameters["normalize"] = test.normalize;
    std::string diagnostic;
    auto model = test_support::CreateModel<BgeEmbeddingModel>(
        session, parameters, &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    EmbeddingBatch output;
    ASSERT_EQ(model->Embed({{3, 5, "HELLO"}}, &output, &diagnostic), 0)
        << diagnostic;
    EXPECT_EQ(Encoded(session->observed, "input_ids", 4),
              (std::vector<int64_t>{2, 4, 3, 0}));
    EXPECT_EQ(Encoded(session->observed, "attention_mask", 4),
              (std::vector<int64_t>{1, 1, 1, 0}));
    EXPECT_EQ(session->observed.count("token_type_ids"), 0U);
    ASSERT_EQ(output.size(), 1U);
    ASSERT_EQ(output[0].data.size(), 2U);
    EXPECT_NEAR(output[0].data[0], test.expected[0], 1e-6f);
    EXPECT_NEAR(output[0].data[1], test.expected[1], 1e-6f);
    auto& returned = session->response.at("last_hidden_state");
    GetMutableTensorData<float>(&returned)[0] = 999;
    EXPECT_NEAR(output[0].data[0], test.expected[0], 1e-6f);
    EXPECT_TRUE(diagnostic.empty());
  }
}

TEST_F(BgeEmbeddingModelTest,
       ConfiguredLowercaseAndDeclaredSegmentInputAreUsed) {
  auto session = std::make_shared<EmbeddingGraph>();
  session->inputs.push_back({"token_type_ids", ElementType::kInt64, {-1, 4}});
  session->response["last_hidden_state"] =
      FloatOutput({1, 4, 2}, {1, 2, 3, 4, 5, 6, 100, 100});
  auto parameters = Parameters();
  parameters["do_lower_case"] = false;
  std::string diagnostic;
  auto model = test_support::CreateModel<BgeEmbeddingModel>(session, parameters,
                                                            &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  EmbeddingBatch output;
  ASSERT_EQ(model->Embed({{0, 0, "HELLO"}}, &output, &diagnostic), 0);
  EXPECT_EQ(Encoded(session->observed, "input_ids", 4),
            (std::vector<int64_t>{2, 1, 3, 0}));
  EXPECT_EQ(Encoded(session->observed, "token_type_ids", 4),
            (std::vector<int64_t>{0, 0, 0, 0}));
}

TEST_F(BgeEmbeddingModelTest, RankTwoSentenceOutputIsUsedDirectly) {
  auto session = std::make_shared<EmbeddingGraph>();
  session->outputs = {{"sentence", ElementType::kFloat32, {-1, 2}}};
  session->response["sentence"] = FloatOutput({1, 2}, {7, 9});
  auto parameters = Parameters();
  parameters["output_name"] = "sentence";
  parameters["pooling"] = "mean";
  std::string diagnostic;
  auto model = test_support::CreateModel<BgeEmbeddingModel>(session, parameters,
                                                            &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  EmbeddingBatch output;
  ASSERT_EQ(model->Embed({{0, 0, "hello"}}, &output, &diagnostic), 0);
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(output[0].data, (std::vector<float>{7, 9}));
}

TEST_F(BgeEmbeddingModelTest, FixedBatchDummyHasEmptyTextEncoding) {
  auto session = std::make_shared<EmbeddingGraph>();
  session->policy = {2, 2};
  session->response["last_hidden_state"] = FloatOutput(
      {2, 4, 2}, {1, 2, 3, 4, 5, 6, 100, 100, 1, 2, 3, 4, 5, 6, 100, 100});
  std::string diagnostic;
  auto model = test_support::CreateModel<BgeEmbeddingModel>(
      session, Parameters(), &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  EmbeddingBatch output;
  ASSERT_EQ(model->Embed({{0, 0, "hello"}}, &output, &diagnostic), 0);
  EXPECT_EQ(Encoded(session->observed, "input_ids", 8),
            (std::vector<int64_t>{2, 4, 3, 0, 2, 3, 0, 0}));
  EXPECT_EQ(Encoded(session->observed, "attention_mask", 8),
            (std::vector<int64_t>{1, 1, 1, 0, 1, 1, 0, 0}));
}

TEST_F(BgeEmbeddingModelTest, ModelSpecificOutputFailuresIncludeMaskedPadding) {
  struct Case {
    const char* name;
    TensorMap response;
    const char* detail;
  };
  Tensor short_buffer = FloatOutput({1, 3, 2}, {1, 2, 3, 4, 5, 6});
  short_buffer.desc.shape = {1, 4, 2};
  const std::vector<Case> cases = {
      {"sequence mismatch",
       {{"last_hidden_state", FloatOutput({1, 3, 2}, {1, 2, 3, 4, 5, 6})}},
       "sequence dimension mismatch"},
      {"dimension mismatch",
       {{"last_hidden_state", FloatOutput({1, 4, 1}, {1, 2, 3, 4})}},
       "embedding_dim mismatch"},
      {"NaN in excluded padding",
       {{"last_hidden_state",
         FloatOutput({1, 4, 2},
                     {1, 2, 3, 4, 5, 6, std::numeric_limits<float>::quiet_NaN(),
                      100})}},
       "NaN or Inf at index 6"},
      {"short buffer under valid output shape",
       {{"last_hidden_state", short_buffer}},
       "Tensor buffer byte size mismatch"},
      {"missing configured output", {}, "Expected output tensor is missing"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto session = std::make_shared<EmbeddingGraph>();
    session->response = test.response;
    auto parameters = Parameters();
    parameters["pooling"] = "mean";
    std::string diagnostic;
    auto model = test_support::CreateModel<BgeEmbeddingModel>(
        session, parameters, &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    EmbeddingBatch output;
    EXPECT_EQ(model->Embed({{0, 0, "hello"}}, &output, &diagnostic), -1);
    EXPECT_TRUE(output.empty());
    EXPECT_NE(diagnostic.find(test.detail), std::string::npos) << diagnostic;
  }
}

TEST_F(BgeEmbeddingModelTest,
       CreationResolvesMetadataAndRejectsContradictions) {
  struct Case {
    const char* name;
    std::vector<int64_t> output_shape;
    nlohmann::json overrides;
    const char* detail;
  };
  const std::vector<Case> cases = {
      {"configured sequence conflict",
       {-1, 4, 2},
       {{"max_tokens", 5}},
       "Configured 'max_tokens'"},
      {"configured dimension conflict",
       {-1, 4, 2},
       {{"embedding_dim", 3}},
       "Configured 'embedding_dim'"},
      {"output static sequence conflict",
       {-1, 3, 2},
       nlohmann::json::object(),
       "static sequence length"},
      {"dynamic dimension needs explicit value",
       {-1, 4, -1},
       nlohmann::json::object(),
       "Cannot read 'embedding_dim'"},
      {"unknown output",
       {-1, 4, 2},
       {{"output_name", "unknown"}, {"embedding_dim", 2}},
       "unknown"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto session = std::make_shared<EmbeddingGraph>();
    session->outputs[0].shape = test.output_shape;
    auto parameters = Parameters();
    parameters.update(test.overrides);
    std::string diagnostic;
    EXPECT_EQ(test_support::CreateModel<BgeEmbeddingModel>(session, parameters,
                                                           &diagnostic),
              nullptr);
    EXPECT_NE(diagnostic.find(test.detail), std::string::npos) << diagnostic;
  }
  auto session = std::make_shared<EmbeddingGraph>();
  session->outputs[0].shape = {-1, 4, -1};
  session->response["last_hidden_state"] =
      FloatOutput({1, 4, 2}, {1, 2, 3, 4, 5, 6, 100, 100});
  auto parameters = Parameters();
  parameters["embedding_dim"] = 2;
  std::string diagnostic;
  auto model = test_support::CreateModel<BgeEmbeddingModel>(session, parameters,
                                                            &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  session->code = -719;
  session->reason = "encoder failed";
  EmbeddingBatch output;
  EXPECT_EQ(model->Embed({{0, 0, "hello"}}, &output, &diagnostic), -719);
  EXPECT_EQ(diagnostic, "encoder failed");
}

}  // namespace
}  // namespace llm_edgeflow
