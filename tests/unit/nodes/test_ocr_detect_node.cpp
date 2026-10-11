#include <gtest/gtest.h>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "engine/model_interface.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {
class RecordingOcr final : public IOcrModel {
 public:
  const std::string& ImplName() const noexcept override { return impl; }
  const std::string& ModelType() const noexcept override { return type; }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  int Recognize(const ImageFrameBatch& images, OcrDocumentBatch* output,
                std::string* diagnostic) noexcept override {
    if (diagnostic) diagnostic->clear();
    calls.push_back(images);
    output->clear();
    if (fail) {
      if (diagnostic) *diagnostic = "image recognition failed";
      return -17;
    }
    *output = response;
    return 0;
  }
  std::string impl = "node_recording_ocr", type = "ocr";
  bool fail = false;
  OcrDocumentBatch response;
  std::vector<ImageFrameBatch> calls;
};

TEST(OcrDetectNodeTest,
     PublishesWholeDocumentsAndTheirCombinedTextFromTheSameCall) {
  SessionContext session;
  auto model = std::make_shared<RecordingOcr>();
  model->response = {
      {9, 4,
       OcrDocumentItem{{{1, 2, 3, 4, "A", 0.9f}, {5, 6, 7, 8, "猫", 0.8f}},
                       "A\n猫"}},
      {2, 7, OcrDocumentItem{{}, "empty boxes"}}};
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "vision", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init("ocr_detect", {{"bind_model", "vision"}}, session,
                        {{"images", "images", ""}},
                        {{"document", "docs", ""}, {"text", "text", ""}}));
  AlgContext context;
  const ImageFrame frame{1, 1, 3, {10, 20, 30}};
  ASSERT_TRUE(
      context.Publish("images", ImageFrameBatch{{9, 4, frame}, {2, 7, frame}}));
  ASSERT_EQ(node.node->Process(&context), 0);
  ASSERT_EQ(model->calls.size(), 1u);
  EXPECT_EQ(model->calls[0][0].data.data, (std::vector<uint8_t>{10, 20, 30}));
  EXPECT_EQ(model->calls[0][1].req_id, 2u);
  const auto* docs = context.Read<OcrDocumentBatch>("docs");
  const auto* text = context.Read<TextBatch>("text");
  ASSERT_NE(docs, nullptr);
  ASSERT_NE(text, nullptr);
  ASSERT_EQ(docs->size(), 2u);
  ASSERT_EQ(text->size(), 2u);
  ASSERT_EQ((*docs)[0].data.boxes.size(), 2u);
  const auto& box = (*docs)[0].data.boxes[1];
  EXPECT_FLOAT_EQ(box.x, 5);
  EXPECT_FLOAT_EQ(box.y, 6);
  EXPECT_FLOAT_EQ(box.width, 7);
  EXPECT_FLOAT_EQ(box.height, 8);
  EXPECT_EQ(box.text, "猫");
  EXPECT_FLOAT_EQ(box.confidence, 0.8f);
  EXPECT_EQ((*docs)[0].data.combined_text, "A\n猫");
  EXPECT_EQ((*text)[0].data, "A\n猫");
  EXPECT_EQ((*text)[0].req_id, 9u);
  EXPECT_EQ((*text)[0].sub_id, 4u);
  EXPECT_EQ((*text)[1].data, "empty boxes");
  EXPECT_TRUE((*docs)[1].data.boxes.empty());
}

TEST(OcrDetectNodeTest,
     RecognitionFailurePublishesNeitherDocumentNorDerivedText) {
  SessionContext session;
  auto model = std::make_shared<RecordingOcr>();
  model->fail = true;
  ASSERT_TRUE(test_support::RegisterNodeModel(session, "vision", model));
  test_support::NodeProbe node;
  ASSERT_TRUE(node.Init("ocr_detect", {{"bind_model", "vision"}}, session,
                        {{"images", "images", ""}},
                        {{"document", "docs", ""}, {"text", "text", ""}}));
  AlgContext context;
  ASSERT_TRUE(context.Publish(
      "images", ImageFrameBatch{{1, 3, ImageFrame{1, 1, 3, {1, 2, 3}}}}));
  EXPECT_EQ(node.node->Process(&context), -17);
  EXPECT_FALSE(context.Has("docs"));
  EXPECT_FALSE(context.Has("text"));
  EXPECT_NE(context.GetErrorMessage().find("image recognition failed"),
            std::string::npos);
}
}  // namespace
}  // namespace llm_edgeflow
