#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "engine/models/vision_document/vision_document_model.h"
#include "tests/support/model_probe.h"

namespace llm_edgeflow {
namespace {

class ImageSession final : public IImageTextGenerationSession {
 public:
  const std::string& BackendType() const noexcept override { return backend; }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kImageTextGeneration;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return {1, 0}; }
  int Generate(const ImageTextInput& input, const GenerateOptions& options,
               std::string* output, std::string* diagnostic) noexcept override {
    ++calls;
    observed_input = input;
    observed_options = options;
    *output = response;
    *diagnostic = reason;
    return code;
  }

  const std::string backend = "neutral_image_probe";
  int calls = 0;
  int code = 0;
  std::string reason;
  std::string response = " OCR ";
  ImageTextInput observed_input;
  GenerateOptions observed_options;
};

ImageFrame TwoPixelsWithRowPadding() {
  return {2, 1, 8, {10, 20, 30, 40, 50, 60, 99, 88}};
}

TEST(VisionDocumentModelTest,
     PatchPaddingPlanarRgbAndGreedyOptionsReachSession) {
  auto session = std::make_shared<ImageSession>();
  std::string diagnostic;
  auto model =
      test_support::CreateModel<VisionDocumentModel>(session,
                                                     {{"prompt", "Read"},
                                                      {"patch_size", 2},
                                                      {"max_pixels", 4},
                                                      {"max_tokens", 7}},
                                                     &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  OcrDocumentBatch output;
  ASSERT_EQ(model->Recognize({{3, 8, TwoPixelsWithRowPadding()}}, &output,
                             &diagnostic),
            0)
      << diagnostic;
  EXPECT_EQ(session->observed_input.prompt, "Read");
  EXPECT_EQ(session->observed_input.width, 2);
  EXPECT_EQ(session->observed_input.height, 2);
  EXPECT_EQ(session->observed_input.patch_size, 2);
  EXPECT_EQ(session->observed_input.rgb_chw,
            (std::vector<uint8_t>{10, 40, 255, 255, 20, 50, 255, 255, 30, 60,
                                  255, 255}));
  EXPECT_EQ(session->observed_options.max_tokens, 7);
  EXPECT_FLOAT_EQ(session->observed_options.temperature, 0);
  EXPECT_FLOAT_EQ(session->observed_options.top_p, 1);
  EXPECT_EQ(session->observed_options.top_k, 0);
  EXPECT_FLOAT_EQ(session->observed_options.repetition_penalty, 1);
  EXPECT_EQ(session->observed_options.random_seed, -1);
  EXPECT_TRUE(session->observed_options.system_prompt.empty());
  EXPECT_TRUE(session->observed_options.stop_words.empty());
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(output[0].data.combined_text, " OCR ");
  EXPECT_TRUE(output[0].data.boxes.empty());
  session->response = "changed";
  EXPECT_EQ(output[0].data.combined_text, " OCR ");
  EXPECT_TRUE(diagnostic.empty());
}

TEST(VisionDocumentModelTest, PixelLimitIncludesWhitePatchPadding) {
  auto session = std::make_shared<ImageSession>();
  std::string diagnostic;
  auto model = test_support::CreateModel<VisionDocumentModel>(
      session, {{"patch_size", 2}, {"max_pixels", 3}}, &diagnostic);
  ASSERT_NE(model, nullptr) << diagnostic;
  OcrDocumentBatch output;
  EXPECT_EQ(model->Recognize({{0, 0, TwoPixelsWithRowPadding()}}, &output,
                             &diagnostic),
            -1);
  EXPECT_EQ(diagnostic, "Padded image exceeds max_pixels");
  EXPECT_EQ(session->calls, 0);
  EXPECT_TRUE(output.empty());
}

TEST(VisionDocumentModelTest, MalformedRgbFramesAreRejectedBeforeGeneration) {
  struct Case {
    const char* name;
    ImageFrame frame;
  };
  const std::vector<Case> cases = {
      {"zero width", {0, 1, 0, {}}},
      {"zero height", {2, 0, 6, {}}},
      {"short stride", {2, 1, 5, {1, 2, 3, 4, 5}}},
      {"short row storage", {2, 1, 8, {1, 2, 3, 4, 5, 6, 7}}},
      {"extra row storage", {2, 1, 6, {1, 2, 3, 4, 5, 6, 7}}},
      {"original pixel limit", {3, 2, 9, std::vector<uint8_t>(18, 0)}},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto session = std::make_shared<ImageSession>();
    std::string diagnostic;
    auto model = test_support::CreateModel<VisionDocumentModel>(
        session, {{"patch_size", 2}, {"max_pixels", 4}}, &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    OcrDocumentBatch output;
    EXPECT_EQ(model->Recognize({{0, 0, test.frame}}, &output, &diagnostic), -1);
    EXPECT_EQ(session->calls, 0);
    EXPECT_EQ(diagnostic, "Invalid RGB8 frame or preprocessing limits");
    EXPECT_TRUE(output.empty());
  }
}

TEST(VisionDocumentModelTest, EmptyTextAndBackendErrorAreRecognitionFailures) {
  struct Case {
    int code;
    const char* response;
    const char* reason;
    const char* expected;
  };
  const std::vector<Case> cases = {
      {0, "", "", "Image generation returned empty text"},
      {-719, "partial", "image decoder failed", "image decoder failed"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.code);
    auto session = std::make_shared<ImageSession>();
    session->code = test.code;
    session->response = test.response;
    session->reason = test.reason;
    std::string diagnostic;
    auto model = test_support::CreateModel<VisionDocumentModel>(
        session, {{"patch_size", 2}}, &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    OcrDocumentBatch output;
    EXPECT_EQ(model->Recognize({{0, 0, TwoPixelsWithRowPadding()}}, &output,
                               &diagnostic),
              -1);
    EXPECT_EQ(diagnostic, test.expected);
    EXPECT_TRUE(output.empty());
  }
}

TEST(VisionDocumentModelTest, RecognitionPromptCannotBeEmptyOrContainNul) {
  for (const std::string& prompt : {std::string(), std::string("a\0b", 3)}) {
    auto session = std::make_shared<ImageSession>();
    std::string diagnostic;
    EXPECT_EQ(test_support::CreateModel<VisionDocumentModel>(
                  session, {{"prompt", prompt}}, &diagnostic),
              nullptr);
    EXPECT_NE(diagnostic.find("non-empty and contain no NUL"),
              std::string::npos)
        << diagnostic;
  }
}

}  // namespace
}  // namespace llm_edgeflow
