#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "core/common_contracts.h"
#include "nodes/node_error_codes.h"
#include "tests/support/node_harness.h"

namespace llm_edgeflow {
namespace {
// One fault injector for the same preserved-output contract. Algorithm-specific
// model responses stay in the component tests.
enum class ModelFault { kNone, kCount, kRequest, kSubId, kFailure };

template <typename Interface>
class ContractModel : public Interface {
 public:
  explicit ContractModel(std::string capability)
      : capability_(std::move(capability)) {}
  const std::string& ImplName() const noexcept override { return capability_; }
  const std::string& ModelType() const noexcept override { return capability_; }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  template <typename Input, typename Output>
  int Execute(const Input& input, Output* output, std::string* diagnostic) {
    ++calls;
    if (diagnostic) diagnostic->clear();
    output->clear();
    for (const auto& item : input)
      output->push_back({item.req_id, item.sub_id, {}});
    if (fault == ModelFault::kFailure) {
      if (diagnostic) *diagnostic = "model unavailable";
      return -6299;
    }
    if (!output->empty()) {
      if (fault == ModelFault::kCount) output->pop_back();
      if (fault == ModelFault::kRequest) ++output->back().req_id;
      if (fault == ModelFault::kSubId) ++output->back().sub_id;
    }
    return 0;
  }
  int calls = 0;
  ModelFault fault = ModelFault::kNone;

 private:
  std::string capability_;
};

struct AsrProbe : ContractModel<IAsrModel> {
  AsrProbe() : ContractModel("asr") {}
  bool SupportsLanguage(std::string_view) const noexcept override {
    return true;
  }
  int Transcribe(const AudioPcmBatch& input, const TranscribeOptions&,
                 TextBatch* output, std::string* diagnostic) noexcept override {
    return Execute(input, output, diagnostic);
  }
};
struct OcrProbe : ContractModel<IOcrModel> {
  OcrProbe() : ContractModel("ocr") {}
  int Recognize(const ImageFrameBatch& input, OcrDocumentBatch* output,
                std::string* diagnostic) noexcept override {
    return Execute(input, output, diagnostic);
  }
};
struct EmbeddingProbe : ContractModel<IEmbeddingModel> {
  EmbeddingProbe() : ContractModel("embedding") {}
  int Embed(const TextBatch& input, EmbeddingBatch* output,
            std::string* diagnostic) noexcept override {
    return Execute(input, output, diagnostic);
  }
};
struct LlmProbe : ContractModel<ILlmModel> {
  LlmProbe() : ContractModel("llm") {}
  int Generate(const TextBatch& input, const GenerateOptions&,
               TextBatch* output, std::string* diagnostic) noexcept override {
    return Execute(input, output, diagnostic);
  }
};

struct AsrModelContract {
  using Model = AsrProbe;
  using Input = AudioPcmBatch;
  using Output = TextBatch;
  using ExtraOutput = TextBatch;
  static constexpr const char* kNode = "asr_transcribe";
  static constexpr const char* kInput = "audio";
  static constexpr const char* kOutput = "text";
  static constexpr const char* kExtraOutput = "text";
  static nlohmann::json Config() { return {{"bind_model", "model"}}; }
  static Input Inputs() {
    return {{9, 3, AudioPcmPayload({0.1f, 0.2f}, 8000)},
            {7, 2, AudioPcmPayload({0.3f}, 16000)}};
  }
};
struct OcrModelContract {
  using Model = OcrProbe;
  using Input = ImageFrameBatch;
  using Output = OcrDocumentBatch;
  using ExtraOutput = TextBatch;
  static constexpr const char* kNode = "ocr_detect";
  static constexpr const char* kInput = "images";
  static constexpr const char* kOutput = "document";
  static constexpr const char* kExtraOutput = "text";
  static nlohmann::json Config() { return {{"bind_model", "model"}}; }
  static Input Inputs() {
    return {{9, 3, ImageFrame{1, 1, 3, {255, 0, 0}}},
            {7, 2, ImageFrame{1, 1, 3, {0, 255, 0}}}};
  }
};
struct EmbeddingModelContract {
  using Model = EmbeddingProbe;
  using Input = TextBatch;
  using Output = EmbeddingBatch;
  using ExtraOutput = EmbeddingBatch;
  static constexpr const char* kNode = "text_embedding";
  static constexpr const char* kInput = "text";
  static constexpr const char* kOutput = "embedding";
  static constexpr const char* kExtraOutput = "embedding";
  static nlohmann::json Config() { return {{"bind_model", "model"}}; }
  static Input Inputs() { return {{9, 3, "first"}, {7, 2, "second"}}; }
};
struct LlmModelContract {
  using Model = LlmProbe;
  using Input = TextBatch;
  using Output = TextBatch;
  using ExtraOutput = StructuredDocumentBatch;
  static constexpr const char* kNode = "llm_generate";
  static constexpr const char* kInput = "input";
  static constexpr const char* kOutput = "text";
  static constexpr const char* kExtraOutput = "document";
  static nlohmann::json Config() {
    return {{"bind_model", "model"},
            {"endpoints", {{"answer", nlohmann::json::object()}}}};
  }
  static Input Inputs() { return {{9, 3, "first"}, {7, 2, "second"}}; }
};

struct ModelContractNames {
  template <typename T>
  static std::string GetName(int) {
    return T::kNode;
  }
};

template <typename Case>
class NodeModelContractTest : public ::testing::Test {
 protected:
  std::shared_ptr<typename Case::Model> model_ =
      std::make_shared<typename Case::Model>();
  NodeHarness harness_{Case::kNode};
  void SetUp() override {
    harness_.Config(Case::Config()).BindModel("model", model_);
    ASSERT_TRUE(harness_.EnsureInitialized());
  }
  void ExpectFailure(const NodeHarnessResult& result, int code) {
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.init_failed());
    EXPECT_EQ(result.process_code(), code) << result.diagnostic();
    ASSERT_NE(result.Context(), nullptr);
    EXPECT_EQ(result.Context()->GetErrorCode(), code);
    EXPECT_EQ(result.template Output<typename Case::Output>(Case::kOutput),
              nullptr);
    EXPECT_EQ(
        result.template Output<typename Case::ExtraOutput>(Case::kExtraOutput),
        nullptr);
  }
};

using ModelContractCases =
    ::testing::Types<AsrModelContract, OcrModelContract, EmbeddingModelContract,
                     LlmModelContract>;
TYPED_TEST_SUITE(NodeModelContractTest, ModelContractCases, ModelContractNames);

TYPED_TEST(NodeModelContractTest, MissingOrWrongInputFailsBeforeModelCall) {
  this->ExpectFailure(this->harness_.Run(),
                      node_error::author_node::kMissingInput);
  this->harness_.CustomInput(TypeParam::kInput, Int32Batch{{9, 3, 123}});
  this->ExpectFailure(this->harness_.Run(),
                      node_error::author_node::kMissingInput);
  EXPECT_EQ(this->model_->calls, 0);
}

TYPED_TEST(NodeModelContractTest, EmptyBatchSkipsInference) {
  this->harness_.CustomInput(TypeParam::kInput, typename TypeParam::Input{});
  const auto result = this->harness_.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* output =
      result.template Output<typename TypeParam::Output>(TypeParam::kOutput);
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->empty());
  const auto* extra = result.template Output<typename TypeParam::ExtraOutput>(
      TypeParam::kExtraOutput);
  ASSERT_NE(extra, nullptr);
  EXPECT_TRUE(extra->empty());
  EXPECT_EQ(this->model_->calls, 0);
}

TYPED_TEST(NodeModelContractTest, InvalidModelOutputFailsClosed) {
  this->harness_.CustomInput(TypeParam::kInput, TypeParam::Inputs());
  for (auto fault :
       {ModelFault::kCount, ModelFault::kRequest, ModelFault::kSubId}) {
    SCOPED_TRACE(static_cast<int>(fault));
    this->model_->fault = fault;
    const int before = this->model_->calls;
    this->ExpectFailure(
        this->harness_.Run(),
        fault == ModelFault::kCount
            ? node_error::author_node::kOutputCountMismatch
            : node_error::author_node::kOutputProvenanceMismatch);
    EXPECT_EQ(this->model_->calls, before + 1);
  }
}

TYPED_TEST(NodeModelContractTest, ModelErrorPreservesDiagnosticAndAllowsRetry) {
  const auto input = TypeParam::Inputs();
  this->harness_.CustomInput(TypeParam::kInput, input);
  this->model_->fault = ModelFault::kFailure;
  const auto failure = this->harness_.Run();
  this->ExpectFailure(failure, -6299);
  EXPECT_NE(failure.diagnostic().find("model unavailable"), std::string::npos);
  this->model_->fault = ModelFault::kNone;
  const auto retry = this->harness_.Run();
  ASSERT_TRUE(retry.ok()) << retry.diagnostic();
  const auto* output =
      retry.template Output<typename TypeParam::Output>(TypeParam::kOutput);
  ASSERT_NE(output, nullptr);
  ASSERT_EQ(output->size(), input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    EXPECT_EQ((*output)[i].req_id, input[i].req_id);
    EXPECT_EQ((*output)[i].sub_id, input[i].sub_id);
  }
  EXPECT_EQ(this->model_->calls, 2);
}
}  // namespace
}  // namespace llm_edgeflow
