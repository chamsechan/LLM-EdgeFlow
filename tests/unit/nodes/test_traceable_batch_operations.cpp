#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "contracts/traceable_item.h"
#include "core/common_contracts.h"
#include "core/node_registry.h"
#include "engine/model_interface.h"
#include "nodes/authoring.h"
#include "nodes/traceable_batch_operations.h"
#include "tests/support/node_harness.h"
#include "tests/support/node_test_utils.h"

namespace llm_edgeflow {
namespace {

class CountingMockLlmModel final : public ILlmModel {
 public:
  const std::string& ModelType() const noexcept override {
    static const std::string t = "counting_mock_llm";
    return t;
  }
  const std::string& Capability() const noexcept override {
    static const std::string cap = "llm";
    return cap;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  size_t GetMaxBatchSize() const noexcept override { return 8; }

  int Generate(const TextBatch& prompts, const GenerateOptions& options,
               TextBatch* outputs) noexcept override {
    ++call_count;
    last_options = options;
    last_prompts = prompts;
    if (fail_first_n > 0 && call_count <= fail_first_n) {
      if (outputs) outputs->clear();
      return -8901;
    }
    if (always_fail) {
      if (outputs) outputs->clear();
      return -8902;
    }
    if (outputs) {
      outputs->clear();
      for (const auto& item : prompts) {
        outputs->emplace_back(item.req_id, item.sub_id, "ans:" + item.data);
      }
      if (return_wrong_count && !outputs->empty()) outputs->pop_back();
      if (corrupt_provenance && !outputs->empty()) ++(*outputs)[0].sub_id;
    }
    return 0;
  }

  mutable int call_count = 0;
  mutable GenerateOptions last_options;
  mutable TextBatch last_prompts;
  int fail_first_n = 0;
  bool always_fail = false;
  bool return_wrong_count = false;
  bool corrupt_provenance = false;
};

class TraceableBatchOperationsTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(), 0); }
};

// ============================================================================
// 1. JoinByItem Tests
// ============================================================================

TEST_F(TraceableBatchOperationsTest, JoinByItemExactMatchingOrderPreserved) {
  std::vector<TraceableItem<std::string>> left = {
      {1, 0, "q1"}, {2, 0, "q2"}, {3, 0, "q3"}};
  std::vector<TraceableItem<std::string>> right = {
      {3, 0, "a3"}, {1, 0, "a1"}, {2, 0, "a2"}};

  auto res = JoinByItem(left, right, JoinMode::kExact);
  ASSERT_TRUE(res.ok()) << res.failure().message;

  const auto& view = res.value();
  ASSERT_EQ(view.size(), 3u);
  EXPECT_FALSE(view.empty());

  // Strict left order
  EXPECT_EQ(view[0].req_id(), 1u);
  EXPECT_EQ(view[0].sub_id(), 0u);
  EXPECT_EQ(view[0].left_payload(), "q1");
  ASSERT_TRUE(view[0].has_right());
  EXPECT_EQ(*view[0].right_payload(), "a1");

  EXPECT_EQ(view[1].req_id(), 2u);
  EXPECT_EQ(view[1].sub_id(), 0u);
  EXPECT_EQ(view[1].left_payload(), "q2");
  ASSERT_TRUE(view[1].has_right());
  EXPECT_EQ(*view[1].right_payload(), "a2");

  EXPECT_EQ(view[2].req_id(), 3u);
  EXPECT_EQ(view[2].sub_id(), 0u);
  EXPECT_EQ(view[2].left_payload(), "q3");
  ASSERT_TRUE(view[2].has_right());
  EXPECT_EQ(*view[2].right_payload(), "a3");
}

TEST_F(TraceableBatchOperationsTest,
       JoinByItemLeftModeMissingRightNullPointer) {
  std::vector<TraceableItem<std::string>> left = {
      {1, 0, "q1"}, {2, 0, "q2"}, {3, 0, "q3"}};
  std::vector<TraceableItem<std::string>> right = {{1, 0, "a1"}, {3, 0, "a3"}};

  auto res = JoinByItem(left, right, JoinMode::kLeft);
  ASSERT_TRUE(res.ok()) << res.failure().message;

  const auto& view = res.value();
  ASSERT_EQ(view.size(), 3u);

  EXPECT_TRUE(view[0].has_right());
  EXPECT_EQ(*view[0].right_payload(), "a1");

  EXPECT_FALSE(view[1].has_right());
  EXPECT_EQ(view[1].right, nullptr);
  EXPECT_EQ(view[1].right_payload(), nullptr);
  EXPECT_EQ(view[1].left_payload(), "q2");

  EXPECT_TRUE(view[2].has_right());
  EXPECT_EQ(*view[2].right_payload(), "a3");
}

TEST_F(TraceableBatchOperationsTest, JoinByItemExactModeMissingRightFails) {
  std::vector<TraceableItem<std::string>> left = {{1, 0, "q1"}, {2, 0, "q2"}};
  std::vector<TraceableItem<std::string>> right = {{1, 0, "a1"}};

  auto res = JoinByItem(left, right, JoinMode::kExact);
  ASSERT_FALSE(res.ok());
  const auto& failure = res.failure();
  ASSERT_TRUE(failure.batch_detail.has_value());
  EXPECT_EQ(failure.batch_detail->operation, "JoinByItem");
  EXPECT_EQ(failure.batch_detail->reason, BatchFailureReason::kMissing);
  ASSERT_TRUE(failure.batch_detail->key.has_value());
  EXPECT_EQ(failure.batch_detail->key->req_id, 2u);
  EXPECT_EQ(failure.batch_detail->key->sub_id, 0u);
}

TEST_F(TraceableBatchOperationsTest, JoinByItemRightExtraKeyFailsBothModes) {
  std::vector<TraceableItem<std::string>> left = {{1, 0, "q1"}};
  std::vector<TraceableItem<std::string>> right = {{1, 0, "a1"},
                                                   {99, 0, "extra"}};

  // Exact mode fails
  auto exact_res = JoinByItem(left, right, JoinMode::kExact);
  ASSERT_FALSE(exact_res.ok());
  ASSERT_TRUE(exact_res.failure().batch_detail.has_value());
  EXPECT_EQ(exact_res.failure().batch_detail->reason,
            BatchFailureReason::kUnknown);
  ASSERT_TRUE(exact_res.failure().batch_detail->key.has_value());
  EXPECT_EQ(exact_res.failure().batch_detail->key->req_id, 99u);

  // Left mode also fails on unknown right key
  auto left_res = JoinByItem(left, right, JoinMode::kLeft);
  ASSERT_FALSE(left_res.ok());
  ASSERT_TRUE(left_res.failure().batch_detail.has_value());
  EXPECT_EQ(left_res.failure().batch_detail->reason,
            BatchFailureReason::kUnknown);
  ASSERT_TRUE(left_res.failure().batch_detail->key.has_value());
  EXPECT_EQ(left_res.failure().batch_detail->key->req_id, 99u);
}

TEST_F(TraceableBatchOperationsTest, JoinByItemDuplicateKeysFail) {
  // Duplicate in left
  std::vector<TraceableItem<std::string>> left_dup = {{1, 0, "q1"},
                                                      {1, 0, "q1_dup"}};
  std::vector<TraceableItem<std::string>> right = {{1, 0, "a1"}};

  auto res1 = JoinByItem(left_dup, right, JoinMode::kExact);
  ASSERT_FALSE(res1.ok());
  ASSERT_TRUE(res1.failure().batch_detail.has_value());
  EXPECT_EQ(res1.failure().batch_detail->reason,
            BatchFailureReason::kDuplicate);
  EXPECT_EQ(res1.failure().batch_detail->key->req_id, 1u);

  // Duplicate in right
  std::vector<TraceableItem<std::string>> left = {{1, 0, "q1"}};
  std::vector<TraceableItem<std::string>> right_dup = {{1, 0, "a1"},
                                                       {1, 0, "a1_dup"}};
  auto res2 = JoinByItem(left, right_dup, JoinMode::kExact);
  ASSERT_FALSE(res2.ok());
  ASSERT_TRUE(res2.failure().batch_detail.has_value());
  EXPECT_EQ(res2.failure().batch_detail->reason,
            BatchFailureReason::kDuplicate);
  EXPECT_EQ(res2.failure().batch_detail->key->req_id, 1u);
}

TEST_F(TraceableBatchOperationsTest, JoinByItemEmptyCombinations) {
  std::vector<TraceableItem<std::string>> empty;
  std::vector<TraceableItem<std::string>> non_empty = {{1, 0, "q1"}};

  // Both empty: succeeds in both modes
  auto res_both_empty_exact = JoinByItem(empty, empty, JoinMode::kExact);
  EXPECT_TRUE(res_both_empty_exact.ok());
  EXPECT_TRUE(res_both_empty_exact.value().empty());

  auto res_both_empty_left = JoinByItem(empty, empty, JoinMode::kLeft);
  EXPECT_TRUE(res_both_empty_left.ok());
  EXPECT_TRUE(res_both_empty_left.value().empty());

  // Left empty, right non-empty: fails in both modes
  auto res_left_empty_exact = JoinByItem(empty, non_empty, JoinMode::kExact);
  EXPECT_FALSE(res_left_empty_exact.ok());
  EXPECT_EQ(res_left_empty_exact.failure().batch_detail->reason,
            BatchFailureReason::kUnknown);

  auto res_left_empty_left = JoinByItem(empty, non_empty, JoinMode::kLeft);
  EXPECT_FALSE(res_left_empty_left.ok());
  EXPECT_EQ(res_left_empty_left.failure().batch_detail->reason,
            BatchFailureReason::kUnknown);

  // Left non-empty, right empty: exact fails, left succeeds
  auto res_right_empty_exact = JoinByItem(non_empty, empty, JoinMode::kExact);
  EXPECT_FALSE(res_right_empty_exact.ok());
  EXPECT_EQ(res_right_empty_exact.failure().batch_detail->reason,
            BatchFailureReason::kMissing);

  auto res_right_empty_left = JoinByItem(non_empty, empty, JoinMode::kLeft);
  ASSERT_TRUE(res_right_empty_left.ok());
  ASSERT_EQ(res_right_empty_left.value().size(), 1u);
  EXPECT_FALSE(res_right_empty_left.value()[0].has_right());
}

// ============================================================================
// 2. GroupByRequest Tests
// ============================================================================

TEST_F(TraceableBatchOperationsTest, GroupByRequestAnchorFirstAppearanceOrder) {
  // A0 (req 10), B0 (req 20), A1 (req 10)
  std::vector<TraceableItem<std::string>> anchor = {
      {10, 0, "A0"}, {20, 0, "B0"}, {10, 1, "A1"}};
  std::vector<TraceableItem<std::string>> members = {
      {20, 0, "mB0"}, {10, 0, "mA0"}, {10, 1, "mA1"}};

  auto res = GroupByRequest(anchor, members);
  ASSERT_TRUE(res.ok()) << res.failure().message;

  const auto& view = res.value();
  ASSERT_EQ(view.size(), 2u);

  // First appearance in anchor: req 10, then req 20
  EXPECT_EQ(view[0].req_id(), 10u);
  EXPECT_EQ(view[1].req_id(), 20u);

  // Group 10 anchors
  ASSERT_EQ(view[0].anchor_count(), 2u);
  EXPECT_EQ(view[0].anchors()[0].get().data, "A0");
  EXPECT_EQ(view[0].anchors()[1].get().data, "A1");

  // Group 20 anchors
  ASSERT_EQ(view[1].anchor_count(), 1u);
  EXPECT_EQ(view[1].anchors()[0].get().data, "B0");
}

TEST_F(TraceableBatchOperationsTest,
       GroupByRequestMembersRelativeOrderPreserved) {
  std::vector<TraceableItem<std::string>> anchor = {{10, 0, "A0"}};
  // Members have sub_id 5 then 2 (unsorted!)
  std::vector<TraceableItem<std::string>> members = {{10, 5, "m5"},
                                                     {10, 2, "m2"}};

  auto res = GroupByRequest(anchor, members);
  ASSERT_TRUE(res.ok()) << res.failure().message;

  const auto& view = res.value();
  ASSERT_EQ(view.size(), 1u);
  ASSERT_EQ(view[0].member_count(), 2u);
  EXPECT_EQ(view[0].members()[0].get().data, "m5");
  EXPECT_EQ(view[0].members()[1].get().data, "m2");
}

TEST_F(TraceableBatchOperationsTest,
       GroupByRequestAnchorWithNoMembersKeepsEmptyGroup) {
  std::vector<TraceableItem<std::string>> anchor = {{10, 0, "A0"},
                                                    {20, 0, "B0"}};
  std::vector<TraceableItem<std::string>> members = {{10, 0, "mA0"}};

  auto res = GroupByRequest(anchor, members);
  ASSERT_TRUE(res.ok()) << res.failure().message;

  const auto& view = res.value();
  ASSERT_EQ(view.size(), 2u);
  EXPECT_EQ(view[0].req_id(), 10u);
  EXPECT_TRUE(view[0].has_members());

  EXPECT_EQ(view[1].req_id(), 20u);
  EXPECT_FALSE(view[1].has_members());
  EXPECT_EQ(view[1].member_count(), 0u);
}

TEST_F(TraceableBatchOperationsTest, GroupByRequestUnknownMemberReqIdFails) {
  std::vector<TraceableItem<std::string>> anchor = {{10, 0, "A0"}};
  std::vector<TraceableItem<std::string>> members = {{999, 0, "m999"}};

  auto res = GroupByRequest(anchor, members);
  ASSERT_FALSE(res.ok());
  ASSERT_TRUE(res.failure().batch_detail.has_value());
  EXPECT_EQ(res.failure().batch_detail->operation, "GroupByRequest");
  EXPECT_EQ(res.failure().batch_detail->reason, BatchFailureReason::kUnknown);
  ASSERT_TRUE(res.failure().batch_detail->key.has_value());
  EXPECT_EQ(res.failure().batch_detail->key->req_id, 999u);
}

TEST_F(TraceableBatchOperationsTest, GroupByRequestDuplicatesFail) {
  // Duplicate in anchor
  std::vector<TraceableItem<std::string>> anchor_dup = {{10, 0, "A0"},
                                                        {10, 0, "A0_dup"}};
  std::vector<TraceableItem<std::string>> members = {{10, 0, "m0"}};
  auto res1 = GroupByRequest(anchor_dup, members);
  ASSERT_FALSE(res1.ok());
  EXPECT_EQ(res1.failure().batch_detail->reason,
            BatchFailureReason::kDuplicate);

  // Duplicate in members
  std::vector<TraceableItem<std::string>> anchor = {{10, 0, "A0"}};
  std::vector<TraceableItem<std::string>> members_dup = {{10, 0, "m0"},
                                                         {10, 0, "m0_dup"}};
  auto res2 = GroupByRequest(anchor, members_dup);
  ASSERT_FALSE(res2.ok());
  EXPECT_EQ(res2.failure().batch_detail->reason,
            BatchFailureReason::kDuplicate);
}

TEST_F(TraceableBatchOperationsTest, GroupByRequestEmptyCombinations) {
  std::vector<TraceableItem<std::string>> empty;
  std::vector<TraceableItem<std::string>> members = {{10, 0, "m"}};

  // Empty anchor + empty members: success
  auto res_empty = GroupByRequest(empty, empty);
  EXPECT_TRUE(res_empty.ok());
  EXPECT_TRUE(res_empty.value().empty());

  // Empty anchor + non-empty members: failure
  auto res_fail = GroupByRequest(empty, members);
  EXPECT_FALSE(res_fail.ok());
  EXPECT_EQ(res_fail.failure().batch_detail->reason,
            BatchFailureReason::kUnknown);
}

TEST_F(TraceableBatchOperationsTest,
       GroupByRequestPreservesOriginalAnchorOrderFor1to1) {
  // Anchor is A0, B0, A1
  std::vector<TraceableItem<std::string>> anchor = {
      {10, 0, "A0"}, {20, 0, "B0"}, {10, 1, "A1"}};
  std::vector<TraceableItem<std::string>> members = {
      {10, 0, "ctx10_1"}, {10, 1, "ctx10_2"}, {20, 0, "ctx20_1"}};

  auto res = GroupByRequest(anchor, members);
  ASSERT_TRUE(res.ok());

  const auto& view = res.value();

  // Generate 1:1 output using GroupByAnchorIndex
  std::vector<TraceableItem<std::string>> output;
  output.reserve(anchor.size());
  for (size_t i = 0; i < anchor.size(); ++i) {
    const auto& item = anchor[i];
    const auto& group = view.GroupByAnchorIndex(i);
    std::string combined = item.data + ":(";
    for (const auto& m : group.members()) {
      combined += m.get().data + ",";
    }
    combined += ")";
    output.emplace_back(item.req_id, item.sub_id, std::move(combined));
  }

  // Verify output strictly matches anchor order: A0, B0, A1
  ASSERT_EQ(output.size(), 3u);
  EXPECT_EQ(output[0].req_id, 10u);
  EXPECT_EQ(output[0].sub_id, 0u);
  EXPECT_EQ(output[0].data, "A0:(ctx10_1,ctx10_2,)");

  EXPECT_EQ(output[1].req_id, 20u);
  EXPECT_EQ(output[1].sub_id, 0u);
  EXPECT_EQ(output[1].data, "B0:(ctx20_1,)");

  EXPECT_EQ(output[2].req_id, 10u);
  EXPECT_EQ(output[2].sub_id, 1u);
  EXPECT_EQ(output[2].data, "A1:(ctx10_1,ctx10_2,)");
}

// ============================================================================
// 3. SelectBatch and ScatterReplace Tests
// ============================================================================

TEST_F(TraceableBatchOperationsTest, SelectAndScatterAllSelected) {
  std::vector<TraceableItem<std::string>> anchor = {
      {1, 0, "a"}, {2, 0, "b"}, {3, 0, "c"}};

  auto sel_res = SelectBatch(anchor, [](const std::string&) { return true; });
  ASSERT_TRUE(sel_res.ok());
  const auto& selection = sel_res.value();
  EXPECT_EQ(selection.size(), 3u);

  auto sub_batch = selection.Materialize();
  ASSERT_EQ(sub_batch.size(), 3u);
  for (auto& item : sub_batch) {
    item.data += "_mod";
  }

  auto scatter_res = ScatterReplace(selection, sub_batch);
  ASSERT_TRUE(scatter_res.ok());
  const auto& full = scatter_res.value();
  ASSERT_EQ(full.size(), 3u);
  EXPECT_EQ(full[0].data, "a_mod");
  EXPECT_EQ(full[1].data, "b_mod");
  EXPECT_EQ(full[2].data, "c_mod");
}

TEST_F(TraceableBatchOperationsTest, SelectAndScatterNoneSelected) {
  std::vector<TraceableItem<std::string>> anchor = {
      {1, 0, "a"}, {2, 0, "b"}, {3, 0, "c"}};

  auto sel_res = SelectBatch(anchor, [](const std::string&) { return false; });
  ASSERT_TRUE(sel_res.ok());
  const auto& selection = sel_res.value();
  EXPECT_EQ(selection.size(), 0u);
  EXPECT_TRUE(selection.empty());

  auto sub_batch = selection.Materialize();
  EXPECT_TRUE(sub_batch.empty());

  auto scatter_res = ScatterReplace(selection, sub_batch);
  ASSERT_TRUE(scatter_res.ok());
  const auto& full = scatter_res.value();
  ASSERT_EQ(full.size(), 3u);
  EXPECT_EQ(full[0].data, "a");
  EXPECT_EQ(full[1].data, "b");
  EXPECT_EQ(full[2].data, "c");
}

TEST_F(TraceableBatchOperationsTest,
       SelectAndScatterPartialOutOfOrderReplacements) {
  std::vector<TraceableItem<std::string>> anchor = {
      {1, 0, "apple"}, {2, 0, "banana"}, {3, 0, "cherry"}};

  // Select items with length > 5: "banana" (index 1) and "cherry" (index 2)
  auto sel_res = SelectBatch(
      anchor, [](const std::string& text) { return text.size() > 5; });
  ASSERT_TRUE(sel_res.ok());
  const auto& selection = sel_res.value();
  ASSERT_EQ(selection.size(), 2u);

  // Provide replacements in REVERSE order
  std::vector<TraceableItem<std::string>> replacements = {{3, 0, "CHERRY"},
                                                          {2, 0, "BANANA"}};

  auto scatter_res = ScatterReplace(selection, replacements);
  ASSERT_TRUE(scatter_res.ok()) << scatter_res.failure().message;
  const auto& full = scatter_res.value();
  ASSERT_EQ(full.size(), 3u);

  // Unselected item 0 remains "apple"
  EXPECT_EQ(full[0].req_id, 1u);
  EXPECT_EQ(full[0].data, "apple");

  // Selected item 1 updated to "BANANA"
  EXPECT_EQ(full[1].req_id, 2u);
  EXPECT_EQ(full[1].data, "BANANA");

  // Selected item 2 updated to "CHERRY"
  EXPECT_EQ(full[2].req_id, 3u);
  EXPECT_EQ(full[2].data, "CHERRY");
}

TEST_F(TraceableBatchOperationsTest, SelectBatchPredicateFailures) {
  std::vector<TraceableItem<std::string>> anchor = {
      {1, 0, "ok"}, {2, 0, "fail"}, {3, 0, "ok"}};

  // Predicate returning NodeResult failure
  auto res =
      SelectBatch(anchor, [](const std::string& text) -> NodeResult<bool> {
        if (text == "fail") {
          return NodeResult<bool>::Failure(NodeErrorKind::kBusinessError,
                                           "Predicate check failed", -5555);
        }
        return NodeResult<bool>::Success(true);
      });
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.failure().cause_code, -5555);
  ASSERT_TRUE(res.failure().batch_detail.has_value());
  EXPECT_EQ(res.failure().batch_detail->reason,
            BatchFailureReason::kCallbackFailed);
  ASSERT_TRUE(res.failure().batch_detail->key.has_value());
  EXPECT_EQ(res.failure().batch_detail->key->req_id, 2u);

  // Predicate throwing exception
  auto throw_res = SelectBatch(anchor, [](const std::string& text) -> bool {
    if (text == "fail") throw std::runtime_error("Unexpected error");
    return true;
  });
  ASSERT_FALSE(throw_res.ok());
  ASSERT_TRUE(throw_res.failure().batch_detail.has_value());
  EXPECT_EQ(throw_res.failure().batch_detail->reason,
            BatchFailureReason::kCallbackFailed);
  EXPECT_EQ(throw_res.failure().batch_detail->key->req_id, 2u);
}

TEST_F(TraceableBatchOperationsTest, ScatterReplaceErrorValidations) {
  std::vector<TraceableItem<std::string>> anchor = {
      {1, 0, "apple"}, {2, 0, "banana"}, {3, 0, "cherry"}};
  auto sel_res =
      SelectBatch(anchor, [](const std::string& s) { return s == "banana"; });
  ASSERT_TRUE(sel_res.ok());
  const auto& selection = sel_res.value();

  // 1. Duplicate in replacements
  std::vector<TraceableItem<std::string>> dup_repl = {{2, 0, "b1"},
                                                      {2, 0, "b2"}};
  auto res_dup = ScatterReplace(selection, dup_repl);
  ASSERT_FALSE(res_dup.ok());
  EXPECT_EQ(res_dup.failure().batch_detail->reason,
            BatchFailureReason::kDuplicate);

  // 2. Replacement has unselected key (e.g. 1:0 which is in anchor but not
  // selected)
  std::vector<TraceableItem<std::string>> unselected_repl = {
      {1, 0, "new_apple"}};
  auto res_unsel = ScatterReplace(selection, unselected_repl);
  ASSERT_FALSE(res_unsel.ok());
  EXPECT_EQ(res_unsel.failure().batch_detail->reason,
            BatchFailureReason::kUnknown);

  // 3. Replacement has unknown key (not in anchor)
  std::vector<TraceableItem<std::string>> unknown_repl = {{99, 0, "ghost"}};
  auto res_unk = ScatterReplace(selection, unknown_repl);
  ASSERT_FALSE(res_unk.ok());
  EXPECT_EQ(res_unk.failure().batch_detail->reason,
            BatchFailureReason::kUnknown);

  // 4. Replacement missing selected key
  std::vector<TraceableItem<std::string>> empty_repl;
  auto res_miss = ScatterReplace(selection, empty_repl);
  ASSERT_FALSE(res_miss.ok());
  EXPECT_EQ(res_miss.failure().batch_detail->reason,
            BatchFailureReason::kMissing);
}

// ============================================================================
// 4. SplitPayloads Tests
// ============================================================================

TEST_F(TraceableBatchOperationsTest,
       SplitPayloadsMultipleParentsContinuousSubId) {
  std::vector<TraceableItem<std::string>> input = {
      {1, 5, "hello world"},
      {1, 9, "single"},
      {2, 1, "foo bar baz"},
  };

  auto res = SplitPayloads(input, [](const std::string& str) {
    std::vector<std::string> words;
    size_t start = 0;
    while (start < str.size()) {
      size_t space = str.find(' ', start);
      if (space == std::string::npos) {
        words.push_back(str.substr(start));
        break;
      }
      words.push_back(str.substr(start, space - start));
      start = space + 1;
    }
    return words;
  });

  ASSERT_TRUE(res.ok()) << res.failure().message;
  const auto& result = res.value();

  // Children check
  ASSERT_EQ(result.children.size(), 6u);
  // req 1 item 0: 2 words -> sub_id 0, 1
  EXPECT_EQ(result.children[0].req_id, 1u);
  EXPECT_EQ(result.children[0].sub_id, 0u);
  EXPECT_EQ(result.children[0].data, "hello");

  EXPECT_EQ(result.children[1].req_id, 1u);
  EXPECT_EQ(result.children[1].sub_id, 1u);
  EXPECT_EQ(result.children[1].data, "world");

  // req 1 item 1: 1 word -> sub_id 2 (continuous for req 1!)
  EXPECT_EQ(result.children[2].req_id, 1u);
  EXPECT_EQ(result.children[2].sub_id, 2u);
  EXPECT_EQ(result.children[2].data, "single");

  // req 2 item 0: 3 words -> sub_id starts at 0 for req 2!
  EXPECT_EQ(result.children[3].req_id, 2u);
  EXPECT_EQ(result.children[3].sub_id, 0u);
  EXPECT_EQ(result.children[3].data, "foo");

  EXPECT_EQ(result.children[4].req_id, 2u);
  EXPECT_EQ(result.children[4].sub_id, 1u);
  EXPECT_EQ(result.children[4].data, "bar");

  EXPECT_EQ(result.children[5].req_id, 2u);
  EXPECT_EQ(result.children[5].sub_id, 2u);
  EXPECT_EQ(result.children[5].data, "baz");

  // Counts check
  ASSERT_EQ(result.counts.size(), 3u);
  EXPECT_EQ(result.counts[0].req_id, 1u);
  EXPECT_EQ(result.counts[0].sub_id, 5u);
  EXPECT_EQ(result.counts[0].data, 2);

  EXPECT_EQ(result.counts[1].req_id, 1u);
  EXPECT_EQ(result.counts[1].sub_id, 9u);
  EXPECT_EQ(result.counts[1].data, 1);

  EXPECT_EQ(result.counts[2].req_id, 2u);
  EXPECT_EQ(result.counts[2].sub_id, 1u);
  EXPECT_EQ(result.counts[2].data, 3);
}

TEST_F(TraceableBatchOperationsTest,
       SplitPayloadsInterleavedRequestsCounterNotReset) {
  std::vector<TraceableItem<std::string>> input = {
      {1, 0, "a b"},
      {2, 0, "x"},
      {1, 1, "c d"},
  };

  auto res = SplitPayloads(input, [](const std::string& str) {
    if (str == "a b") return std::vector<std::string>{"a", "b"};
    if (str == "x") return std::vector<std::string>{"x"};
    if (str == "c d") return std::vector<std::string>{"c", "d"};
    return std::vector<std::string>{};
  });

  ASSERT_TRUE(res.ok());
  const auto& children = res.value().children;
  ASSERT_EQ(children.size(), 5u);

  EXPECT_EQ(children[0].req_id, 1u);
  EXPECT_EQ(children[0].sub_id, 0u);

  EXPECT_EQ(children[1].req_id, 1u);
  EXPECT_EQ(children[1].sub_id, 1u);

  EXPECT_EQ(children[2].req_id, 2u);
  EXPECT_EQ(children[2].sub_id, 0u);

  // req 1 resumed: sub_id must be 2, 3!
  EXPECT_EQ(children[3].req_id, 1u);
  EXPECT_EQ(children[3].sub_id, 2u);

  EXPECT_EQ(children[4].req_id, 1u);
  EXPECT_EQ(children[4].sub_id, 3u);
}

TEST_F(TraceableBatchOperationsTest, SplitPayloadsZeroChildrenAndEmptyInput) {
  // Zero children is legal
  std::vector<TraceableItem<std::string>> input = {{1, 0, "empty"}};
  auto res = SplitPayloads(
      input, [](const std::string&) { return std::vector<std::string>{}; });
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res.value().children.empty());
  ASSERT_EQ(res.value().counts.size(), 1u);
  EXPECT_EQ(res.value().counts[0].data, 0);

  // Empty input
  std::vector<TraceableItem<std::string>> empty_input;
  auto empty_res = SplitPayloads(empty_input, [](const std::string&) {
    return std::vector<std::string>{"never"};
  });
  ASSERT_TRUE(empty_res.ok());
  EXPECT_TRUE(empty_res.value().children.empty());
  EXPECT_TRUE(empty_res.value().counts.empty());
}

TEST_F(TraceableBatchOperationsTest,
       SplitPayloadsCallbackFailurePreservesCause) {
  std::vector<TraceableItem<std::string>> input = {
      {1, 0, "ok"}, {2, 3, "fail"}, {3, 0, "ok"}};

  auto res = SplitPayloads(
      input, [](const std::string& s) -> NodeResult<std::vector<std::string>> {
        if (s == "fail") {
          return NodeResult<std::vector<std::string>>::Failure(
              NodeErrorKind::kBusinessError, "custom splitter error", -7788);
        }
        return NodeResult<std::vector<std::string>>::Success(
            std::vector<std::string>{s});
      });

  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.failure().cause_code, -7788);
  ASSERT_TRUE(res.failure().batch_detail.has_value());
  EXPECT_EQ(res.failure().batch_detail->operation, "SplitPayloads");
  EXPECT_EQ(res.failure().batch_detail->reason,
            BatchFailureReason::kCallbackFailed);
  ASSERT_TRUE(res.failure().batch_detail->key.has_value());
  EXPECT_EQ(res.failure().batch_detail->key->req_id, 2u);
  EXPECT_EQ(res.failure().batch_detail->key->sub_id, 3u);
}

TEST_F(TraceableBatchOperationsTest, SplitPayloadsSubIdOverflowSeam) {
  std::vector<TraceableItem<std::string>> input = {{1, 0, "split"}};

  // Set near-boundary sub_id using internal test seam
  std::unordered_map<uint32_t, uint64_t> initial = {
      {1, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())}};

  auto res = detail::SplitPayloadsInternal(
      input,
      [](const std::string&) {
        // Generates 2 children: first fits at UINT32_MAX, second overflows!
        return std::vector<std::string>{"chunk1", "chunk2"};
      },
      initial);

  ASSERT_FALSE(res.ok());
  ASSERT_TRUE(res.failure().batch_detail.has_value());
  EXPECT_EQ(res.failure().batch_detail->operation, "SplitPayloads");
  EXPECT_EQ(res.failure().batch_detail->reason,
            BatchFailureReason::kSubIdOverflow);
  ASSERT_TRUE(res.failure().batch_detail->key.has_value());
  EXPECT_EQ(res.failure().batch_detail->key->req_id, 1u);
}

TEST_F(TraceableBatchOperationsTest, CheckedSplitCountInt32Boundaries) {
  const size_t max_count =
      static_cast<size_t>(std::numeric_limits<int32_t>::max());
  for (size_t count : {size_t{0}, max_count - 1, max_count}) {
    SCOPED_TRACE(count);
    auto result = detail::CheckedSplitCount(count, TraceableItemKey{42, 9});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), static_cast<int32_t>(count));
  }
  for (size_t count : {max_count + 1, std::numeric_limits<size_t>::max()}) {
    SCOPED_TRACE(count);
    auto result = detail::CheckedSplitCount(count, TraceableItemKey{42, 9});
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.failure().kind, NodeErrorKind::kBusinessError);
    EXPECT_EQ(result.failure().cause_code, 0);
    EXPECT_NE(result.failure().message.find("Int32 capacity"),
              std::string::npos);
    ASSERT_TRUE(result.failure().batch_detail.has_value());
    const auto& detail = *result.failure().batch_detail;
    EXPECT_EQ(detail.operation, "SplitPayloads");
    EXPECT_EQ(detail.reason, BatchFailureReason::kCountOverflow);
    ASSERT_TRUE(detail.key.has_value());
    EXPECT_EQ(detail.key->req_id, 42u);
    EXPECT_EQ(detail.key->sub_id, 9u);
    EXPECT_NE(result.failure().FormatDiagnostic("").find("req_id=42, sub_id=9"),
              std::string::npos);
  }
}

TEST_F(TraceableBatchOperationsTest,
       SplitPayloadsMaxSubIdThenZeroChildrenSucceeds) {
  const TextBatch input = {{42, 3, "last child"}, {42, 9, "empty"}};
  const std::unordered_map<uint32_t, uint64_t> initial = {
      {42, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())}};
  auto result = detail::SplitPayloadsInternal(
      input,
      [](const std::string& payload) {
        return payload == "empty" ? std::vector<std::string>{}
                                  : std::vector<std::string>{payload};
      },
      initial);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.value().children.size(), 1u);
  EXPECT_EQ(result.value().children[0].req_id, 42u);
  EXPECT_EQ(result.value().children[0].sub_id,
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(result.value().children[0].data, "last child");
  ASSERT_EQ(result.value().counts.size(), 2u);
  EXPECT_EQ(result.value().counts[0].req_id, 42u);
  EXPECT_EQ(result.value().counts[0].sub_id, 3u);
  EXPECT_EQ(result.value().counts[0].data, 1);
  EXPECT_EQ(result.value().counts[1].req_id, 42u);
  EXPECT_EQ(result.value().counts[1].sub_id, 9u);
  EXPECT_EQ(result.value().counts[1].data, 0);
}

// ============================================================================
// 5. View and Ownership Tests
// ============================================================================

// SFINAE probes to detect deleted overloads for factory functions
template <typename L, typename R, typename = void>
struct CanJoinByItem : std::false_type {};
template <typename L, typename R>
struct CanJoinByItem<
    L, R,
    std::void_t<decltype(JoinByItem(std::declval<L>(), std::declval<R>()))>>
    : std::true_type {};

template <typename A, typename M, typename = void>
struct CanGroupByRequest : std::false_type {};
template <typename A, typename M>
struct CanGroupByRequest<
    A, M,
    std::void_t<decltype(GroupByRequest(std::declval<A>(), std::declval<M>()))>>
    : std::true_type {};

template <typename B, typename P, typename = void>
struct CanSelectBatch : std::false_type {};
template <typename B, typename P>
struct CanSelectBatch<
    B, P,
    std::void_t<decltype(SelectBatch(std::declval<B>(), std::declval<P>()))>>
    : std::true_type {};

template <typename G, typename I, typename = void>
struct CanAddAnchor : std::false_type {};
template <typename G, typename I>
struct CanAddAnchor<
    G, I, std::void_t<decltype(std::declval<G>().AddAnchor(std::declval<I>()))>>
    : std::true_type {};

template <typename G, typename I, typename = void>
struct CanAddMember : std::false_type {};
template <typename G, typename I>
struct CanAddMember<
    G, I, std::void_t<decltype(std::declval<G>().AddMember(std::declval<I>()))>>
    : std::true_type {};

TEST_F(TraceableBatchOperationsTest, CompileTimeRejectionOfRvalues) {
  using Batch = std::vector<TraceableItem<std::string>>;
  using Pred = bool (*)(const std::string&);
  using Item = TraceableItem<std::string>;
  using Group = RequestGroup<std::string, std::string>;

  // ItemJoinView must reject non-const and const rvalues
  static_assert(
      std::is_constructible_v<ItemJoinView<std::string, std::string>,
                              const Batch&, const Batch&,
                              std::vector<JoinedRow<std::string, std::string>>>,
      "ItemJoinView must accept const lvalues");
  static_assert(
      std::is_constructible_v<ItemJoinView<std::string, std::string>, Batch&,
                              Batch&,
                              std::vector<JoinedRow<std::string, std::string>>>,
      "ItemJoinView must accept non-const lvalues");
  static_assert(
      !std::is_constructible_v<
          ItemJoinView<std::string, std::string>, Batch&&, const Batch&,
          std::vector<JoinedRow<std::string, std::string>>>,
      "ItemJoinView must reject rvalue left");
  static_assert(
      !std::is_constructible_v<
          ItemJoinView<std::string, std::string>, const Batch&&, const Batch&,
          std::vector<JoinedRow<std::string, std::string>>>,
      "ItemJoinView must reject const rvalue left");
  static_assert(!std::is_constructible_v<
                    ItemJoinView<std::string, std::string>, const Batch&,
                    Batch&&, std::vector<JoinedRow<std::string, std::string>>>,
                "ItemJoinView must reject rvalue right");
  static_assert(
      !std::is_constructible_v<
          ItemJoinView<std::string, std::string>, const Batch&, const Batch&&,
          std::vector<JoinedRow<std::string, std::string>>>,
      "ItemJoinView must reject const rvalue right");
  static_assert(!std::is_constructible_v<
                    ItemJoinView<std::string, std::string>, Batch&&, Batch&&,
                    std::vector<JoinedRow<std::string, std::string>>>,
                "ItemJoinView must reject both non-const rvalues");
  static_assert(
      !std::is_constructible_v<
          ItemJoinView<std::string, std::string>, Batch&&, const Batch&&,
          std::vector<JoinedRow<std::string, std::string>>>,
      "ItemJoinView must reject rvalue left, const rvalue right");
  static_assert(!std::is_constructible_v<
                    ItemJoinView<std::string, std::string>, const Batch&&,
                    Batch&&, std::vector<JoinedRow<std::string, std::string>>>,
                "ItemJoinView must reject const rvalue left, rvalue right");
  static_assert(
      !std::is_constructible_v<
          ItemJoinView<std::string, std::string>, const Batch&&, const Batch&&,
          std::vector<JoinedRow<std::string, std::string>>>,
      "ItemJoinView must reject both const rvalues");

  // JoinByItem factory function must reject non-const and const rvalues
  static_assert(CanJoinByItem<const Batch&, const Batch&>::value,
                "JoinByItem must accept const lvalues");
  static_assert(CanJoinByItem<Batch&, Batch&>::value,
                "JoinByItem must accept non-const lvalues");
  static_assert(CanJoinByItem<Batch&, const Batch&>::value,
                "JoinByItem must accept mixed lvalues");
  static_assert(CanJoinByItem<const Batch&, Batch&>::value,
                "JoinByItem must accept mixed lvalues");
  static_assert(!CanJoinByItem<Batch&&, const Batch&>::value,
                "JoinByItem must reject rvalue left");
  static_assert(!CanJoinByItem<const Batch&&, const Batch&>::value,
                "JoinByItem must reject const rvalue left");
  static_assert(!CanJoinByItem<const Batch&, Batch&&>::value,
                "JoinByItem must reject rvalue right");
  static_assert(!CanJoinByItem<const Batch&, const Batch&&>::value,
                "JoinByItem must reject const rvalue right");
  static_assert(!CanJoinByItem<Batch&&, Batch&&>::value,
                "JoinByItem must reject both non-const rvalues");
  static_assert(!CanJoinByItem<Batch&&, const Batch&&>::value,
                "JoinByItem must reject rvalue left, const rvalue right");
  static_assert(!CanJoinByItem<const Batch&&, Batch&&>::value,
                "JoinByItem must reject const rvalue left, rvalue right");
  static_assert(!CanJoinByItem<const Batch&&, const Batch&&>::value,
                "JoinByItem must reject both const rvalues");

  // RequestGroupView must reject non-const and const rvalues
  static_assert(
      std::is_constructible_v<
          RequestGroupView<std::string, std::string>, const Batch&,
          const Batch&, std::vector<RequestGroup<std::string, std::string>>,
          std::vector<size_t>, std::unordered_map<uint32_t, size_t>>,
      "RequestGroupView must accept const lvalues");
  static_assert(std::is_constructible_v<
                    RequestGroupView<std::string, std::string>, Batch&, Batch&,
                    std::vector<RequestGroup<std::string, std::string>>,
                    std::vector<size_t>, std::unordered_map<uint32_t, size_t>>,
                "RequestGroupView must accept non-const lvalues");
  static_assert(
      !std::is_constructible_v<
          RequestGroupView<std::string, std::string>, Batch&&, const Batch&,
          std::vector<RequestGroup<std::string, std::string>>,
          std::vector<size_t>, std::unordered_map<uint32_t, size_t>>,
      "RequestGroupView must reject rvalue anchor");
  static_assert(
      !std::is_constructible_v<
          RequestGroupView<std::string, std::string>, const Batch&&,
          const Batch&, std::vector<RequestGroup<std::string, std::string>>,
          std::vector<size_t>, std::unordered_map<uint32_t, size_t>>,
      "RequestGroupView must reject const rvalue anchor");
  static_assert(
      !std::is_constructible_v<
          RequestGroupView<std::string, std::string>, const Batch&, Batch&&,
          std::vector<RequestGroup<std::string, std::string>>,
          std::vector<size_t>, std::unordered_map<uint32_t, size_t>>,
      "RequestGroupView must reject rvalue members");
  static_assert(
      !std::is_constructible_v<
          RequestGroupView<std::string, std::string>, const Batch&,
          const Batch&&, std::vector<RequestGroup<std::string, std::string>>,
          std::vector<size_t>, std::unordered_map<uint32_t, size_t>>,
      "RequestGroupView must reject const rvalue members");
  static_assert(
      !std::is_constructible_v<
          RequestGroupView<std::string, std::string>, Batch&&, Batch&&,
          std::vector<RequestGroup<std::string, std::string>>,
          std::vector<size_t>, std::unordered_map<uint32_t, size_t>>,
      "RequestGroupView must reject both non-const rvalues");
  static_assert(
      !std::is_constructible_v<
          RequestGroupView<std::string, std::string>, Batch&&, const Batch&&,
          std::vector<RequestGroup<std::string, std::string>>,
          std::vector<size_t>, std::unordered_map<uint32_t, size_t>>,
      "RequestGroupView must reject rvalue anchor, const rvalue members");
  static_assert(
      !std::is_constructible_v<
          RequestGroupView<std::string, std::string>, const Batch&&, Batch&&,
          std::vector<RequestGroup<std::string, std::string>>,
          std::vector<size_t>, std::unordered_map<uint32_t, size_t>>,
      "RequestGroupView must reject const rvalue anchor, rvalue members");
  static_assert(
      !std::is_constructible_v<
          RequestGroupView<std::string, std::string>, const Batch&&,
          const Batch&&, std::vector<RequestGroup<std::string, std::string>>,
          std::vector<size_t>, std::unordered_map<uint32_t, size_t>>,
      "RequestGroupView must reject both const rvalues");

  // GroupByRequest factory function must reject non-const and const rvalues
  static_assert(CanGroupByRequest<const Batch&, const Batch&>::value,
                "GroupByRequest must accept const lvalues");
  static_assert(CanGroupByRequest<Batch&, Batch&>::value,
                "GroupByRequest must accept non-const lvalues");
  static_assert(CanGroupByRequest<Batch&, const Batch&>::value,
                "GroupByRequest must accept mixed lvalues");
  static_assert(CanGroupByRequest<const Batch&, Batch&>::value,
                "GroupByRequest must accept mixed lvalues");
  static_assert(!CanGroupByRequest<Batch&&, const Batch&>::value,
                "GroupByRequest must reject rvalue anchor");
  static_assert(!CanGroupByRequest<const Batch&&, const Batch&>::value,
                "GroupByRequest must reject const rvalue anchor");
  static_assert(!CanGroupByRequest<const Batch&, Batch&&>::value,
                "GroupByRequest must reject rvalue members");
  static_assert(!CanGroupByRequest<const Batch&, const Batch&&>::value,
                "GroupByRequest must reject const rvalue members");
  static_assert(!CanGroupByRequest<Batch&&, Batch&&>::value,
                "GroupByRequest must reject both non-const rvalues");
  static_assert(
      !CanGroupByRequest<Batch&&, const Batch&&>::value,
      "GroupByRequest must reject rvalue anchor, const rvalue members");
  static_assert(
      !CanGroupByRequest<const Batch&&, Batch&&>::value,
      "GroupByRequest must reject const rvalue anchor, rvalue members");
  static_assert(!CanGroupByRequest<const Batch&&, const Batch&&>::value,
                "GroupByRequest must reject both const rvalues");

  // Selection must reject non-const and const rvalues
  static_assert(!std::is_constructible_v<Selection<std::string>, Batch&&,
                                         std::vector<size_t>>,
                "Selection must reject rvalue anchor");
  static_assert(!std::is_constructible_v<Selection<std::string>, const Batch&&,
                                         std::vector<size_t>>,
                "Selection must reject const rvalue anchor");

  // SelectBatch factory function must reject non-const and const rvalues
  static_assert(CanSelectBatch<const Batch&, Pred>::value,
                "SelectBatch must accept const lvalue");
  static_assert(CanSelectBatch<Batch&, Pred>::value,
                "SelectBatch must accept non-const lvalue");
  static_assert(!CanSelectBatch<Batch&&, Pred>::value,
                "SelectBatch must reject rvalue anchor");
  static_assert(!CanSelectBatch<const Batch&&, Pred>::value,
                "SelectBatch must reject const rvalue anchor");

  // RequestGroup AddAnchor and AddMember must reject rvalue items
  static_assert(CanAddAnchor<Group&, const Item&>::value,
                "AddAnchor must accept const lvalue item");
  static_assert(CanAddAnchor<Group&, Item&>::value,
                "AddAnchor must accept non-const lvalue item");
  static_assert(!CanAddAnchor<Group&, Item&&>::value,
                "AddAnchor must reject rvalue item");
  static_assert(!CanAddAnchor<Group&, const Item&&>::value,
                "AddAnchor must reject const rvalue item");

  static_assert(CanAddMember<Group&, const Item&>::value,
                "AddMember must accept const lvalue item");
  static_assert(CanAddMember<Group&, Item&>::value,
                "AddMember must accept non-const lvalue item");
  static_assert(!CanAddMember<Group&, Item&&>::value,
                "AddMember must reject rvalue item");
  static_assert(!CanAddMember<Group&, const Item&&>::value,
                "AddMember must reject const rvalue item");

  // JoinedRow must reject rvalue left items
  static_assert(std::is_constructible_v<JoinedRow<std::string, std::string>,
                                        const Item&, const Item*>,
                "JoinedRow must accept const lvalue left");
  static_assert(std::is_constructible_v<JoinedRow<std::string, std::string>,
                                        Item&, const Item*>,
                "JoinedRow must accept non-const lvalue left");
  static_assert(!std::is_constructible_v<JoinedRow<std::string, std::string>,
                                         Item&&, const Item*>,
                "JoinedRow must reject rvalue left");
  static_assert(!std::is_constructible_v<JoinedRow<std::string, std::string>,
                                         const Item&&, const Item*>,
                "JoinedRow must reject const rvalue left");
}

TEST_F(TraceableBatchOperationsTest,
       MaterializeIndependentOfSelectionLifetime) {
  std::vector<TraceableItem<std::string>> anchor = {{1, 0, "persist_me"}};
  std::vector<TraceableItem<std::string>> materialized;
  {
    auto sel_res = SelectBatch(anchor, [](const std::string&) { return true; });
    ASSERT_TRUE(sel_res.ok());
    materialized = sel_res.value().Materialize();
  }
  // sel_res and Selection are now out of scope
  ASSERT_EQ(materialized.size(), 1u);
  EXPECT_EQ(materialized[0].req_id, 1u);
  EXPECT_EQ(materialized[0].data, "persist_me");
}

// ============================================================================
// 6. Starter Nodes Verification with Mock LLM
// ============================================================================

TEST_F(TraceableBatchOperationsTest, StarterBatchJoinNodeHarness) {
  auto mock_llm = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("StarterBatchJoinNode");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_llm);

  harness.TextInput("questions", {"what is capital", "who wrote hamlet"});
  harness.TextInput("attributes", {"author", "geography"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();

  EXPECT_EQ(mock_llm->call_count, 1);
  const auto& prompts = mock_llm->last_prompts;
  ASSERT_EQ(prompts.size(), 2u);
  EXPECT_EQ(prompts[0].data, "what is capital [attr: author]");
  EXPECT_EQ(prompts[1].data, "who wrote hamlet [attr: geography]");
}

TEST_F(TraceableBatchOperationsTest, StarterBatchGroupNodeHarness) {
  auto mock_llm = std::make_shared<CountingMockLlmModel>();
  NodeHarness harness("StarterBatchGroupNode");
  harness.Config({{"bind_model", "test_llm"}});
  harness.BindModel("test_llm", mock_llm);

  // Interleaved queries
  harness.CustomInput("queries",
                      TextBatch{{10, 0, "q1"}, {20, 0, "q2"}, {10, 1, "q3"}});
  // Aggregated references
  harness.CustomInput(
      "references",
      TextBatch{{10, 0, "ref1"}, {10, 1, "ref2"}, {20, 0, "ref3"}});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();

  EXPECT_EQ(mock_llm->call_count, 1);
  const auto& prompts = mock_llm->last_prompts;
  ASSERT_EQ(prompts.size(), 3u);
  // req 10 query 0: context ref1 + ref2
  EXPECT_EQ(prompts[0].req_id, 10u);
  EXPECT_EQ(prompts[0].sub_id, 0u);
  EXPECT_EQ(prompts[0].data, "ref1\nref2\nq1");

  // req 20 query 0: context ref3
  EXPECT_EQ(prompts[1].req_id, 20u);
  EXPECT_EQ(prompts[1].sub_id, 0u);
  EXPECT_EQ(prompts[1].data, "ref3\nq2");

  // req 10 query 1: context ref1 + ref2
  EXPECT_EQ(prompts[2].req_id, 10u);
  EXPECT_EQ(prompts[2].sub_id, 1u);
  EXPECT_EQ(prompts[2].data, "ref1\nref2\nq3");
}

TEST_F(TraceableBatchOperationsTest,
       StarterBatchSelectScatterNodeHarnessNoneSelected) {
  auto generator = std::make_shared<CountingMockLlmModel>();
  auto polisher = std::make_shared<CountingMockLlmModel>();

  NodeHarness harness("StarterBatchSelectScatterNode");
  harness.Config(
      {{"bind_model", "test_gen_llm"}, {"polish_model", "test_pol_llm"}});
  harness.BindModel("test_gen_llm", generator);
  harness.BindModel("test_pol_llm", polisher);

  // Generator produces answers without [POLISH]
  harness.TextInput("input", {"hello", "world"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();

  EXPECT_EQ(generator->call_count, 1);
  EXPECT_EQ(polisher->call_count, 0);  // Second call skipped!
  EXPECT_EQ(result.TextValues("output"),
            (std::vector<std::string>{"ans:hello", "ans:world"}));
}

TEST_F(TraceableBatchOperationsTest,
       StarterBatchSelectScatterNodeHarnessPartialPolishing) {
  class PolishingMockLlm final : public ILlmModel {
   public:
    const std::string& ModelType() const noexcept override {
      static const std::string t = "polishing_mock_llm";
      return t;
    }
    const std::string& Capability() const noexcept override {
      static const std::string cap = "llm";
      return cap;
    }
    InferenceConcurrency Concurrency() const noexcept override {
      return InferenceConcurrency::kConcurrent;
    }
    size_t GetMaxBatchSize() const noexcept override { return 8; }

    int Generate(const TextBatch& prompts, const GenerateOptions&,
                 TextBatch* outputs) noexcept override {
      ++call_count;
      last_prompts = prompts;
      if (outputs) {
        outputs->clear();
        for (const auto& item : prompts) {
          if (is_generator) {
            // If item has "bad", output with [POLISH]
            std::string text = (item.data.find("bad") != std::string::npos)
                                   ? (item.data + " [POLISH]")
                                   : ("clean:" + item.data);
            outputs->emplace_back(item.req_id, item.sub_id, std::move(text));
          } else {
            // Polisher: replace [POLISH] with polished version
            std::string text = item.data;
            size_t tag = text.find(" [POLISH]");
            if (tag != std::string::npos) text.erase(tag);
            outputs->emplace_back(item.req_id, item.sub_id, "polished:" + text);
          }
        }
      }
      return 0;
    }

    bool is_generator = true;
    mutable int call_count = 0;
    mutable TextBatch last_prompts;
  };

  auto generator = std::make_shared<PolishingMockLlm>();
  generator->is_generator = true;
  auto polisher = std::make_shared<PolishingMockLlm>();
  polisher->is_generator = false;

  NodeHarness harness("StarterBatchSelectScatterNode");
  harness.Config(
      {{"bind_model", "test_gen_llm"}, {"polish_model", "test_pol_llm"}});
  harness.BindModel("test_gen_llm", generator);
  harness.BindModel("test_pol_llm", polisher);

  // Input 1 is clean, input 2 is bad, input 3 is clean
  harness.TextInput("input", {"good1", "bad2", "good3"});

  auto result = harness.Run();
  ASSERT_TRUE(result.ok()) << result.diagnostic();

  // Generator called once with all 3 items
  EXPECT_EQ(generator->call_count, 1);
  EXPECT_EQ(generator->last_prompts.size(), 3u);

  // Polisher called once ONLY on the 1 selected item ("bad2 [POLISH]")
  EXPECT_EQ(polisher->call_count, 1);
  ASSERT_EQ(polisher->last_prompts.size(), 1u);
  EXPECT_EQ(polisher->last_prompts[0].req_id, 102u);
  EXPECT_EQ(polisher->last_prompts[0].sub_id, 0u);
  EXPECT_EQ(polisher->last_prompts[0].data, "bad2 [POLISH]");

  // Full output has clean items unchanged, polished item replaced, in original
  // order
  auto outputs = result.TextValues("output");
  ASSERT_EQ(outputs.size(), 3u);
  EXPECT_EQ(outputs[0], "clean:good1");
  EXPECT_EQ(outputs[1], "polished:bad2");
  EXPECT_EQ(outputs[2], "clean:good3");
}

TEST_F(TraceableBatchOperationsTest,
       StarterBatchSelectScatterNodePolisherFailure) {
  auto generator = std::make_shared<CountingMockLlmModel>();
  auto polisher = std::make_shared<CountingMockLlmModel>();
  polisher->always_fail = true;

  // Custom node with polish_tag = "ans:" so generator outputs get selected
  NodeHarness harness("StarterBatchSelectScatterNode");
  harness.Config({{"bind_model", "test_gen_llm"},
                  {"polish_model", "test_pol_llm"},
                  {"polish_tag", "ans:"}});
  harness.BindModel("test_gen_llm", generator);
  harness.BindModel("test_pol_llm", polisher);

  harness.TextInput("input", {"item1"});

  auto result = harness.Run();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(generator->call_count, 1);
  EXPECT_EQ(polisher->call_count, 1);
}

// ============================================================================
// 7. Functional Integration & Additional RFC-0055 Assertions
// ============================================================================

struct DirectSubBatchInputs {
  const TextBatch* input = nullptr;
};
struct DirectSubBatchOptions {};

NodeResult<TextBatch> RunDirectSubBatch(const DirectSubBatchInputs& in,
                                        const DirectSubBatchOptions&) {
  if (!in.input || in.input->empty()) {
    return NodeResult<TextBatch>::Success(TextBatch{});
  }
  auto sel =
      SelectBatch(*in.input, [](const std::string& s) { return s == "keep"; });
  if (!sel.ok()) {
    return NodeResult<TextBatch>::Failure(std::move(sel).ExtractFailure());
  }
  // Intentionally return sub-batch without ScatterReplace to test fail-closed
  // PreservedOutput count validation.
  return sel.value().Materialize();
}

auto DirectSubBatchSpec() {
  return MakeBatchSpec(InputsOf<DirectSubBatchInputs>({
                           Required("input", &DirectSubBatchInputs::input),
                       }),
                       PreservedOutput<TextBatch>("output", "input"),
                       Parameters<DirectSubBatchOptions>({}),
                       &RunDirectSubBatch)
      .Description("Test fixture for unscattered sub-batch rejection");
}

REGISTER_FUNCTION_NODE(DirectSubBatchTestNode, DirectSubBatchSpec());

TEST_F(TraceableBatchOperationsTest, FunctionNodeRejectsDirectSubBatchReturn) {
  NodeHarness harness("DirectSubBatchTestNode");
  harness.TextInput("input", {"keep", "drop"});

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.process_code(),
            node_error::author_node::kOutputCountMismatch);
  EXPECT_EQ(result.Output<TextBatch>("output"), nullptr);
}

TEST_F(TraceableBatchOperationsTest, DeterministicSeedRandomPermutation) {
  std::vector<TraceableItem<std::string>> left;
  std::vector<TraceableItem<std::string>> right;
  for (uint32_t i = 0; i < 20; ++i) {
    left.emplace_back(i + 1, 0, "q_" + std::to_string(i));
    right.emplace_back(i + 1, 0, "a_" + std::to_string(i));
  }

  std::mt19937 rng(42);
  std::shuffle(right.begin(), right.end(), rng);

  auto join_res = JoinByItem(left, right, JoinMode::kExact);
  ASSERT_TRUE(join_res.ok());
  const auto& view = join_res.value();
  ASSERT_EQ(view.size(), 20u);

  for (size_t i = 0; i < 20; ++i) {
    EXPECT_EQ(view[i].req_id(), i + 1);
    EXPECT_EQ(view[i].sub_id(), 0u);
    EXPECT_EQ(view[i].left_payload(), "q_" + std::to_string(i));
    ASSERT_TRUE(view[i].has_right());
    EXPECT_EQ(*view[i].right_payload(), "a_" + std::to_string(i));
  }
}

TEST_F(TraceableBatchOperationsTest, SelectBatchCatchesNonStdException) {
  std::vector<TraceableItem<std::string>> anchor = {{1, 0, "throw_int"}};
  auto res = SelectBatch(anchor, [](const std::string&) -> bool { throw 42; });
  ASSERT_FALSE(res.ok());
  ASSERT_TRUE(res.failure().batch_detail.has_value());
  EXPECT_EQ(res.failure().batch_detail->reason,
            BatchFailureReason::kCallbackFailed);
  EXPECT_EQ(res.failure().batch_detail->key->req_id, 1u);
}

TEST_F(TraceableBatchOperationsTest, SplitPayloadsCatchesNonStdException) {
  std::vector<TraceableItem<std::string>> input = {{1, 0, "throw_int"}};
  auto res = SplitPayloads(
      input, [](const std::string&) -> std::vector<std::string> { throw 42; });
  ASSERT_FALSE(res.ok());
  ASSERT_TRUE(res.failure().batch_detail.has_value());
  EXPECT_EQ(res.failure().batch_detail->reason,
            BatchFailureReason::kCallbackFailed);
  EXPECT_EQ(res.failure().batch_detail->key->req_id, 1u);
}

TEST_F(TraceableBatchOperationsTest, HashDistributionQualityForSubIdZero) {
  TraceableItemKeyHash hasher;
  std::unordered_set<size_t> hashes;
  for (uint32_t req = 1; req <= 100; ++req) {
    hashes.insert(hasher(TraceableItemKey{req, 0}));
  }
  EXPECT_EQ(hashes.size(), 100u);
}

TEST_F(TraceableBatchOperationsTest, RequestGroupViewHasReqIdAndContains) {
  std::vector<TraceableItem<std::string>> anchor = {{10, 0, "A0"},
                                                    {20, 0, "B0"}};
  std::vector<TraceableItem<std::string>> members = {{10, 0, "m0"}};
  auto res = GroupByRequest(anchor, members);
  ASSERT_TRUE(res.ok());
  const auto& view = res.value();
  EXPECT_TRUE(view.HasReqId(10));
  EXPECT_TRUE(view.Contains(20));
  EXPECT_FALSE(view.HasReqId(30));
  EXPECT_FALSE(view.Contains(999));
}

TEST_F(TraceableBatchOperationsTest,
       SelectBatchTraceableItemPredicateSupported) {
  std::vector<TraceableItem<std::string>> anchor = {
      {1, 0, "alpha"}, {2, 0, "beta"}, {3, 0, "gamma"}};

  // 1. Predicate taking const TraceableItem<Payload>& returning bool
  auto res_bool = SelectBatch(
      anchor,
      [](const TraceableItem<std::string>& item) { return item.req_id == 2; });
  ASSERT_TRUE(res_bool.ok());
  EXPECT_EQ(res_bool.value().size(), 1u);
  EXPECT_EQ(res_bool.value()[0].data, "beta");

  // 2. Predicate taking const TraceableItem<Payload>& returning
  // NodeResult<bool>
  auto res_node = SelectBatch(
      anchor, [](const TraceableItem<std::string>& item) -> NodeResult<bool> {
        if (item.sub_id != 0) {
          return NodeResult<bool>::Failure(NodeErrorKind::kBusinessError,
                                           "Invalid sub_id");
        }
        return NodeResult<bool>::Success(item.req_id >= 2);
      });
  ASSERT_TRUE(res_node.ok());
  EXPECT_EQ(res_node.value().size(), 2u);
  EXPECT_EQ(res_node.value()[0].data, "beta");
  EXPECT_EQ(res_node.value()[1].data, "gamma");
}

TEST_F(TraceableBatchOperationsTest, BatchFailureDetailFormatDiagnosticDirect) {
  // 1. Failure with batch_detail containing key
  NodeFailure f1(
      NodeErrorKind::kBusinessError, "predicate failed",
      BatchFailureDetail{"SelectBatch", BatchFailureReason::kCallbackFailed,
                         TraceableItemKey{42, 9}},
      -7788);
  EXPECT_EQ(
      f1.FormatDiagnostic("fallback"),
      "SelectBatch callback_failed for req_id=42, sub_id=9: predicate failed");

  // 2. Failure where message already contains req_id - avoid redundant
  // formatting
  NodeFailure f2(
      NodeErrorKind::kInputError,
      "JoinByItem right batch missing key present in left: req_id=2, sub_id=0",
      BatchFailureDetail{"JoinByItem", BatchFailureReason::kMissing,
                         TraceableItemKey{2, 0}});
  EXPECT_EQ(
      f2.FormatDiagnostic("fallback"),
      "JoinByItem right batch missing key present in left: req_id=2, sub_id=0");

  // 3. Failure without batch_detail
  NodeFailure f3(NodeErrorKind::kBusinessError, "plain error", -1234);
  EXPECT_EQ(f3.FormatDiagnostic("fallback"), "plain error");

  // 4. Failure with empty message and batch_detail with key
  NodeFailure f4(
      NodeErrorKind::kBusinessError, "",
      BatchFailureDetail{"SplitPayloads", BatchFailureReason::kCallbackFailed,
                         TraceableItemKey{10, 3}},
      -6677);
  EXPECT_EQ(f4.FormatDiagnostic("fallback"),
            "SplitPayloads callback_failed for req_id=10, sub_id=3: fallback");

  // 5. Message with different req_id substring collision (req_id=4 vs
  // req_id=400)
  NodeFailure f5(
      NodeErrorKind::kBusinessError, "failed on req_id=400",
      BatchFailureDetail{"SelectBatch", BatchFailureReason::kCallbackFailed,
                         TraceableItemKey{4, 9}},
      -7788);
  EXPECT_EQ(f5.FormatDiagnostic("fallback"),
            "SelectBatch callback_failed for req_id=4, sub_id=9: failed on "
            "req_id=400");

  // 6. Message contains req_id but lacks sub_id
  NodeFailure f6(
      NodeErrorKind::kBusinessError, "failed on req_id=42",
      BatchFailureDetail{"SelectBatch", BatchFailureReason::kCallbackFailed,
                         TraceableItemKey{42, 9}},
      -7788);
  EXPECT_EQ(f6.FormatDiagnostic("fallback"),
            "SelectBatch callback_failed for req_id=42, sub_id=9: failed on "
            "req_id=42");

  // 7. Message already contains full structured detail - avoids redundant
  // double formatting
  NodeFailure f7(
      NodeErrorKind::kBusinessError,
      "SelectBatch callback_failed for req_id=42, sub_id=9: predicate failed",
      BatchFailureDetail{"SelectBatch", BatchFailureReason::kCallbackFailed,
                         TraceableItemKey{42, 9}},
      -7788);
  EXPECT_EQ(
      f7.FormatDiagnostic("fallback"),
      "SelectBatch callback_failed for req_id=42, sub_id=9: predicate failed");
}

TEST_F(TraceableBatchOperationsTest, BatchDiagnosticRequiresCompleteKeyTokens) {
  for (const std::string message :
       {"SelectBatch predicate failed for req_id=42",
        "SelectBatch predicate failed for req_id=420, sub_id=9",
        "SelectBatch predicate failed for req_id=42, sub_id=90",
        "SelectBatch predicate failed for xreq_id=42, sub_id=9",
        "SelectBatch predicate failed for req_id=42, xsub_id=9",
        "SelectBatch predicate failed for req_id=42x, sub_id=9",
        "SelectBatch predicate failed for req_id=42, sub_id=9x"}) {
    SCOPED_TRACE(message);
    NodeFailure failure(
        NodeErrorKind::kBusinessError, message,
        BatchFailureDetail{"SelectBatch", BatchFailureReason::kCallbackFailed,
                           TraceableItemKey{42, 9}},
        -7788);
    EXPECT_EQ(
        failure.FormatDiagnostic("fallback"),
        "SelectBatch callback_failed for req_id=42, sub_id=9: " + message);
    EXPECT_EQ(failure.message, message);
    EXPECT_EQ(failure.cause_code, -7788);
  }
}

struct BatchSelectFailInputs {
  const TextBatch* input = nullptr;
};
struct BatchSelectFailOptions {};

NodeResult<TextBatch> RunBatchSelectFail(const BatchSelectFailInputs& in,
                                         const BatchSelectFailOptions&) {
  if (!in.input || in.input->empty()) {
    return NodeResult<TextBatch>::Success(TextBatch{});
  }
  // Item (42, 9) triggers failure with cause_code = -7788 and message =
  // "predicate failed"
  auto sel =
      SelectBatch(*in.input, [](const std::string& s) -> NodeResult<bool> {
        if (s == "trigger_partial_key_failure") {
          return NodeResult<bool>::Failure(
              NodeErrorKind::kBusinessError,
              "SelectBatch predicate failed for req_id=42", -7788);
        }
        if (s == "trigger_failure") {
          return NodeResult<bool>::Failure(NodeErrorKind::kBusinessError,
                                           "predicate failed", -7788);
        }
        return NodeResult<bool>::Success(true);
      });
  if (!sel.ok()) {
    return NodeResult<TextBatch>::Failure(std::move(sel).ExtractFailure());
  }
  return NodeResult<TextBatch>::Success(*in.input);
}

auto BatchSelectFailSpec() {
  return MakeBatchSpec(InputsOf<BatchSelectFailInputs>({
                           Required("input", &BatchSelectFailInputs::input),
                       }),
                       PreservedOutput<TextBatch>("output", "input"),
                       Parameters<BatchSelectFailOptions>({}),
                       &RunBatchSelectFail)
      .Description(
          "Test fixture for SelectBatch failure diagnostic formatting");
}

REGISTER_FUNCTION_NODE(BatchSelectFailTestNode, BatchSelectFailSpec());

TEST_F(TraceableBatchOperationsTest,
       AuthorNodeFormatsSelectBatchFailureDiagnostic) {
  NodeHarness harness("BatchSelectFailTestNode");
  TextBatch batch = {{1, 0, "ok"}, {42, 9, "trigger_failure"}};
  harness.TextInputWithBatch("input", std::move(batch));

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.process_code(), -7788);
  const auto& diag = result.diagnostic();
  EXPECT_NE(diag.find("Process returned -7788"), std::string::npos);
  EXPECT_NE(diag.find("SelectBatch"), std::string::npos);
  EXPECT_NE(diag.find("predicate failed"), std::string::npos);
  EXPECT_NE(diag.find("req_id=42"), std::string::npos);
  EXPECT_NE(diag.find("sub_id=9"), std::string::npos);
}

TEST_F(TraceableBatchOperationsTest,
       AuthorNodeCompletesSelectBatchPartialKeyDiagnostic) {
  NodeHarness harness("BatchSelectFailTestNode");
  harness.TextInputWithBatch("input", {{42, 9, "trigger_partial_key_failure"}});

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.process_code(), -7788);
  EXPECT_NE(result.diagnostic().find("Process returned -7788"),
            std::string::npos);
  EXPECT_NE(result.diagnostic().find(
                "SelectBatch callback_failed for req_id=42, sub_id=9: "
                "SelectBatch predicate failed for req_id=42"),
            std::string::npos);
}

struct BatchSplitFailInputs {
  const TextBatch* input = nullptr;
};
struct BatchSplitFailOptions {};

NodeResult<TextBatch> RunBatchSplitFail(const BatchSplitFailInputs& in,
                                        const BatchSplitFailOptions&) {
  if (!in.input || in.input->empty()) {
    return NodeResult<TextBatch>::Success(TextBatch{});
  }
  auto split = SplitPayloads(
      *in.input,
      [](const std::string& s) -> NodeResult<std::vector<std::string>> {
        if (s == "trigger_split_failure") {
          return NodeResult<std::vector<std::string>>::Failure(
              NodeErrorKind::kBusinessError, "split callback failed", -6677);
        }
        return NodeResult<std::vector<std::string>>::Success({s});
      });
  if (!split.ok()) {
    return NodeResult<TextBatch>::Failure(std::move(split).ExtractFailure());
  }
  return NodeResult<TextBatch>::Success(*in.input);
}

auto BatchSplitFailSpec() {
  return MakeBatchSpec(InputsOf<BatchSplitFailInputs>({
                           Required("input", &BatchSplitFailInputs::input),
                       }),
                       PreservedOutput<TextBatch>("output", "input"),
                       Parameters<BatchSplitFailOptions>({}),
                       &RunBatchSplitFail)
      .Description(
          "Test fixture for SplitPayloads failure diagnostic formatting");
}

REGISTER_FUNCTION_NODE(BatchSplitFailTestNode, BatchSplitFailSpec());

TEST_F(TraceableBatchOperationsTest,
       AuthorNodeFormatsSplitPayloadsFailureDiagnostic) {
  NodeHarness harness("BatchSplitFailTestNode");
  TextBatch batch = {{10, 3, "trigger_split_failure"}};
  harness.TextInputWithBatch("input", std::move(batch));

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.process_code(), -6677);
  const auto& diag = result.diagnostic();
  EXPECT_NE(diag.find("Process returned -6677"), std::string::npos);
  EXPECT_NE(diag.find("SplitPayloads"), std::string::npos);
  EXPECT_NE(diag.find("split callback failed"), std::string::npos);
  EXPECT_NE(diag.find("req_id=10"), std::string::npos);
  EXPECT_NE(diag.find("sub_id=3"), std::string::npos);
}

inline NodeResult<std::string> RunMapItemFail(const std::string& s) {
  if (s == "trigger_map_failure") {
    return NodeResult<std::string>::Failure(NodeErrorKind::kBusinessError,
                                            "map item failed", -5544);
  }
  return NodeResult<std::string>::Success(s);
}

inline auto MapItemFailSpec() {
  return MakeMapSpec(Input<TextBatch>("input"), Output<TextBatch>("output"),
                     &RunMapItemFail)
      .Description("Test fixture for MapSpec failure diagnostic formatting");
}

REGISTER_FUNCTION_NODE(MapItemFailTestNode, MapItemFailSpec());

TEST_F(TraceableBatchOperationsTest,
       AuthorNodeFormatsMapSpecFailureDiagnostic) {
  NodeHarness harness("MapItemFailTestNode");
  TextBatch batch = {{1, 0, "ok"}, {7, 3, "trigger_map_failure"}};
  harness.TextInputWithBatch("input", std::move(batch));

  auto result = harness.Run();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.process_code(), -5544);
  const auto& diag = result.diagnostic();
  EXPECT_NE(diag.find("Process returned -5544"), std::string::npos);
  EXPECT_NE(diag.find("MapItemFailTestNode"), std::string::npos);
  EXPECT_NE(diag.find("map item failed"), std::string::npos);
  EXPECT_NE(diag.find("req_id=7"), std::string::npos);
  EXPECT_NE(diag.find("sub_id=3"), std::string::npos);
}

}  // namespace
}  // namespace llm_edgeflow
