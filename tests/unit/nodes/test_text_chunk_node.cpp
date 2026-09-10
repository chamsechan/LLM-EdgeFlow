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
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {

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
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);

  // Default config
  EXPECT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  // Custom valid config
  nlohmann::json cfg = {{"chunk_size", 50}, {"overlap", 10}};
  EXPECT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  auto invalid_chunk = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(invalid_chunk, nullptr);
  EXPECT_FALSE(
      InitNodeForTest(*invalid_chunk, {{"chunk_size", 0}}, session_ctx_.get()));

  auto invalid_overlap = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(invalid_overlap, nullptr);
  EXPECT_FALSE(InitNodeForTest(*invalid_overlap,
                               {{"chunk_size", 10}, {"overlap", 10}},
                               session_ctx_.get()));

  auto float_chunk = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(float_chunk, nullptr);
  EXPECT_FALSE(
      InitNodeForTest(*float_chunk, {{"chunk_size", 2.5}}, session_ctx_.get()));

  auto unknown_field = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(unknown_field, nullptr);
  EXPECT_FALSE(InitNodeForTest(*unknown_field, {{"non_existent_field", 123}},
                               session_ctx_.get()));
}

// 2. Process Single and Batch Chunks with ChunkCounts
TEST_F(TextChunkNodeTest, ProcessBatchAndChunkCounts) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"chunk_size", 20}, {"overlap", 0}};
  ASSERT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch input_batch;
  // Item 0: 50 chars -> 3 chunks (20, 20, 10)
  input_batch.emplace_back(
      101, 0, "12345678901234567890123456789012345678901234567890");
  // Item 1: 10 chars -> 1 chunk
  input_batch.emplace_back(102, 0, "1234567890");
  ctx.Publish("text", input_batch);

  EXPECT_EQ(node->Process(&ctx), 0);

  const auto* chunks = ctx.Read<TextBatch>("chunks");
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

  const auto* counts = ctx.Read<Int32Batch>("chunk_counts");
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(counts->size(), 2u);
  EXPECT_EQ((*counts)[0].data, 3);
  EXPECT_EQ((*counts)[1].data, 1);
}

TEST_F(TextChunkNodeTest, HighOverlapStopsWhenInputIsCovered) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_TRUE(InitNodeForTest(*node, {{"chunk_size", 1000}, {"overlap", 999}},
                              session_ctx_.get()));
  AlgContext ctx;
  const std::string shorter(999, 'a');
  const std::string exact(1000, 'b');
  const std::string longer(1001, 'c');
  ctx.Publish("text",
              TextBatch{{1, 0, shorter}, {2, 0, exact}, {3, 0, longer}});
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* chunks = ctx.Read<TextBatch>("chunks");
  const auto* counts = ctx.Read<Int32Batch>("chunk_counts");
  ASSERT_NE(chunks, nullptr);
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(chunks->size(), 4u);
  ASSERT_EQ(counts->size(), 3u);
  EXPECT_EQ(chunks->at(0).data, shorter);
  EXPECT_EQ(chunks->at(1).data, exact);
  EXPECT_EQ(chunks->at(2).data, longer.substr(0, 1000));
  EXPECT_EQ(chunks->at(3).data, longer.substr(1));
  EXPECT_EQ(chunks->at(2).req_id, 3u);
  EXPECT_EQ(chunks->at(2).sub_id, 0u);
  EXPECT_EQ(chunks->at(3).req_id, 3u);
  EXPECT_EQ(chunks->at(3).sub_id, 1u);
  EXPECT_EQ(counts->at(0).data, 1);
  EXPECT_EQ(counts->at(1).data, 1);
  EXPECT_EQ(counts->at(2).data, 2);
}

TEST_F(TextChunkNodeTest, OverlappingFinalPartialChunkIsEmittedOnce) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_TRUE(InitNodeForTest(*node, {{"chunk_size", 5}, {"overlap", 3}},
                              session_ctx_.get()));
  AlgContext ctx;
  ctx.Publish("text", TextBatch{{7, 0, "A中🙂BC文"}});
  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* chunks = ctx.Read<TextBatch>("chunks");
  ASSERT_NE(chunks, nullptr);
  ASSERT_EQ(chunks->size(), 2u);
  EXPECT_EQ(chunks->at(0).data, "A中🙂BC");
  EXPECT_EQ(chunks->at(1).data, "🙂BC文");
  const auto* counts = ctx.Read<Int32Batch>("chunk_counts");
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(counts->size(), 1u);
  EXPECT_EQ(counts->at(0).data, 2);
}

// 3. Process Empty Input Strings
TEST_F(TextChunkNodeTest, ProcessEmptyStrings) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  AlgContext ctx;
  TextBatch input_batch;
  input_batch.emplace_back(1, 0, "");
  ctx.Publish("text", input_batch);

  EXPECT_EQ(node->Process(&ctx), 0);
  const auto* chunks = ctx.Read<TextBatch>("chunks");
  ASSERT_NE(chunks, nullptr);
  ASSERT_EQ(chunks->size(), 1u);
  EXPECT_TRUE((*chunks)[0].data.empty());

  const auto* counts = ctx.Read<Int32Batch>("chunk_counts");
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(counts->size(), 1u);
  EXPECT_EQ((*counts)[0].data, 1);
}

TEST_F(TextChunkNodeTest, ChunksOnUnicodeCodePointBoundaries) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"chunk_size", 3}, {"overlap", 1}},
                              session_ctx_.get()));

  AlgContext ctx;
  TextBatch input_batch;
  input_batch.emplace_back(7, 4, "A中🙂B");
  ctx.Publish("text", input_batch);

  ASSERT_EQ(node->Process(&ctx), 0);
  const auto* chunks = ctx.Read<TextBatch>("chunks");
  ASSERT_NE(chunks, nullptr);
  ASSERT_EQ(chunks->size(), 2u);
  EXPECT_EQ((*chunks)[0].data, "A中🙂");
  EXPECT_EQ((*chunks)[1].data, "🙂B");
  EXPECT_EQ((*chunks)[0].req_id, 7u);
  EXPECT_EQ((*chunks)[0].sub_id, 0u);
  EXPECT_EQ((*chunks)[1].req_id, 7u);
  EXPECT_EQ((*chunks)[1].sub_id, 1u);

  const auto* counts = ctx.Read<Int32Batch>("chunk_counts");
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(counts->size(), 1u);
  EXPECT_EQ((*counts)[0].data, 2);
}

TEST_F(TextChunkNodeTest, InvalidUtf8FailsClosed) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  AlgContext ctx;
  TextBatch input_batch;
  input_batch.emplace_back(8, 0, std::string("ok") + "\xE4\xB8");
  ctx.Publish("text", input_batch);

  EXPECT_EQ(node->Process(&ctx), -4002);
  EXPECT_EQ(ctx.Read<TextBatch>("chunks"), nullptr);
  EXPECT_EQ(ctx.Read<Int32Batch>("chunk_counts"), nullptr);
}

// 4. Missing Input Fails Closed
TEST_F(TextChunkNodeTest, MissingInputFailsClosed) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), -4001);
}

TEST_F(TextChunkNodeTest, DuplicateInputFailsClosed) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  AlgContext ctx;
  TextBatch input_batch;
  input_batch.emplace_back(10, 0, "first message");
  input_batch.emplace_back(10, 0, "second message with same req_id and sub_id");
  ctx.Publish("text", input_batch);

  EXPECT_EQ(node->Process(&ctx), -4003);
  EXPECT_EQ(ctx.Read<TextBatch>("chunks"), nullptr);
  EXPECT_EQ(ctx.Read<Int32Batch>("chunk_counts"), nullptr);
}

TEST_F(TextChunkNodeTest,
       PreservesParentProvenanceAndContinuousSubIdPerRequest) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"chunk_size", 10}, {"overlap", 0}},
                              session_ctx_.get()));

  AlgContext ctx;
  TextBatch input_batch;
  // Request 10: item 0 has sub_id 5, produces 2 chunks (20 chars)
  input_batch.emplace_back(10, 5, "12345678901234567890");
  // Request 10: item 1 has sub_id 9, produces 1 chunk (10 chars)
  input_batch.emplace_back(10, 9, "abcdefghij");
  // Request 20: item 0 has sub_id 1, produces 1 chunk
  input_batch.emplace_back(20, 1, "hello");
  ctx.Publish("text", input_batch);

  ASSERT_EQ(node->Process(&ctx), 0);

  const auto* chunks = ctx.Read<TextBatch>("chunks");
  ASSERT_NE(chunks, nullptr);
  ASSERT_EQ(chunks->size(), 4u);
  // Request 10 chunks must have continuous sub_ids: 0, 1, 2
  EXPECT_EQ((*chunks)[0].req_id, 10u);
  EXPECT_EQ((*chunks)[0].sub_id, 0u);
  EXPECT_EQ((*chunks)[1].req_id, 10u);
  EXPECT_EQ((*chunks)[1].sub_id, 1u);
  EXPECT_EQ((*chunks)[2].req_id, 10u);
  EXPECT_EQ((*chunks)[2].sub_id, 2u);
  // Request 20 chunks must have sub_id: 0
  EXPECT_EQ((*chunks)[3].req_id, 20u);
  EXPECT_EQ((*chunks)[3].sub_id, 0u);

  // chunk_counts must preserve parent (req_id, sub_id)
  const auto* counts = ctx.Read<Int32Batch>("chunk_counts");
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(counts->size(), 3u);
  EXPECT_EQ((*counts)[0].req_id, 10u);
  EXPECT_EQ((*counts)[0].sub_id, 5u);
  EXPECT_EQ((*counts)[0].data, 2);

  EXPECT_EQ((*counts)[1].req_id, 10u);
  EXPECT_EQ((*counts)[1].sub_id, 9u);
  EXPECT_EQ((*counts)[1].data, 1);

  EXPECT_EQ((*counts)[2].req_id, 20u);
  EXPECT_EQ((*counts)[2].sub_id, 1u);
  EXPECT_EQ((*counts)[2].data, 1);
}

}  // namespace llm_edgeflow
