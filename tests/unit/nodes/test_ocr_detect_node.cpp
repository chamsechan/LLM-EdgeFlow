#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/common_contracts.h"
#include "dev_support/inference/test_capability_models.h"
#include "tests/support/node_harness.h"

namespace llm_edgeflow {

TEST(OcrDetectNodeTest, ProcessOcrDetection) {
  auto model = std::make_shared<test::TestOcrModel>();
  NodeHarness harness("ocr_detect");
  harness.Config({{"bind_model", "model"}}).BindModel("model", model);
  ImageFrameBatch images;
  images.emplace_back(1, 0, ImageFrame{2, 1, 6, {255, 0, 0, 0, 255, 0}});
  harness.CustomInput("images", images);

  const auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();
  const auto* out_doc = result.Output<OcrDocumentBatch>("document");
  const auto* out_text = result.Output<TextBatch>("text");
  ASSERT_NE(out_doc, nullptr);
  ASSERT_NE(out_text, nullptr);
  ASSERT_EQ(out_doc->size(), 1u);
  ASSERT_EQ(out_text->size(), 1u);
  ASSERT_EQ((*out_doc)[0].data.boxes.size(), 1u);
  EXPECT_EQ((*out_doc)[0].data.combined_text, "recognized:2");
  EXPECT_EQ((*out_text)[0].data, "recognized:2");
}

}  // namespace llm_edgeflow
