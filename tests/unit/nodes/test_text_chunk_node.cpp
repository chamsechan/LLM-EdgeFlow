#include <gtest/gtest.h>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "contracts/error_codes.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "tests/support/node_probe.h"

namespace llm_edgeflow {
namespace {

constexpr BlackboardKey<TextBatch> kText{"text", "TextBatch"};
constexpr BlackboardKey<TextBatch> kChunks{"chunks", "TextBatch"};
constexpr BlackboardKey<Int32Batch> kCounts{"chunk_counts", "Int32Batch"};

class TextChunkNodeTest : public ::testing::Test {
 protected:
  bool Init(test_support::NodeProbe& probe, const nlohmann::json& params) {
    return probe.Init("text_chunk", params, session_, {{"text", "text"}},
                      {{"chunks", "chunks"}, {"chunk_counts", "chunk_counts"}});
  }
  SessionContext session_;
};

TEST_F(TextChunkNodeTest, DefaultWindowIsOneHundredCodePointsWithoutOverlap) {
  test_support::NodeProbe node;
  ASSERT_TRUE(Init(node, nlohmann::json::object()));
  AlgContext context;
  const std::string first_window(100, 'a');
  context.Publish(kText, TextBatch{{91, 7, first_window + "尾"}});
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* chunks = context.Read(kChunks);
  const auto* counts = context.Read(kCounts);
  ASSERT_NE(chunks, nullptr);
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(chunks->size(), 2U);
  EXPECT_EQ(chunks->at(0).data, first_window);
  EXPECT_EQ(chunks->at(1).data, "尾");
  ASSERT_EQ(counts->size(), 1U);
  EXPECT_EQ(counts->at(0).data, 2);
}

TEST_F(TextChunkNodeTest, ProducesLiteralWindowsInUnicodeCodePoints) {
  struct Case {
    const char* name;
    std::string text;
    int64_t size;
    int64_t overlap;
    std::vector<std::string> expected;
  };
  const std::vector<Case> cases{
      {"empty string", "", 4, 1, {""}},
      {"short ASCII", "xy", 4, 0, {"xy"}},
      {"exact ASCII window", "wxyz", 4, 0, {"wxyz"}},
      {"ASCII partial tail", "uvwxyz", 4, 0, {"uvwx", "yz"}},
      {"multibyte code points", "山🙂海é猫", 2, 0, {"山🙂", "海é", "猫"}},
      {"Unicode overlap", "日a🌙b星c", 4, 2, {"日a🌙b", "🌙b星c"}},
      {"overlap ends on full window",
       "abcdefgh",
       4,
       2,
       {"abcd", "cdef", "efgh"}},
      {"overlap ends on partial window",
       "abcdefghij",
       5,
       2,
       {"abcde", "defgh", "ghij"}},
      {"step of one stops after coverage", "pqrst", 4, 3, {"pqrs", "qrst"}},
      {"one code point per window", "🙂汉z", 1, 0, {"🙂", "汉", "z"}},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.name);
    test_support::NodeProbe node;
    ASSERT_TRUE(
        Init(node, {{"chunk_size", item.size}, {"overlap", item.overlap}}));
    AlgContext context;
    context.Publish(kText, TextBatch{{91, 7, item.text}});
    ASSERT_EQ(node.node->Process(&context), 0);
    const auto* chunks = context.Read(kChunks);
    ASSERT_NE(chunks, nullptr);
    ASSERT_EQ(chunks->size(), item.expected.size());
    for (size_t i = 0; i < item.expected.size(); ++i) {
      EXPECT_EQ(chunks->at(i).data, item.expected[i]);
    }
    const auto* counts = context.Read(kCounts);
    ASSERT_NE(counts, nullptr);
    ASSERT_EQ(counts->size(), 1U);
    EXPECT_EQ(counts->at(0).data, static_cast<int32_t>(item.expected.size()));
  }
}

TEST_F(TextChunkNodeTest, EmptyBatchPublishesTwoEmptyOutputs) {
  test_support::NodeProbe node;
  ASSERT_TRUE(Init(node, {{"chunk_size", 3}, {"overlap", 1}}));
  AlgContext context;
  context.Publish(kText, TextBatch{});
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* chunks = context.Read(kChunks);
  const auto* counts = context.Read(kCounts);
  ASSERT_NE(chunks, nullptr);
  ASSERT_NE(counts, nullptr);
  EXPECT_TRUE(chunks->empty());
  EXPECT_TRUE(counts->empty());
}

TEST_F(TextChunkNodeTest, CountsKeepParentKeysAndChildrenAdvancePerRequest) {
  test_support::NodeProbe node;
  ASSERT_TRUE(Init(node, {{"chunk_size", 3}, {"overlap", 1}}));
  AlgContext context;
  context.Publish(
      kText, TextBatch{{73, 9, "abcdef"}, {12, 4, "四🙂五六"}, {73, 2, "jk"}});
  const TextBatch expected_chunks{{73, 0, "abc"},  {73, 1, "cde"},
                                  {73, 2, "ef"},   {12, 0, "四🙂五"},
                                  {12, 1, "五六"}, {73, 3, "jk"}};
  const Int32Batch expected_counts{{73, 9, 3}, {12, 4, 2}, {73, 2, 1}};
  ASSERT_EQ(node.node->Process(&context), 0);
  const auto* chunks = context.Read(kChunks);
  const auto* counts = context.Read(kCounts);
  ASSERT_NE(chunks, nullptr);
  ASSERT_NE(counts, nullptr);
  ASSERT_EQ(chunks->size(), expected_chunks.size());
  ASSERT_EQ(counts->size(), expected_counts.size());
  for (size_t i = 0; i < expected_chunks.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(chunks->at(i).req_id, expected_chunks[i].req_id);
    EXPECT_EQ(chunks->at(i).sub_id, expected_chunks[i].sub_id);
    EXPECT_EQ(chunks->at(i).data, expected_chunks[i].data);
  }
  for (size_t i = 0; i < expected_counts.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(counts->at(i).req_id, expected_counts[i].req_id);
    EXPECT_EQ(counts->at(i).sub_id, expected_counts[i].sub_id);
    EXPECT_EQ(counts->at(i).data, expected_counts[i].data);
  }
}

TEST_F(TextChunkNodeTest, InvalidUtf8AfterValidParentPublishesNeitherOutput) {
  test_support::NodeProbe node;
  ASSERT_TRUE(Init(node, {{"chunk_size", 3}, {"overlap", 1}}));
  AlgContext context;
  // Two bytes of an unfinished four-byte code point after an ASCII prefix.
  const std::string invalid = std::string("no") + "\xF0\x9F";
  context.Publish(kText, TextBatch{{73, 9, "abcdef"}, {12, 4, invalid}});
  EXPECT_EQ(node.node->Process(&context), error::kInvalidUtf8);
  EXPECT_NE(
      context.GetErrorMessage().find("invalid UTF-8 input at byte offset 2"),
      std::string::npos);
  const auto& diagnostic = context.GetErrorMessage();
  const std::string source =
      "SplitPayloads callback_failed for req_id=12, sub_id=4";
  EXPECT_EQ(diagnostic.find(source), 0u);
  EXPECT_EQ(diagnostic.find("SplitPayloads", source.size()), std::string::npos);
  EXPECT_EQ(diagnostic.find("req_id=12, sub_id=4", source.size()),
            std::string::npos);
  EXPECT_EQ(context.Read(kChunks), nullptr);
  EXPECT_EQ(context.Read(kCounts), nullptr);
}

TEST_F(TextChunkNodeTest, OverlapMustBeSmallerThanWindow) {
  for (int64_t overlap : {4, 5}) {
    SCOPED_TRACE(overlap);
    test_support::NodeProbe node;
    EXPECT_FALSE(Init(node, {{"chunk_size", 4}, {"overlap", overlap}}));
    EXPECT_NE(node.diagnostic.find("overlap must be smaller than chunk_size"),
              std::string::npos);
  }
}

}  // namespace
}  // namespace llm_edgeflow
