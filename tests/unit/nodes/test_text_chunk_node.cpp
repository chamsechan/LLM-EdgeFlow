#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "nodes/node_error_codes.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {

class TextChunkNodeTest : public ::testing::Test {
 protected:
  void SetUp() override { session_ctx_ = std::make_unique<SessionContext>(); }
  std::unique_ptr<SessionContext> session_ctx_;
};

TEST_F(TextChunkNodeTest, InitAndConfigValidation) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);

  // 默认配置
  EXPECT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  // 自定义合法配置
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

TEST_F(TextChunkNodeTest, ProcessBatchAndChunkCounts) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);

  nlohmann::json cfg = {{"chunk_size", 20}, {"overlap", 0}};
  ASSERT_TRUE(InitNodeForTest(*node, cfg, session_ctx_.get()));

  AlgContext ctx;
  TextBatch input_batch;
  // 条目 0：50 个字符 -> 3 个分块 (20, 20, 10)
  input_batch.emplace_back(
      101, 0, "12345678901234567890123456789012345678901234567890");
  // 条目 1：10 个字符 -> 1 个分块
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

TEST_F(TextChunkNodeTest, MissingInputFailsClosed) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(
      InitNodeForTest(*node, nlohmann::json::object(), session_ctx_.get()));

  AlgContext empty_ctx;
  EXPECT_EQ(node->Process(&empty_ctx), node_error::author_node::kMissingInput);
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

  EXPECT_EQ(node->Process(&ctx), node_error::author_node::kBusinessError);
  EXPECT_NE(ctx.GetErrorMessage().find("SplitPayloads duplicate input item"),
            std::string::npos);
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
  // 请求 10：条目 0 的 sub_id 为 5，产出 2 个分块 (20 个字符)
  input_batch.emplace_back(10, 5, "12345678901234567890");
  // 请求 10：条目 1 的 sub_id 为 9，产出 1 个分块 (10 个字符)
  input_batch.emplace_back(10, 9, "abcdefghij");
  // 请求 20：条目 0 的 sub_id 为 1，产出 1 个分块
  input_batch.emplace_back(20, 1, "hello");
  ctx.Publish("text", input_batch);

  ASSERT_EQ(node->Process(&ctx), 0);

  const auto* chunks = ctx.Read<TextBatch>("chunks");
  ASSERT_NE(chunks, nullptr);
  ASSERT_EQ(chunks->size(), 4u);
  // 请求 10 的分块 sub_id 必须连续：0, 1, 2
  EXPECT_EQ((*chunks)[0].req_id, 10u);
  EXPECT_EQ((*chunks)[0].sub_id, 0u);
  EXPECT_EQ((*chunks)[1].req_id, 10u);
  EXPECT_EQ((*chunks)[1].sub_id, 1u);
  EXPECT_EQ((*chunks)[2].req_id, 10u);
  EXPECT_EQ((*chunks)[2].sub_id, 2u);
  // 请求 20 的分块 sub_id 必须为 0
  EXPECT_EQ((*chunks)[3].req_id, 20u);
  EXPECT_EQ((*chunks)[3].sub_id, 0u);

  // chunk_counts 必须保留父条目的 (req_id, sub_id)
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

TEST_F(TextChunkNodeTest, InterleavedRequestsContinuousSubIdAcrossParents) {
  auto node = NodeRegistry::Instance().Create("TextChunkNode");
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(InitNodeForTest(*node, {{"chunk_size", 10}, {"overlap", 0}},
                              session_ctx_.get()));

  AlgContext ctx;
  TextBatch input_batch;
  // 请求 10，sub 0：20 个字符 -> 2 个分块
  input_batch.emplace_back(10, 0, "12345678901234567890");
  // 交错的请求 20，sub 0：10 个字符 -> 1 个分块
  input_batch.emplace_back(20, 0, "abcdefghij");
  // 继续请求 10，sub 1：10 个字符 -> 1 个分块
  input_batch.emplace_back(10, 1, "klmnopqrst");
  ctx.Publish("text", input_batch);

  ASSERT_EQ(node->Process(&ctx), 0);

  const auto* chunks = ctx.Read<TextBatch>("chunks");
  ASSERT_NE(chunks, nullptr);
  ASSERT_EQ(chunks->size(), 4u);

  // 请求 10 的第一批
  EXPECT_EQ((*chunks)[0].req_id, 10u);
  EXPECT_EQ((*chunks)[0].sub_id, 0u);
  EXPECT_EQ((*chunks)[0].data, "1234567890");

  EXPECT_EQ((*chunks)[1].req_id, 10u);
  EXPECT_EQ((*chunks)[1].sub_id, 1u);
  EXPECT_EQ((*chunks)[1].data, "1234567890");

  // 交错的请求 20 从 0 开始
  EXPECT_EQ((*chunks)[2].req_id, 20u);
  EXPECT_EQ((*chunks)[2].sub_id, 0u);
  EXPECT_EQ((*chunks)[2].data, "abcdefghij");

  // 继续的请求 10 必须从 sub_id 2 接续 (不能重置！)
  EXPECT_EQ((*chunks)[3].req_id, 10u);
  EXPECT_EQ((*chunks)[3].sub_id, 2u);
  EXPECT_EQ((*chunks)[3].data, "klmnopqrst");

  const auto* counts = ctx.Read<Int32Batch>("chunk_counts");
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(counts->size(), 3u);
  EXPECT_EQ((*counts)[0].req_id, 10u);
  EXPECT_EQ((*counts)[0].sub_id, 0u);
  EXPECT_EQ((*counts)[0].data, 2);

  EXPECT_EQ((*counts)[1].req_id, 20u);
  EXPECT_EQ((*counts)[1].sub_id, 0u);
  EXPECT_EQ((*counts)[1].data, 1);

  EXPECT_EQ((*counts)[2].req_id, 10u);
  EXPECT_EQ((*counts)[2].sub_id, 1u);
  EXPECT_EQ((*counts)[2].data, 1);
}

}  // namespace llm_edgeflow
