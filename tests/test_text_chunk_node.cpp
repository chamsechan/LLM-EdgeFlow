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

namespace alg_framework {

class TextChunkNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0);
    session_ctx_ = std::make_unique<SessionContext>();
  }
  std::unique_ptr<SessionContext> session_ctx_;
};

// 1. Init & Config Validation
TEST_F(TextChunkNodeTest, InitAndConfigValidation) {
  auto node = NodeFactory::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);

  // Default config
  EXPECT_TRUE(node->Init(nlohmann::json::object(), session_ctx_.get()));

  // Custom valid config
  nlohmann::json cfg = {{"chunk_size", 50}, {"overlap", 10}};
  EXPECT_TRUE(node->Init(cfg, session_ctx_.get()));
}

// 2. Process Single and Batch Chunks with ChunkCounts
TEST_F(TextChunkNodeTest, ProcessBatchAndChunkCounts) {
  auto node = NodeFactory::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"chunk_size", 20}, {"overlap", 0}};
  ASSERT_TRUE(node->Init(cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch input_batch;
  // Item 0: 50 chars -> 3 chunks (20, 20, 10)
  input_batch.emplace_back(
      101, 0, "12345678901234567890123456789012345678901234567890");
  // Item 1: 10 chars -> 1 chunk
  input_batch.emplace_back(102, 0, "1234567890");
  ctx.Set("text", input_batch);

  EXPECT_EQ(node->Process(&ctx), 0);

  const auto* chunks = ctx.Get<TextBatch>("chunks");
  ASSERT_NE(chunks, nullptr);
  EXPECT_EQ(chunks->size(), 4u);
  EXPECT_EQ((*chunks)[0].req_id, 101u);
  EXPECT_EQ((*chunks)[0].sub_id, 0u);
  EXPECT_EQ((*chunks)[1].req_id, 101u);
  EXPECT_EQ((*chunks)[1].sub_id, 1u);
  EXPECT_EQ((*chunks)[2].req_id, 101u);
  EXPECT_EQ((*chunks)[2].sub_id, 2u);
  EXPECT_EQ((*chunks)[3].req_id, 102u);
  EXPECT_EQ((*chunks)[3].sub_id, 0u);

  const auto* counts = ctx.Get<Int32Batch>("chunk_counts");
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(counts->size(), 2u);
  EXPECT_EQ((*counts)[0].data, 3);
  EXPECT_EQ((*counts)[1].data, 1);
}

// 3. Process Empty Input Strings
TEST_F(TextChunkNodeTest, ProcessEmptyStrings) {
  auto node = NodeFactory::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init(nlohmann::json::object(), session_ctx_.get()));

  AlgContext ctx;
  TextBatch input_batch;
  input_batch.emplace_back(1, 0, "");
  ctx.Set("text", input_batch);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* chunks = ctx.Get<TextBatch>("chunks");
  ASSERT_NE(chunks, nullptr);
  ASSERT_EQ(chunks->size(), 1u);
  EXPECT_TRUE((*chunks)[0].data.empty());

  const auto* counts = ctx.Get<Int32Batch>("chunk_counts");
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(counts->size(), 1u);
  EXPECT_EQ((*counts)[0].data, 1);
}

// 4. Missing Input Fails Closed
TEST_F(TextChunkNodeTest, MissingInputFailsClosed) {
  auto node = NodeFactory::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->Init(nlohmann::json::object(), session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_NE(node->Process(&empty_ctx), 0);
}

}  // namespace alg_framework
