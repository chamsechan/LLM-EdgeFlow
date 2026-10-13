#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "nodes/traceable_algorithms.h"
#include "nodes/traceable_batch_operations.h"

namespace llm_edgeflow {
namespace {

template <typename Payload>
void ExpectBatch(const std::vector<TraceableItem<Payload>>& actual,
                 const std::vector<TraceableItem<Payload>>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(actual[i].req_id, expected[i].req_id);
    EXPECT_EQ(actual[i].sub_id, expected[i].sub_id);
    EXPECT_EQ(actual[i].data, expected[i].data);
  }
}

void ExpectDetail(const NodeFailure& failure, const char* operation,
                  BatchFailureReason reason, TraceableItemKey key) {
  ASSERT_TRUE(failure.batch_detail.has_value());
  EXPECT_EQ(failure.batch_detail->operation, operation);
  EXPECT_EQ(failure.batch_detail->reason, reason);
  ASSERT_TRUE(failure.batch_detail->key.has_value());
  EXPECT_EQ(failure.batch_detail->key->req_id, key.req_id);
  EXPECT_EQ(failure.batch_detail->key->sub_id, key.sub_id);
  const std::string diagnostic = failure.FormatDiagnostic();
  EXPECT_EQ(diagnostic.find(operation), 0u);
  EXPECT_EQ(diagnostic.find(operation, std::string(operation).size()),
            std::string::npos);
  const std::string item = "req_id=" + std::to_string(key.req_id) +
                           ", sub_id=" + std::to_string(key.sub_id);
  const auto position = diagnostic.find(item);
  ASSERT_NE(position, std::string::npos);
  EXPECT_EQ(diagnostic.find(item, position + item.size()), std::string::npos);
}

struct AcceptPayload {
  bool operator()(const std::string&) const { return true; }
};
template <typename Left, typename Right, typename = void>
struct CanJoin : std::false_type {};
template <typename Left, typename Right>
struct CanJoin<Left, Right,
               std::void_t<decltype(JoinByItem(std::declval<Left>(),
                                               std::declval<Right>()))>>
    : std::true_type {};
template <typename Anchor, typename Members, typename = void>
struct CanGroup : std::false_type {};
template <typename Anchor, typename Members>
struct CanGroup<Anchor, Members,
                std::void_t<decltype(GroupByRequest(std::declval<Anchor>(),
                                                    std::declval<Members>()))>>
    : std::true_type {};
template <typename Batch, typename = void>
struct CanSelect : std::false_type {};
template <typename Batch>
struct CanSelect<Batch, std::void_t<decltype(SelectBatch(std::declval<Batch>(),
                                                         AcceptPayload{}))>>
    : std::true_type {};

TEST(TraceableBatchOperationsTest, BorrowedViewsRequireLvalueStorage) {
  static_assert(CanJoin<TextBatch&, const TextBatch&>::value);
  static_assert(!CanJoin<TextBatch&&, const TextBatch&>::value);
  static_assert(!CanJoin<const TextBatch&, const TextBatch&&>::value);
  static_assert(CanGroup<const TextBatch&, TextBatch&>::value);
  static_assert(!CanGroup<const TextBatch&&, TextBatch&>::value);
  static_assert(!CanGroup<const TextBatch&, TextBatch&&>::value);
  static_assert(CanSelect<const TextBatch&>::value);
  static_assert(!CanSelect<TextBatch&&>::value);
  static_assert(!CanSelect<const TextBatch&&>::value);
  static_assert(!std::is_constructible_v<JoinedRow<std::string, std::string>,
                                         TraceableItem<std::string>&&>);
  SUCCEED();
}

TEST(TraceableBatchOperationsTest,
     JoinUsesWholeKeysLeftOrderAndBorrowedAddresses) {
  const TextBatch left{{30, 9, "left-a"}, {10, 2, "left-b"}, {30, 1, "left-c"}};
  const TextBatch right{
      {30, 1, "right-c"}, {30, 9, "right-a"}, {10, 2, "right-b"}};
  auto exact = JoinByItem(left, right);
  ASSERT_TRUE(exact.ok());
  ASSERT_EQ(exact.value().size(), 3U);
  const std::vector<size_t> right_indices{1, 2, 0};
  for (size_t i = 0; i < right_indices.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(&exact.value()[i].left, &left[i]);
    EXPECT_EQ(exact.value()[i].right, &right[right_indices[i]]);
  }
  EXPECT_EQ(&exact.value().left_batch(), &left);
  EXPECT_EQ(&exact.value().right_batch(), &right);

  const TextBatch partial{{30, 1, "right-c"}};
  auto left_join = JoinByItem(left, partial, JoinMode::kLeft);
  ASSERT_TRUE(left_join.ok());
  ASSERT_EQ(left_join.value().size(), 3U);
  EXPECT_EQ(left_join.value()[0].right, nullptr);
  EXPECT_EQ(left_join.value()[1].right, nullptr);
  EXPECT_EQ(left_join.value()[2].right, &partial[0]);
  for (size_t i = 0; i < left.size(); ++i)
    EXPECT_EQ(&left_join.value()[i].left, &left[i]);
  const TextBatch empty;
  auto empty_join = JoinByItem(empty, empty);
  ASSERT_TRUE(empty_join.ok());
  EXPECT_TRUE(empty_join.value().empty());
}

TEST(TraceableBatchOperationsTest, JoinRejectsDuplicateMissingAndUnknownKeys) {
  struct Case {
    const char* name;
    TextBatch left, right;
    JoinMode mode;
    BatchFailureReason reason;
    TraceableItemKey key;
  };
  const std::vector<Case> cases{
      {"left duplicate",
       {{30, 9, "a"}, {30, 9, "b"}},
       {{30, 9, "r"}},
       JoinMode::kExact,
       BatchFailureReason::kDuplicate,
       {30, 9}},
      {"right duplicate",
       {{30, 9, "a"}},
       {{30, 9, "r"}, {30, 9, "s"}},
       JoinMode::kLeft,
       BatchFailureReason::kDuplicate,
       {30, 9}},
      {"Exact missing right item",
       {{30, 9, "a"}, {30, 1, "b"}},
       {{30, 1, "r"}},
       JoinMode::kExact,
       BatchFailureReason::kMissing,
       {30, 9}},
      {"Exact unknown subkey",
       {{30, 9, "a"}},
       {{30, 1, "r"}},
       JoinMode::kExact,
       BatchFailureReason::kUnknown,
       {30, 1}},
      {"Left also rejects extras",
       {{30, 9, "a"}},
       {{30, 9, "r"}, {31, 9, "s"}},
       JoinMode::kLeft,
       BatchFailureReason::kUnknown,
       {31, 9}},
  };
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    auto result = JoinByItem(row.left, row.right, row.mode);
    ASSERT_FALSE(result.ok());
    ExpectDetail(result.failure(), "JoinByItem", row.reason, row.key);
  }
}

TEST(TraceableBatchOperationsTest,
     GroupsFollowFirstAnchorRequestAndMemberOrder) {
  const TextBatch anchor{
      {30, 9, "a"}, {10, 2, "b"}, {30, 1, "c"}, {20, 8, "d"}};
  const TextBatch members{{10, 6, "m1"}, {30, 4, "m2"}, {10, 0, "m3"}};
  auto result = GroupByRequest(anchor, members);
  ASSERT_TRUE(result.ok());
  const auto& groups = result.value();
  ASSERT_EQ(groups.size(), 3U);
  const std::vector<uint32_t> requests{30, 10, 20};
  const std::vector<std::vector<size_t>> anchor_indices{{0, 2}, {1}, {3}};
  const std::vector<std::vector<size_t>> member_indices{{1}, {0, 2}, {}};
  for (size_t i = 0; i < requests.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(groups[i].req_id(), requests[i]);
    ASSERT_EQ(groups[i].anchor_count(), anchor_indices[i].size());
    ASSERT_EQ(groups[i].member_count(), member_indices[i].size());
    for (size_t j = 0; j < anchor_indices[i].size(); ++j)
      EXPECT_EQ(&groups[i].anchors()[j].get(), &anchor[anchor_indices[i][j]]);
    for (size_t j = 0; j < member_indices[i].size(); ++j)
      EXPECT_EQ(&groups[i].members()[j].get(), &members[member_indices[i][j]]);
  }
  EXPECT_FALSE(groups[2].has_members());
  EXPECT_EQ(groups.FindByReqId(10), &groups[1]);
  EXPECT_EQ(groups.FindByReqId(99), nullptr);
  const std::vector<size_t> index_by_anchor{0, 1, 0, 2};
  for (size_t i = 0; i < index_by_anchor.size(); ++i)
    EXPECT_EQ(&groups.GroupByAnchorIndex(i), &groups[index_by_anchor[i]]);
  const TextBatch empty;
  auto empty_groups = GroupByRequest(empty, empty);
  ASSERT_TRUE(empty_groups.ok());
  EXPECT_TRUE(empty_groups.value().empty());
}

TEST(TraceableBatchOperationsTest,
     GroupRejectsDuplicateKeysAndUnknownRequests) {
  struct Case {
    const char* name;
    TextBatch anchor, members;
    BatchFailureReason reason;
    TraceableItemKey key;
  };
  const std::vector<Case> cases{
      {"duplicate anchor",
       {{30, 9, "a"}, {30, 9, "b"}},
       {},
       BatchFailureReason::kDuplicate,
       {30, 9}},
      {"duplicate member",
       {{30, 9, "a"}},
       {{30, 2, "m"}, {30, 2, "n"}},
       BatchFailureReason::kDuplicate,
       {30, 2}},
      {"unknown member request",
       {{30, 9, "a"}},
       {{31, 9, "m"}},
       BatchFailureReason::kUnknown,
       {31, 9}},
  };
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    auto result = GroupByRequest(row.anchor, row.members);
    ASSERT_FALSE(result.ok());
    ExpectDetail(result.failure(), "GroupByRequest", row.reason, row.key);
  }
}

TEST(TraceableBatchOperationsTest,
     SelectBorrowsAndScatterReplacesOnlySelectedWholeKeys) {
  TextBatch anchor{
      {20, 4, "keep"}, {30, 9, "replace"}, {20, 2, "replace"}, {30, 1, "keep"}};
  auto selected =
      SelectBatch(anchor, [](const TraceableItem<std::string>& item) {
        return item.sub_id == 9 || item.sub_id == 2;
      });
  ASSERT_TRUE(selected.ok());
  ASSERT_EQ(selected.value().size(), 2U);
  EXPECT_EQ(&selected.value()[0], &anchor[1]);
  EXPECT_EQ(&selected.value()[1], &anchor[2]);
  auto owned = selected.value().Materialize();
  anchor[1].data = "changed after Materialize";
  ExpectBatch(owned, TextBatch{{30, 9, "replace"}, {20, 2, "replace"}});
  const TextBatch replacements{{20, 2, "B"}, {30, 9, "A"}};
  auto scattered = ScatterReplace(selected.value(), replacements);
  ASSERT_TRUE(scattered.ok());
  ExpectBatch(
      scattered.value(),
      TextBatch{{20, 4, "keep"}, {30, 9, "A"}, {20, 2, "B"}, {30, 1, "keep"}});
  auto none = SelectBatch(anchor, [](const std::string&) { return false; });
  ASSERT_TRUE(none.ok());
  EXPECT_TRUE(none.value().empty());
  const TextBatch empty;
  auto unchanged = ScatterReplace(none.value(), empty);
  ASSERT_TRUE(unchanged.ok());
  ExpectBatch(unchanged.value(), TextBatch{{20, 4, "keep"},
                                           {30, 9, "changed after Materialize"},
                                           {20, 2, "replace"},
                                           {30, 1, "keep"}});
}

TEST(TraceableBatchOperationsTest, SelectionAndScatterRejectInvalidCoverage) {
  const TextBatch duplicate{{30, 9, "a"}, {30, 9, "b"}};
  int calls = 0;
  auto rejected = SelectBatch(duplicate, [&](const std::string&) {
    ++calls;
    return true;
  });
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(calls, 0);
  ExpectDetail(rejected.failure(), "SelectBatch",
               BatchFailureReason::kDuplicate, {30, 9});
  const TextBatch anchor{{30, 9, "a"}, {10, 2, "keep"}, {30, 1, "b"}};
  auto selected = SelectBatch(
      anchor,
      [](const TraceableItem<std::string>& item) { return item.req_id == 30; });
  ASSERT_TRUE(selected.ok());
  struct Case {
    const char* name;
    TextBatch replacements;
    BatchFailureReason reason;
    TraceableItemKey key;
  };
  const std::vector<Case> cases{
      {"duplicate replacement",
       {{30, 9, "A"}, {30, 9, "B"}},
       BatchFailureReason::kDuplicate,
       {30, 9}},
      {"missing selected key",
       {{30, 1, "B"}},
       BatchFailureReason::kMissing,
       {30, 9}},
      {"known but unselected",
       {{10, 2, "X"}},
       BatchFailureReason::kUnknown,
       {10, 2}},
      {"unknown full key",
       {{30, 2, "X"}},
       BatchFailureReason::kUnknown,
       {30, 2}},
  };
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    auto result = ScatterReplace(selected.value(), row.replacements);
    ASSERT_FALSE(result.ok());
    ExpectDetail(result.failure(), "ScatterReplace", row.reason, row.key);
  }
}

TEST(TraceableBatchOperationsTest, SelectionPrefersPayloadWhenBothAreCallable) {
  struct Predicate {
    int payload_calls = 0;
    int item_calls = 0;

    bool operator()(const std::string& payload) {
      ++payload_calls;
      return payload == "selected";
    }
    NodeResult<bool> operator()(const TraceableItem<std::string>&) {
      ++item_calls;
      return NodeResult<bool>::Failure("item overload must not run");
    }
  } predicate;
  const TextBatch anchor{{30, 9, "keep"}, {10, 2, "selected"}};
  auto result = SelectBatch(anchor, predicate);
  ASSERT_TRUE(result.ok());
  ExpectBatch(result.value().Materialize(), TextBatch{{10, 2, "selected"}});
  EXPECT_EQ(predicate.payload_calls, 2);
  EXPECT_EQ(predicate.item_calls, 0);
}

TEST(TraceableBatchOperationsTest, SelectionFailureIdentifiesTheActualItem) {
  const TextBatch anchor{{30, 9, "first"}, {10, 2, "second"}, {30, 1, "third"}};
  struct Case {
    const char* name;
    int fault;
    const char* message;
  };
  const std::vector<Case> cases{
      {"NodeResult failure", 0, "literal predicate failure"},
      {"standard exception", 1, "literal predicate exception"},
      {"unknown exception", 2, "unknown exception"}};
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    int calls = 0;
    auto result =
        SelectBatch(anchor, [&](const std::string&) -> NodeResult<bool> {
          if (++calls == 1) return NodeResult<bool>::Success(true);
          if (row.fault == 1)
            throw std::runtime_error("literal predicate exception");
          if (row.fault == 2) throw 42;
          return NodeResult<bool>::Failure(
              "literal predicate failure",
              BatchFailureDetail{"previous operation",
                                 BatchFailureReason::kMissing,
                                 TraceableItemKey{88, 4}},
              -932);
        });
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(calls, 2);
    ExpectDetail(result.failure(), "SelectBatch",
                 BatchFailureReason::kCallbackFailed, {10, 2});
    EXPECT_EQ(result.failure().cause_code, row.fault == 0 ? -932 : 0);
    EXPECT_NE(result.failure().message.find(row.message), std::string::npos);
    if (row.fault == 0) {
      EXPECT_EQ(result.failure().message, row.message);
    }
  }
  auto item_predicate =
      SelectBatch(anchor, [](const TraceableItem<std::string>& item) {
        return NodeResult<bool>::Success(item.req_id == 10);
      });
  ASSERT_TRUE(item_predicate.ok());
  ExpectBatch(item_predicate.value().Materialize(),
              TextBatch{{10, 2, "second"}});

  int item_calls = 0;
  auto failed_item = SelectBatch(
      anchor, [&](const TraceableItem<std::string>& item) -> NodeResult<bool> {
        ++item_calls;
        if (item.req_id != 10) return NodeResult<bool>::Success(true);
        return NodeResult<bool>::Failure(
            "item predicate failed",
            BatchFailureDetail{"previous operation",
                               BatchFailureReason::kMissing,
                               TraceableItemKey{88, 4}},
            -933);
      });
  ASSERT_FALSE(failed_item.ok());
  EXPECT_EQ(item_calls, 2);
  ExpectDetail(failed_item.failure(), "SelectBatch",
               BatchFailureReason::kCallbackFailed, {10, 2});
  EXPECT_EQ(failed_item.failure().cause_code, -933);
  EXPECT_EQ(failed_item.failure().message, "item predicate failed");
}

TEST(TraceableBatchOperationsTest,
     SplitPreservesParentsAndAllocatesChildrenPerRequest) {
  const TextBatch input{
      {30, 9, "two"}, {10, 2, "one"}, {30, 1, "none"}, {30, 7, "last"}};
  auto result = SplitPayloads(input, [](const std::string& value) {
    if (value == "two") return std::vector<std::string>{"A", "B"};
    if (value == "one") return std::vector<std::string>{"C"};
    if (value == "last") return std::vector<std::string>{"D"};
    return std::vector<std::string>{};
  });
  ASSERT_TRUE(result.ok());
  ExpectBatch(
      result.value().children,
      TextBatch{{30, 0, "A"}, {30, 1, "B"}, {10, 0, "C"}, {30, 2, "D"}});
  ExpectBatch(result.value().counts,
              Int32Batch{{30, 9, 2}, {10, 2, 1}, {30, 1, 0}, {30, 7, 1}});
  const TextBatch single{{40, 7, "single"}};
  auto wrapped = SplitPayloads(single, [](const std::string&) {
    return NodeResult<std::vector<int32_t>>::Success({3, 4});
  });
  ASSERT_TRUE(wrapped.ok());
  ExpectBatch(wrapped.value().children, Int32Batch{{40, 0, 3}, {40, 1, 4}});
  ExpectBatch(wrapped.value().counts, Int32Batch{{40, 7, 2}});
  const TextBatch empty;
  int calls = 0;
  auto empty_result = SplitPayloads(empty, [&](const std::string&) {
    ++calls;
    return std::vector<int>{7};
  });
  ASSERT_TRUE(empty_result.ok());
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(empty_result.value().children.empty());
  EXPECT_TRUE(empty_result.value().counts.empty());
}

TEST(TraceableBatchOperationsTest,
     SplitRejectsDuplicatesAndAttachesCallbackFailures) {
  const TextBatch duplicate{{30, 9, "a"}, {30, 9, "b"}};
  int calls = 0;
  auto rejected = SplitPayloads(duplicate, [&](const std::string&) {
    ++calls;
    return std::vector<int>{1};
  });
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(calls, 0);
  ExpectDetail(rejected.failure(), "SplitPayloads",
               BatchFailureReason::kDuplicate, {30, 9});
  const TextBatch input{{30, 9, "first"}, {10, 2, "second"}, {30, 1, "third"}};
  struct Case {
    const char* name;
    int fault;
    const char* message;
  };
  const std::vector<Case> cases{
      {"NodeResult failure", 0, "literal split failure"},
      {"standard exception", 1, "literal split exception"},
      {"unknown exception", 2, "unknown exception"}};
  for (const auto& row : cases) {
    SCOPED_TRACE(row.name);
    calls = 0;
    auto result = SplitPayloads(
        input, [&](const std::string&) -> NodeResult<std::vector<int>> {
          if (++calls == 1) return NodeResult<std::vector<int>>::Success({7});
          if (row.fault == 1)
            throw std::runtime_error("literal split exception");
          if (row.fault == 2) throw 42;
          return NodeResult<std::vector<int>>::Failure(
              "literal split failure",
              BatchFailureDetail{"previous operation",
                                 BatchFailureReason::kMissing,
                                 TraceableItemKey{88, 4}},
              -933);
        });
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(calls, 2);
    ExpectDetail(result.failure(), "SplitPayloads",
                 BatchFailureReason::kCallbackFailed, {10, 2});
    EXPECT_EQ(result.failure().cause_code, row.fault == 0 ? -933 : 0);
    EXPECT_NE(result.failure().message.find(row.message), std::string::npos);
    if (row.fault == 0) {
      EXPECT_EQ(result.failure().message, row.message);
    }
  }
}

TEST(TraceableBatchOperationsTest, SplitChecksCountAndSubIdAtNarrowBoundaries) {
  const TraceableItemKey parent{30, 9};
  auto zero = detail::CheckedSplitCount(0, parent);
  ASSERT_TRUE(zero.ok());
  EXPECT_EQ(zero.value(), 0);
  constexpr size_t max_count =
      static_cast<size_t>(std::numeric_limits<int32_t>::max());
  auto maximum = detail::CheckedSplitCount(max_count, parent);
  ASSERT_TRUE(maximum.ok());
  EXPECT_EQ(maximum.value(), std::numeric_limits<int32_t>::max());
  auto overflow = detail::CheckedSplitCount(max_count + 1, parent);
  ASSERT_FALSE(overflow.ok());
  ExpectDetail(overflow.failure(), "SplitPayloads",
               BatchFailureReason::kCountOverflow, parent);
  const TextBatch input{{30, 9, "parent"}};
  constexpr uint32_t max_sub = std::numeric_limits<uint32_t>::max();
  auto last_valid = detail::SplitPayloadsInternal(
      input,
      [](const std::string&) { return std::vector<std::string>{"last"}; },
      {{30, max_sub}});
  ASSERT_TRUE(last_valid.ok());
  ExpectBatch(last_valid.value().children, TextBatch{{30, max_sub, "last"}});
  auto exhausted = detail::SplitPayloadsInternal(
      input,
      [](const std::string&) {
        return std::vector<std::string>{"last", "overflow"};
      },
      {{30, max_sub}});
  ASSERT_FALSE(exhausted.ok());
  ExpectDetail(exhausted.failure(), "SplitPayloads",
               BatchFailureReason::kSubIdOverflow, parent);
}

TEST(TraceableBatchOperationsTest,
     MapKeepsOrderProvenanceAndSkipsEmptyCallbacks) {
  const Int32Batch input{{30, 9, 11}, {10, 2, 22}, {30, 9, 33}};
  int calls = 0;
  auto plain = MapPayloads(input, [&](int32_t value) {
    ++calls;
    return value + 1;
  });
  EXPECT_EQ(calls, 3);
  ExpectBatch(plain, Int32Batch{{30, 9, 12}, {10, 2, 23}, {30, 9, 34}});
  auto wrapped = MapPayloads(input, [](int32_t value) {
    return NodeResult<int32_t>::Success(value + 1);
  });
  ASSERT_TRUE(wrapped.ok());
  ExpectBatch(wrapped.value(),
              Int32Batch{{30, 9, 12}, {10, 2, 23}, {30, 9, 34}});
  const Int32Batch empty;
  calls = 0;
  EXPECT_TRUE(MapPayloads(empty, [&](int32_t value) {
                ++calls;
                return value;
              }).empty());
  auto empty_wrapped = MapPayloads(empty, [&](int32_t value) {
    ++calls;
    return NodeResult<int32_t>::Success(value);
  });
  ASSERT_TRUE(empty_wrapped.ok());
  EXPECT_TRUE(empty_wrapped.value().empty());
  EXPECT_EQ(calls, 0);
}

TEST(TraceableBatchOperationsTest,
     MapPreservesExistingFailureDetailsOrAttachesItemKey) {
  const Int32Batch input{{30, 9, 11}, {10, 2, 22}, {30, 1, 33}};
  for (bool supplied_detail : {false, true}) {
    SCOPED_TRACE(supplied_detail);
    int calls = 0;
    auto result = MapPayloads(input, [&](int32_t value) {
      ++calls;
      if (value != 22) return NodeResult<int32_t>::Success(value + 1);
      NodeFailure failure{"literal map failure", -934};
      if (supplied_detail)
        failure.batch_detail = BatchFailureDetail{"original operation",
                                                  BatchFailureReason::kMissing,
                                                  TraceableItemKey{99, 7}};
      return NodeResult<int32_t>::Failure(std::move(failure));
    });
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(result.failure().cause_code, -934);
    EXPECT_EQ(result.failure().message, "literal map failure");
    ExpectDetail(
        result.failure(),
        supplied_detail ? "original operation" : "MapPayloads",
        supplied_detail ? BatchFailureReason::kMissing
                        : BatchFailureReason::kCallbackFailed,
        supplied_detail ? TraceableItemKey{99, 7} : TraceableItemKey{10, 2});
  }
}

TEST(TraceableBatchOperationsTest, MapLeavesExceptionsForTheRuntimeBoundary) {
  const Int32Batch input{{30, 9, 11}};
  EXPECT_THROW(MapPayloads(input,
                           [](int32_t) -> int32_t {
                             throw std::runtime_error("literal map exception");
                           }),
               std::runtime_error);
  EXPECT_THROW(MapPayloads(input,
                           [](int32_t) -> NodeResult<int32_t> {
                             throw std::runtime_error("literal map exception");
                           }),
               std::runtime_error);
  EXPECT_THROW(MapPayloads(input, [](int32_t) -> int32_t { throw 42; }), int);
  EXPECT_THROW(
      MapPayloads(input, [](int32_t) -> NodeResult<int32_t> { throw 42; }),
      int);
}

}  // namespace
}  // namespace llm_edgeflow
