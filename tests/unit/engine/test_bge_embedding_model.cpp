#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/models/bge_embedding/bge_embedding_model.h"
#include "tests/support/model_probe.h"
#include "tests/support/tensor_graph_probe.h"

namespace llm_edgeflow {
namespace {

using test_support::Encoded;
using test_support::FloatOutput;

class EmbeddingGraph final : public test_support::TensorGraphProbe {
 public:
  EmbeddingGraph() : TensorGraphProbe("neutral_embedding_graph") {
    policy = {1, 0};
    inputs = {{"input_ids", ElementType::kInt64, {-1, 4}},
              {"attention_mask", ElementType::kInt64, {-1, 4}}};
    outputs = {{"last_hidden_state", ElementType::kFloat32, {-1, 4, 2}}};
  }
};

class BgeEmbeddingModelTest : public test_support::WordPieceVocabularyTest {
 protected:
  nlohmann::json Parameters() const {
    return {{"tokenizer_file", vocab}, {"normalize", false}};
  }
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
