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
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"

namespace alg_framework {

class OcrDetectNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();

    auto ocr_engine = EngineFactory::Instance().Create("mock_npu_ocr");
    ASSERT_NE(ocr_engine, nullptr);
    ocr_engine->Load("./models/ppocr.bin", {{"max_batch_size", 2}});
    ASSERT_TRUE(session_ctx_->GetModelManager().RegisterModel(
        "ocr_model_v1", std::move(ocr_engine)));
  }
  std::unique_ptr<SessionContext> session_ctx_;
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
  EXPECT_FALSE((*out_text)[0].data.empty());
}

// 2. Missing Input Images Fails Closed
TEST_F(OcrDetectNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("OcrDetectNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init({{"bind_model", "ocr_model_v1"}}, session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_NE(node->Process(&empty_ctx), 0);
}

}  // namespace alg_framework
