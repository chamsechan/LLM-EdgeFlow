#include <gtest/gtest.h>

#include <array>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "core/alg_context.h"
#include "core/common_contracts.h"

namespace llm_edgeflow {
namespace {

TEST(TypedBlackboardContractsTest,
     PublishedPayloadOwnsAStableImmutableSnapshot) {
  constexpr BlackboardKey<TextBatch> key{"response", "TextBatch"};
  AlgContext context;
  TextBatch source{{17, 4, "original"}};
  ASSERT_TRUE(context.Publish(key, source));
  const auto* view = context.Read(key);
  ASSERT_NE(view, nullptr);
  source[0].data = "client mutation";
  EXPECT_FALSE(context.Publish(key, TextBatch{{99, 2, "replacement"}}));
  EXPECT_EQ(context.Read<int>("response"), nullptr);
  EXPECT_EQ(context.Read<TextBatch>("missing"), nullptr);
  for (int i = 0; i < 128; ++i) {
    ASSERT_TRUE(context.Publish("other_" + std::to_string(i), i));
  }
  EXPECT_EQ(context.Read(key), view);
  ASSERT_EQ(view->size(), 1U);
  EXPECT_EQ(view->at(0).req_id, 17U);
  EXPECT_EQ(view->at(0).sub_id, 4U);
  EXPECT_EQ(view->at(0).data, "original");
}

TEST(TypedBlackboardContractsTest, CompetingPublishersCommitExactlyOneValue) {
  AlgContext context;
  std::promise<void> release;
  auto start = release.get_future().share();
  std::array<bool, 8> committed{};
  std::vector<std::thread> threads;
  for (size_t i = 0; i < committed.size(); ++i) {
    threads.emplace_back([&, i] {
      start.wait();
      committed[i] = context.Publish("winner", static_cast<int>(i));
    });
  }
  release.set_value();
  for (auto& thread : threads) thread.join();
  int winners = 0;
  int expected = -1;
  for (size_t i = 0; i < committed.size(); ++i) {
    if (committed[i]) {
      ++winners;
      expected = static_cast<int>(i);
    }
  }
  EXPECT_EQ(winners, 1);
  const auto* value = context.Read<int>("winner");
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, expected);
}
}  // namespace
}  // namespace llm_edgeflow
