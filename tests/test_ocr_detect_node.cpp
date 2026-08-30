#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "tests/support/inference/test_capability_models.h"

namespace alg_framework {

class OcrDetectNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();

    ocr_model_ = std::make_shared<test::TestOcrModel>();
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "ocr_model_v1", ocr_model_, "test-revision", "test_ocr_model", "ocr",
        "test_tensor_backend"));
  }
  std::unique_ptr<SessionContext> session_ctx_;
  std::shared_ptr<test::TestOcrModel> ocr_model_;
};

// 1. Process OCR Document Detection
TEST_F(OcrDetectNodeTest, ProcessOcrDetection) {
  auto node = NodeFactory::Instance().Create("OcrDetectNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"bind_model", "ocr_model_v1"}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  ImageRefBatch images;
  images.emplace_back(1, 0, "mock_invoice.jpg");
  ctx.Set("images", images);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* out_doc = ctx.Get<OcrDocumentBatch>("document");
  const auto* out_text = ctx.Get<TextBatch>("text");
  ASSERT_NE(out_doc, nullptr);
  ASSERT_NE(out_text, nullptr);
  ASSERT_EQ(out_doc->size(), 1u);
  ASSERT_EQ((*out_doc)[0].data.boxes.size(), 1u);
  EXPECT_EQ((*out_doc)[0].data.combined_text, "recognized:mock_invoice.jpg");
  EXPECT_EQ((*out_text)[0].data, "recognized:mock_invoice.jpg");
}

// 2. Missing Input Images Fails Closed
TEST_F(OcrDetectNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("OcrDetectNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init({{"bind_model", "ocr_model_v1"}}, session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -7101);
}

TEST_F(OcrDetectNodeTest, InvalidModelOutputFailsClosed) {
  auto node = NodeFactory::Instance().Create("OcrDetectNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init({{"bind_model", "ocr_model_v1"}}, session_ctx_.get()));

  ImageRefBatch images;
  images.emplace_back(7, 2, "neutral-image-ref");

  AlgContext count_ctx;
  count_ctx.Set("images", images);
  ocr_model_->return_wrong_count_ = true;
  EXPECT_EQ(node->Process(&count_ctx), -7102);

  ocr_model_->return_wrong_count_ = false;
  ocr_model_->corrupt_provenance_ = true;
  AlgContext provenance_ctx;
  provenance_ctx.Set("images", images);
  EXPECT_EQ(node->Process(&provenance_ctx), -7103);
}

}  // namespace alg_framework
