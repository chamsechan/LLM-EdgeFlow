#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "contracts/traceable_item.h"
#include "core/common_contracts.h"
#include "nodes/node_result.h"
#include "nodes/traceable_algorithms.h"

namespace llm_edgeflow {

// TraceableItemKeyHash and std::hash<TraceableItemKey> are defined in
// node_result.h for repository-wide availability.

// ============================================================================
// 1. JoinByItem
// ============================================================================

enum class JoinMode {
  kExact,
  kLeft,
};

template <typename LeftPayload, typename RightPayload>
struct JoinedRow {
  const TraceableItem<LeftPayload>& left;
  const TraceableItem<RightPayload>* right = nullptr;

  JoinedRow(const TraceableItem<LeftPayload>& l,
            const TraceableItem<RightPayload>* r = nullptr)
      : left(l), right(r) {}
  JoinedRow(TraceableItem<LeftPayload>&&,
            const TraceableItem<RightPayload>* = nullptr) = delete;
  JoinedRow(const TraceableItem<LeftPayload>&&,
            const TraceableItem<RightPayload>* = nullptr) = delete;

  JoinedRow(const JoinedRow&) = default;
  JoinedRow(JoinedRow&&) = default;

  uint32_t req_id() const noexcept { return left.req_id; }
  uint32_t sub_id() const noexcept { return left.sub_id; }
  const LeftPayload& left_payload() const noexcept { return left.data; }
  const RightPayload* right_payload() const noexcept {
    return right ? &right->data : nullptr;
  }
  bool has_right() const noexcept { return right != nullptr; }
};

template <typename LeftPayload, typename RightPayload>
class ItemJoinView {
 public:
  using RowType = JoinedRow<LeftPayload, RightPayload>;

  ItemJoinView(const std::vector<TraceableItem<LeftPayload>>& left,
               const std::vector<TraceableItem<RightPayload>>& right,
               std::vector<RowType> rows)
      : left_(&left), right_(&right), rows_(std::move(rows)) {}

  // Disallow construction from temporary / rvalue batches (both non-const and
  // const)
  ItemJoinView(std::vector<TraceableItem<LeftPayload>>&&,
               const std::vector<TraceableItem<RightPayload>>&,
               std::vector<RowType>) = delete;
  ItemJoinView(const std::vector<TraceableItem<LeftPayload>>&&,
               const std::vector<TraceableItem<RightPayload>>&,
               std::vector<RowType>) = delete;
  ItemJoinView(const std::vector<TraceableItem<LeftPayload>>&,
               std::vector<TraceableItem<RightPayload>>&&,
               std::vector<RowType>) = delete;
  ItemJoinView(const std::vector<TraceableItem<LeftPayload>>&,
               const std::vector<TraceableItem<RightPayload>>&&,
               std::vector<RowType>) = delete;
  ItemJoinView(std::vector<TraceableItem<LeftPayload>>&&,
               std::vector<TraceableItem<RightPayload>>&&,
               std::vector<RowType>) = delete;
  ItemJoinView(std::vector<TraceableItem<LeftPayload>>&&,
               const std::vector<TraceableItem<RightPayload>>&&,
               std::vector<RowType>) = delete;
  ItemJoinView(const std::vector<TraceableItem<LeftPayload>>&&,
               std::vector<TraceableItem<RightPayload>>&&,
               std::vector<RowType>) = delete;
  ItemJoinView(const std::vector<TraceableItem<LeftPayload>>&&,
               const std::vector<TraceableItem<RightPayload>>&&,
               std::vector<RowType>) = delete;

  size_t size() const noexcept { return rows_.size(); }
  bool empty() const noexcept { return rows_.empty(); }

  const RowType& operator[](size_t index) const { return rows_[index]; }
  const RowType& at(size_t index) const { return rows_.at(index); }

  auto begin() const noexcept { return rows_.begin(); }
  auto end() const noexcept { return rows_.end(); }

  const std::vector<RowType>& rows() const noexcept { return rows_; }
  const std::vector<TraceableItem<LeftPayload>>& left_batch() const noexcept {
    return *left_;
  }
  const std::vector<TraceableItem<RightPayload>>& right_batch() const noexcept {
    return *right_;
  }

 private:
  const std::vector<TraceableItem<LeftPayload>>* left_ = nullptr;
  const std::vector<TraceableItem<RightPayload>>* right_ = nullptr;
  std::vector<RowType> rows_;
};

template <typename LeftPayload, typename RightPayload>
NodeResult<ItemJoinView<LeftPayload, RightPayload>> JoinByItem(
    const std::vector<TraceableItem<LeftPayload>>& left,
    const std::vector<TraceableItem<RightPayload>>& right,
    JoinMode mode = JoinMode::kExact) {
  std::unordered_map<TraceableItemKey, size_t> left_key_to_idx;
  left_key_to_idx.reserve(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    TraceableItemKey key{left[i].req_id, left[i].sub_id};
    if (!left_key_to_idx.emplace(key, i).second) {
      return NodeResult<ItemJoinView<LeftPayload, RightPayload>>::Failure(
          NodeErrorKind::kInputError,
          "JoinByItem left batch contains duplicate key: req_id=" +
              std::to_string(key.req_id) +
              ", sub_id=" + std::to_string(key.sub_id),
          BatchFailureDetail{"JoinByItem", BatchFailureReason::kDuplicate,
                             key});
    }
  }

  std::unordered_map<TraceableItemKey, const TraceableItem<RightPayload>*>
      right_key_to_ptr;
  right_key_to_ptr.reserve(right.size());
  for (size_t j = 0; j < right.size(); ++j) {
    TraceableItemKey key{right[j].req_id, right[j].sub_id};
    if (!right_key_to_ptr.emplace(key, &right[j]).second) {
      return NodeResult<ItemJoinView<LeftPayload, RightPayload>>::Failure(
          NodeErrorKind::kInputError,
          "JoinByItem right batch contains duplicate key: req_id=" +
              std::to_string(key.req_id) +
              ", sub_id=" + std::to_string(key.sub_id),
          BatchFailureDetail{"JoinByItem", BatchFailureReason::kDuplicate,
                             key});
    }
  }

  // Check for unknown keys in right (in both kExact and kLeft)
  for (const auto& r_item : right) {
    TraceableItemKey r_key{r_item.req_id, r_item.sub_id};
    if (left_key_to_idx.find(r_key) == left_key_to_idx.end()) {
      return NodeResult<ItemJoinView<LeftPayload, RightPayload>>::Failure(
          NodeErrorKind::kInputError,
          "JoinByItem right batch contains unknown key not in left: req_id=" +
              std::to_string(r_key.req_id) +
              ", sub_id=" + std::to_string(r_key.sub_id),
          BatchFailureDetail{"JoinByItem", BatchFailureReason::kUnknown,
                             r_key});
    }
  }

  std::vector<JoinedRow<LeftPayload, RightPayload>> rows;
  rows.reserve(left.size());
  for (const auto& l_item : left) {
    TraceableItemKey key{l_item.req_id, l_item.sub_id};
    auto it = right_key_to_ptr.find(key);
    if (it == right_key_to_ptr.end()) {
      if (mode == JoinMode::kExact) {
        return NodeResult<ItemJoinView<LeftPayload, RightPayload>>::Failure(
            NodeErrorKind::kInputError,
            "JoinByItem right batch missing key present in left: req_id=" +
                std::to_string(key.req_id) +
                ", sub_id=" + std::to_string(key.sub_id),
            BatchFailureDetail{"JoinByItem", BatchFailureReason::kMissing,
                               key});
      }
      rows.push_back(JoinedRow<LeftPayload, RightPayload>{l_item, nullptr});
    } else {
      rows.push_back(JoinedRow<LeftPayload, RightPayload>{l_item, it->second});
    }
  }

  return NodeResult<ItemJoinView<LeftPayload, RightPayload>>::Success(
      ItemJoinView<LeftPayload, RightPayload>(left, right, std::move(rows)));
}

template <typename LeftPayload, typename RightPayload>
void JoinByItem(std::vector<TraceableItem<LeftPayload>>&&,
                const std::vector<TraceableItem<RightPayload>>&,
                JoinMode = JoinMode::kExact) = delete;

template <typename LeftPayload, typename RightPayload>
void JoinByItem(const std::vector<TraceableItem<LeftPayload>>&&,
                const std::vector<TraceableItem<RightPayload>>&,
                JoinMode = JoinMode::kExact) = delete;

template <typename LeftPayload, typename RightPayload>
void JoinByItem(const std::vector<TraceableItem<LeftPayload>>&,
                std::vector<TraceableItem<RightPayload>>&&,
                JoinMode = JoinMode::kExact) = delete;

template <typename LeftPayload, typename RightPayload>
void JoinByItem(const std::vector<TraceableItem<LeftPayload>>&,
                const std::vector<TraceableItem<RightPayload>>&&,
                JoinMode = JoinMode::kExact) = delete;

template <typename LeftPayload, typename RightPayload>
void JoinByItem(std::vector<TraceableItem<LeftPayload>>&&,
                std::vector<TraceableItem<RightPayload>>&&,
                JoinMode = JoinMode::kExact) = delete;

template <typename LeftPayload, typename RightPayload>
void JoinByItem(std::vector<TraceableItem<LeftPayload>>&&,
                const std::vector<TraceableItem<RightPayload>>&&,
                JoinMode = JoinMode::kExact) = delete;

template <typename LeftPayload, typename RightPayload>
void JoinByItem(const std::vector<TraceableItem<LeftPayload>>&&,
                std::vector<TraceableItem<RightPayload>>&&,
                JoinMode = JoinMode::kExact) = delete;

template <typename LeftPayload, typename RightPayload>
void JoinByItem(const std::vector<TraceableItem<LeftPayload>>&&,
                const std::vector<TraceableItem<RightPayload>>&&,
                JoinMode = JoinMode::kExact) = delete;

// ============================================================================
// 2. GroupByRequest
// ============================================================================

template <typename AnchorPayload, typename MemberPayload>
class RequestGroup {
 public:
  using AnchorItem = TraceableItem<AnchorPayload>;
  using MemberItem = TraceableItem<MemberPayload>;

  explicit RequestGroup(uint32_t req_id) : req_id_(req_id) {}

  uint32_t req_id() const noexcept { return req_id_; }

  const std::vector<std::reference_wrapper<const AnchorItem>>& anchors()
      const noexcept {
    return anchors_;
  }
  const std::vector<std::reference_wrapper<const MemberItem>>& members()
      const noexcept {
    return members_;
  }

  size_t anchor_count() const noexcept { return anchors_.size(); }
  size_t member_count() const noexcept { return members_.size(); }
  bool has_members() const noexcept { return !members_.empty(); }

  void AddAnchor(const AnchorItem& item) {
    anchors_.push_back(std::cref(item));
  }
  void AddAnchor(AnchorItem&&) = delete;
  void AddAnchor(const AnchorItem&&) = delete;

  void AddMember(const MemberItem& item) {
    members_.push_back(std::cref(item));
  }
  void AddMember(MemberItem&&) = delete;
  void AddMember(const MemberItem&&) = delete;

 private:
  uint32_t req_id_ = 0;
  std::vector<std::reference_wrapper<const AnchorItem>> anchors_;
  std::vector<std::reference_wrapper<const MemberItem>> members_;
};

template <typename AnchorPayload, typename MemberPayload>
class RequestGroupView {
 public:
  using GroupType = RequestGroup<AnchorPayload, MemberPayload>;

  RequestGroupView(const std::vector<TraceableItem<AnchorPayload>>& anchor,
                   const std::vector<TraceableItem<MemberPayload>>& members,
                   std::vector<GroupType> groups,
                   std::vector<size_t> anchor_to_group_index,
                   std::unordered_map<uint32_t, size_t> req_id_to_group_index)
      : anchor_(&anchor),
        members_(&members),
        groups_(std::move(groups)),
        anchor_to_group_index_(std::move(anchor_to_group_index)),
        req_id_to_group_index_(std::move(req_id_to_group_index)) {}

  RequestGroupView(std::vector<TraceableItem<AnchorPayload>>&&,
                   const std::vector<TraceableItem<MemberPayload>>&,
                   std::vector<GroupType>, std::vector<size_t>,
                   std::unordered_map<uint32_t, size_t>) = delete;
  RequestGroupView(const std::vector<TraceableItem<AnchorPayload>>&&,
                   const std::vector<TraceableItem<MemberPayload>>&,
                   std::vector<GroupType>, std::vector<size_t>,
                   std::unordered_map<uint32_t, size_t>) = delete;
  RequestGroupView(const std::vector<TraceableItem<AnchorPayload>>&,
                   std::vector<TraceableItem<MemberPayload>>&&,
                   std::vector<GroupType>, std::vector<size_t>,
                   std::unordered_map<uint32_t, size_t>) = delete;
  RequestGroupView(const std::vector<TraceableItem<AnchorPayload>>&,
                   const std::vector<TraceableItem<MemberPayload>>&&,
                   std::vector<GroupType>, std::vector<size_t>,
                   std::unordered_map<uint32_t, size_t>) = delete;
  RequestGroupView(std::vector<TraceableItem<AnchorPayload>>&&,
                   std::vector<TraceableItem<MemberPayload>>&&,
                   std::vector<GroupType>, std::vector<size_t>,
                   std::unordered_map<uint32_t, size_t>) = delete;
  RequestGroupView(std::vector<TraceableItem<AnchorPayload>>&&,
                   const std::vector<TraceableItem<MemberPayload>>&&,
                   std::vector<GroupType>, std::vector<size_t>,
                   std::unordered_map<uint32_t, size_t>) = delete;
  RequestGroupView(const std::vector<TraceableItem<AnchorPayload>>&&,
                   std::vector<TraceableItem<MemberPayload>>&&,
                   std::vector<GroupType>, std::vector<size_t>,
                   std::unordered_map<uint32_t, size_t>) = delete;
  RequestGroupView(const std::vector<TraceableItem<AnchorPayload>>&&,
                   const std::vector<TraceableItem<MemberPayload>>&&,
                   std::vector<GroupType>, std::vector<size_t>,
                   std::unordered_map<uint32_t, size_t>) = delete;

  size_t size() const noexcept { return groups_.size(); }
  bool empty() const noexcept { return groups_.empty(); }

  const GroupType& operator[](size_t index) const { return groups_[index]; }
  const GroupType& at(size_t index) const { return groups_.at(index); }

  auto begin() const noexcept { return groups_.begin(); }
  auto end() const noexcept { return groups_.end(); }

  const std::vector<GroupType>& groups() const noexcept { return groups_; }

  const GroupType* FindByReqId(uint32_t req_id) const noexcept {
    auto it = req_id_to_group_index_.find(req_id);
    if (it == req_id_to_group_index_.end()) return nullptr;
    return &groups_[it->second];
  }

  const GroupType* Find(uint32_t req_id) const noexcept {
    return FindByReqId(req_id);
  }

  bool HasReqId(uint32_t req_id) const noexcept {
    return req_id_to_group_index_.find(req_id) != req_id_to_group_index_.end();
  }

  bool Contains(uint32_t req_id) const noexcept { return HasReqId(req_id); }

  const GroupType& GroupByAnchorIndex(size_t anchor_index) const {
    return groups_.at(anchor_to_group_index_.at(anchor_index));
  }

  size_t GroupIndexForAnchor(size_t anchor_index) const {
    return anchor_to_group_index_.at(anchor_index);
  }

  const std::vector<TraceableItem<AnchorPayload>>& anchor_batch()
      const noexcept {
    return *anchor_;
  }
  const std::vector<TraceableItem<MemberPayload>>& members_batch()
      const noexcept {
    return *members_;
  }

 private:
  const std::vector<TraceableItem<AnchorPayload>>* anchor_ = nullptr;
  const std::vector<TraceableItem<MemberPayload>>* members_ = nullptr;
  std::vector<GroupType> groups_;
  std::vector<size_t> anchor_to_group_index_;
  std::unordered_map<uint32_t, size_t> req_id_to_group_index_;
};

template <typename AnchorPayload, typename MemberPayload>
NodeResult<RequestGroupView<AnchorPayload, MemberPayload>> GroupByRequest(
    const std::vector<TraceableItem<AnchorPayload>>& anchor,
    const std::vector<TraceableItem<MemberPayload>>& members) {
  std::unordered_set<TraceableItemKey> anchor_keys;
  std::vector<RequestGroup<AnchorPayload, MemberPayload>> groups;
  std::unordered_map<uint32_t, size_t> req_id_to_group_index;
  std::vector<size_t> anchor_to_group_index;
  anchor_to_group_index.reserve(anchor.size());

  for (size_t i = 0; i < anchor.size(); ++i) {
    const auto& item = anchor[i];
    TraceableItemKey key{item.req_id, item.sub_id};
    if (!anchor_keys.insert(key).second) {
      return NodeResult<RequestGroupView<AnchorPayload, MemberPayload>>::
          Failure(NodeErrorKind::kInputError,
                  "GroupByRequest anchor contains duplicate key: req_id=" +
                      std::to_string(key.req_id) +
                      ", sub_id=" + std::to_string(key.sub_id),
                  BatchFailureDetail{"GroupByRequest",
                                     BatchFailureReason::kDuplicate, key});
    }

    auto it = req_id_to_group_index.find(item.req_id);
    size_t group_idx = 0;
    if (it == req_id_to_group_index.end()) {
      group_idx = groups.size();
      groups.emplace_back(item.req_id);
      req_id_to_group_index.emplace(item.req_id, group_idx);
    } else {
      group_idx = it->second;
    }
    groups[group_idx].AddAnchor(item);
    anchor_to_group_index.push_back(group_idx);
  }

  std::unordered_set<TraceableItemKey> member_keys;
  for (const auto& item : members) {
    TraceableItemKey key{item.req_id, item.sub_id};
    if (!member_keys.insert(key).second) {
      return NodeResult<RequestGroupView<AnchorPayload, MemberPayload>>::
          Failure(NodeErrorKind::kInputError,
                  "GroupByRequest members contains duplicate key: req_id=" +
                      std::to_string(key.req_id) +
                      ", sub_id=" + std::to_string(key.sub_id),
                  BatchFailureDetail{"GroupByRequest",
                                     BatchFailureReason::kDuplicate, key});
    }

    auto it = req_id_to_group_index.find(item.req_id);
    if (it == req_id_to_group_index.end()) {
      return NodeResult<RequestGroupView<AnchorPayload, MemberPayload>>::
          Failure(NodeErrorKind::kInputError,
                  "GroupByRequest member contains unknown req_id=" +
                      std::to_string(item.req_id) + " not present in anchor",
                  BatchFailureDetail{"GroupByRequest",
                                     BatchFailureReason::kUnknown, key});
    }
    groups[it->second].AddMember(item);
  }

  return NodeResult<RequestGroupView<AnchorPayload, MemberPayload>>::Success(
      RequestGroupView<AnchorPayload, MemberPayload>(
          anchor, members, std::move(groups), std::move(anchor_to_group_index),
          std::move(req_id_to_group_index)));
}

template <typename AnchorPayload, typename MemberPayload>
void GroupByRequest(std::vector<TraceableItem<AnchorPayload>>&&,
                    const std::vector<TraceableItem<MemberPayload>>&) = delete;

template <typename AnchorPayload, typename MemberPayload>
void GroupByRequest(const std::vector<TraceableItem<AnchorPayload>>&&,
                    const std::vector<TraceableItem<MemberPayload>>&) = delete;

template <typename AnchorPayload, typename MemberPayload>
void GroupByRequest(const std::vector<TraceableItem<AnchorPayload>>&,
                    std::vector<TraceableItem<MemberPayload>>&&) = delete;

template <typename AnchorPayload, typename MemberPayload>
void GroupByRequest(const std::vector<TraceableItem<AnchorPayload>>&,
                    const std::vector<TraceableItem<MemberPayload>>&&) = delete;

template <typename AnchorPayload, typename MemberPayload>
void GroupByRequest(std::vector<TraceableItem<AnchorPayload>>&&,
                    std::vector<TraceableItem<MemberPayload>>&&) = delete;

template <typename AnchorPayload, typename MemberPayload>
void GroupByRequest(std::vector<TraceableItem<AnchorPayload>>&&,
                    const std::vector<TraceableItem<MemberPayload>>&&) = delete;

template <typename AnchorPayload, typename MemberPayload>
void GroupByRequest(const std::vector<TraceableItem<AnchorPayload>>&&,
                    std::vector<TraceableItem<MemberPayload>>&&) = delete;

template <typename AnchorPayload, typename MemberPayload>
void GroupByRequest(const std::vector<TraceableItem<AnchorPayload>>&&,
                    const std::vector<TraceableItem<MemberPayload>>&&) = delete;

// ============================================================================
// 3. SelectBatch and ScatterReplace
// ============================================================================

template <typename Payload>
class Selection {
 public:
  using ItemType = TraceableItem<Payload>;
  using BatchType = std::vector<ItemType>;

  Selection(std::vector<ItemType>&&, std::vector<size_t>) = delete;
  Selection(const std::vector<ItemType>&&, std::vector<size_t>) = delete;

  size_t size() const noexcept { return selected_indices_.size(); }
  bool empty() const noexcept { return selected_indices_.empty(); }
  size_t total_anchor_size() const noexcept {
    return anchor_ ? anchor_->size() : 0;
  }

  const ItemType& operator[](size_t index) const {
    return (*anchor_)[selected_indices_.at(index)];
  }
  const ItemType& at(size_t index) const {
    return (*anchor_)[selected_indices_.at(index)];
  }

  class const_iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = ItemType;
    using difference_type = std::ptrdiff_t;
    using pointer = const ItemType*;
    using reference = const ItemType&;

    const_iterator(const BatchType* anchor,
                   std::vector<size_t>::const_iterator it)
        : anchor_(anchor), it_(it) {}

    reference operator*() const { return (*anchor_)[*it_]; }
    pointer operator->() const { return &((*anchor_)[*it_]); }

    const_iterator& operator++() {
      ++it_;
      return *this;
    }
    const_iterator operator++(int) {
      const_iterator tmp = *this;
      ++it_;
      return tmp;
    }

    bool operator==(const const_iterator& other) const noexcept {
      return it_ == other.it_;
    }
    bool operator!=(const const_iterator& other) const noexcept {
      return !(*this == other);
    }

   private:
    const BatchType* anchor_ = nullptr;
    std::vector<size_t>::const_iterator it_;
  };

  const_iterator begin() const noexcept {
    return const_iterator(anchor_, selected_indices_.cbegin());
  }
  const_iterator end() const noexcept {
    return const_iterator(anchor_, selected_indices_.cend());
  }

  BatchType Materialize() const {
    BatchType result;
    result.reserve(selected_indices_.size());
    for (size_t idx : selected_indices_) {
      result.push_back((*anchor_)[idx]);
    }
    return result;
  }

  const BatchType& original_batch() const noexcept { return *anchor_; }
  const std::vector<size_t>& selected_indices() const noexcept {
    return selected_indices_;
  }

 private:
  template <typename P, typename Pred>
  friend NodeResult<Selection<P>> SelectBatch(
      const std::vector<TraceableItem<P>>& anchor, Pred&& predicate);

  Selection(const BatchType& anchor, std::vector<size_t> indices)
      : anchor_(&anchor), selected_indices_(std::move(indices)) {}

  const BatchType* anchor_ = nullptr;
  std::vector<size_t> selected_indices_;
};

template <typename Payload, typename Predicate>
NodeResult<Selection<Payload>> SelectBatch(
    const std::vector<TraceableItem<Payload>>& anchor, Predicate&& predicate) {
  std::unordered_set<TraceableItemKey> seen_keys;
  for (const auto& item : anchor) {
    TraceableItemKey key{item.req_id, item.sub_id};
    if (!seen_keys.insert(key).second) {
      return NodeResult<Selection<Payload>>::Failure(
          NodeErrorKind::kInputError,
          "SelectBatch anchor contains duplicate key: req_id=" +
              std::to_string(key.req_id) +
              ", sub_id=" + std::to_string(key.sub_id),
          BatchFailureDetail{"SelectBatch", BatchFailureReason::kDuplicate,
                             key});
    }
  }

  std::vector<size_t> selected_indices;
  for (size_t i = 0; i < anchor.size(); ++i) {
    const auto& item = anchor[i];
    try {
      if constexpr (std::is_invocable_v<Predicate, const Payload&>) {
        using Ret = std::invoke_result_t<Predicate, const Payload&>;
        if constexpr (std::is_same_v<Ret, bool>) {
          if (predicate(item.data)) {
            selected_indices.push_back(i);
          }
        } else if constexpr (std::is_same_v<Ret, NodeResult<bool>>) {
          auto res = predicate(item.data);
          if (!res.ok()) {
            auto failure = std::move(res).ExtractFailure();
            failure.batch_detail = BatchFailureDetail{
                "SelectBatch", BatchFailureReason::kCallbackFailed,
                TraceableItemKey{item.req_id, item.sub_id}};
            return NodeResult<Selection<Payload>>::Failure(std::move(failure));
          }
          if (res.value()) {
            selected_indices.push_back(i);
          }
        } else {
          static_assert(std::is_same_v<Ret, bool> ||
                            std::is_same_v<Ret, NodeResult<bool>>,
                        "SelectBatch predicate must return bool or "
                        "NodeResult<bool>");
        }
      } else if constexpr (std::is_invocable_v<Predicate,
                                               const TraceableItem<Payload>&>) {
        using Ret =
            std::invoke_result_t<Predicate, const TraceableItem<Payload>&>;
        if constexpr (std::is_same_v<Ret, bool>) {
          if (predicate(item)) {
            selected_indices.push_back(i);
          }
        } else if constexpr (std::is_same_v<Ret, NodeResult<bool>>) {
          auto res = predicate(item);
          if (!res.ok()) {
            auto failure = std::move(res).ExtractFailure();
            failure.batch_detail = BatchFailureDetail{
                "SelectBatch", BatchFailureReason::kCallbackFailed,
                TraceableItemKey{item.req_id, item.sub_id}};
            return NodeResult<Selection<Payload>>::Failure(std::move(failure));
          }
          if (res.value()) {
            selected_indices.push_back(i);
          }
        } else {
          static_assert(std::is_same_v<Ret, bool> ||
                            std::is_same_v<Ret, NodeResult<bool>>,
                        "SelectBatch predicate must return bool or "
                        "NodeResult<bool>");
        }
      } else {
        static_assert(
            std::is_invocable_v<Predicate, const Payload&> ||
                std::is_invocable_v<Predicate, const TraceableItem<Payload>&>,
            "SelectBatch predicate must accept (const Payload&) or (const "
            "TraceableItem<Payload>&)");
      }
    } catch (const std::exception& e) {
      return NodeResult<Selection<Payload>>::Failure(
          NodeErrorKind::kBusinessError,
          std::string("SelectBatch predicate threw exception: ") + e.what(),
          BatchFailureDetail{"SelectBatch", BatchFailureReason::kCallbackFailed,
                             TraceableItemKey{item.req_id, item.sub_id}});
    } catch (...) {
      return NodeResult<Selection<Payload>>::Failure(
          NodeErrorKind::kBusinessError,
          "SelectBatch predicate threw unknown exception",
          BatchFailureDetail{"SelectBatch", BatchFailureReason::kCallbackFailed,
                             TraceableItemKey{item.req_id, item.sub_id}});
    }
  }

  return NodeResult<Selection<Payload>>::Success(
      Selection<Payload>(anchor, std::move(selected_indices)));
}

template <typename Payload, typename Predicate>
void SelectBatch(std::vector<TraceableItem<Payload>>&&, Predicate&&) = delete;

template <typename Payload, typename Predicate>
void SelectBatch(const std::vector<TraceableItem<Payload>>&&,
                 Predicate&&) = delete;

template <typename Payload>
NodeResult<std::vector<TraceableItem<Payload>>> ScatterReplace(
    const Selection<Payload>& selection,
    const std::vector<TraceableItem<Payload>>& replacements) {
  std::unordered_map<TraceableItemKey, const TraceableItem<Payload>*> repl_map;
  repl_map.reserve(replacements.size());
  for (const auto& item : replacements) {
    TraceableItemKey key{item.req_id, item.sub_id};
    if (!repl_map.emplace(key, &item).second) {
      return NodeResult<std::vector<TraceableItem<Payload>>>::Failure(
          NodeErrorKind::kInputError,
          "ScatterReplace replacements contains duplicate key: req_id=" +
              std::to_string(key.req_id) +
              ", sub_id=" + std::to_string(key.sub_id),
          BatchFailureDetail{"ScatterReplace", BatchFailureReason::kDuplicate,
                             key});
    }
  }

  std::unordered_set<TraceableItemKey> selected_keys;
  const auto& anchor = selection.original_batch();
  for (size_t idx : selection.selected_indices()) {
    selected_keys.insert(
        TraceableItemKey{anchor[idx].req_id, anchor[idx].sub_id});
  }

  for (const auto& item : replacements) {
    TraceableItemKey key{item.req_id, item.sub_id};
    if (selected_keys.find(key) == selected_keys.end()) {
      return NodeResult<std::vector<TraceableItem<Payload>>>::Failure(
          NodeErrorKind::kInputError,
          "ScatterReplace replacement contains unselected or unknown key: "
          "req_id=" +
              std::to_string(key.req_id) +
              ", sub_id=" + std::to_string(key.sub_id),
          BatchFailureDetail{"ScatterReplace", BatchFailureReason::kUnknown,
                             key});
    }
  }

  for (size_t idx : selection.selected_indices()) {
    TraceableItemKey sel_key{anchor[idx].req_id, anchor[idx].sub_id};
    if (repl_map.find(sel_key) == repl_map.end()) {
      return NodeResult<std::vector<TraceableItem<Payload>>>::Failure(
          NodeErrorKind::kInputError,
          "ScatterReplace replacement missing selected key: req_id=" +
              std::to_string(sel_key.req_id) +
              ", sub_id=" + std::to_string(sel_key.sub_id),
          BatchFailureDetail{"ScatterReplace", BatchFailureReason::kMissing,
                             sel_key});
    }
  }

  std::vector<TraceableItem<Payload>> full_batch = anchor;
  for (size_t idx : selection.selected_indices()) {
    TraceableItemKey key{anchor[idx].req_id, anchor[idx].sub_id};
    auto it = repl_map.find(key);
    full_batch[idx].data = it->second->data;
  }

  return NodeResult<std::vector<TraceableItem<Payload>>>::Success(
      std::move(full_batch));
}

// ============================================================================
// 4. SplitPayloads
// ============================================================================

template <typename OutputPayload>
struct SplitResult {
  std::vector<TraceableItem<OutputPayload>> children;
  Int32Batch counts;
};

namespace detail {

inline NodeResult<int32_t> CheckedSplitCount(size_t count,
                                             TraceableItemKey parent) {
  if (count > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    return NodeResult<int32_t>::Failure(
        NodeErrorKind::kBusinessError,
        "SplitPayloads chunk count exceeds Int32 capacity for req_id=" +
            std::to_string(parent.req_id) +
            ", sub_id=" + std::to_string(parent.sub_id),
        BatchFailureDetail{"SplitPayloads", BatchFailureReason::kCountOverflow,
                           parent});
  }
  return NodeResult<int32_t>::Success(static_cast<int32_t>(count));
}

template <typename Fn, typename InPayload>
struct SplitResultTraits {
  using RawResult = std::invoke_result_t<Fn, const InPayload&>;
  static constexpr bool kReturnsNodeResult = IsNodeResultType<RawResult>::value;
  using ValueType = typename std::conditional_t<
      kReturnsNodeResult, typename IsNodeResultType<RawResult>::ValueType,
      RawResult>;
  using OutputPayload = typename ValueType::value_type;
};

template <typename InputPayload, typename SplitFn>
auto SplitPayloadsInternal(
    const std::vector<TraceableItem<InputPayload>>& input, SplitFn&& split_one,
    const std::unordered_map<uint32_t, uint64_t>& initial_sub_id_by_req) {
  using Traits = SplitResultTraits<std::decay_t<SplitFn>, InputPayload>;
  using OutPayload = typename Traits::OutputPayload;
  using ResultType = SplitResult<OutPayload>;

  std::unordered_set<TraceableItemKey> seen_keys;
  for (const auto& item : input) {
    TraceableItemKey key{item.req_id, item.sub_id};
    if (!seen_keys.insert(key).second) {
      return NodeResult<ResultType>::Failure(
          NodeErrorKind::kInputError,
          "SplitPayloads duplicate input item for req_id=" +
              std::to_string(key.req_id) +
              ", sub_id=" + std::to_string(key.sub_id),
          BatchFailureDetail{"SplitPayloads", BatchFailureReason::kDuplicate,
                             key});
    }
  }

  std::unordered_map<uint32_t, uint64_t> next_sub_id_by_req =
      initial_sub_id_by_req;
  std::vector<TraceableItem<OutPayload>> children;
  Int32Batch counts;
  counts.reserve(input.size());

  for (const auto& parent : input) {
    std::vector<OutPayload> child_items;
    try {
      if constexpr (Traits::kReturnsNodeResult) {
        auto res = split_one(parent.data);
        if (!res.ok()) {
          auto failure = std::move(res).ExtractFailure();
          failure.batch_detail = BatchFailureDetail{
              "SplitPayloads", BatchFailureReason::kCallbackFailed,
              TraceableItemKey{parent.req_id, parent.sub_id}};
          return NodeResult<ResultType>::Failure(std::move(failure));
        }
        child_items = std::move(res).value();
      } else {
        child_items = split_one(parent.data);
      }
    } catch (const std::exception& e) {
      return NodeResult<ResultType>::Failure(
          NodeErrorKind::kBusinessError,
          std::string("SplitPayloads split_one threw exception: ") + e.what(),
          BatchFailureDetail{"SplitPayloads",
                             BatchFailureReason::kCallbackFailed,
                             TraceableItemKey{parent.req_id, parent.sub_id}});
    } catch (...) {
      return NodeResult<ResultType>::Failure(
          NodeErrorKind::kBusinessError,
          "SplitPayloads split_one threw unknown exception",
          BatchFailureDetail{"SplitPayloads",
                             BatchFailureReason::kCallbackFailed,
                             TraceableItemKey{parent.req_id, parent.sub_id}});
    }

    auto count =
        CheckedSplitCount(child_items.size(), {parent.req_id, parent.sub_id});
    if (!count.ok()) {
      return NodeResult<ResultType>::Failure(std::move(count).ExtractFailure());
    }

    uint64_t& next_sub_id = next_sub_id_by_req[parent.req_id];
    for (auto& payload : child_items) {
      if (next_sub_id >
          static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return NodeResult<ResultType>::Failure(
            NodeErrorKind::kBusinessError,
            "SplitPayloads sub_id overflow for req_id=" +
                std::to_string(parent.req_id),
            BatchFailureDetail{"SplitPayloads",
                               BatchFailureReason::kSubIdOverflow,
                               TraceableItemKey{parent.req_id, parent.sub_id}});
      }
      children.emplace_back(parent.req_id, static_cast<uint32_t>(next_sub_id++),
                            std::move(payload));
    }

    counts.emplace_back(parent.req_id, parent.sub_id, count.value());
  }

  return NodeResult<ResultType>::Success(
      ResultType{std::move(children), std::move(counts)});
}

}  // namespace detail

template <typename InputPayload, typename SplitFn>
auto SplitPayloads(const std::vector<TraceableItem<InputPayload>>& input,
                   SplitFn&& split_one) {
  return detail::SplitPayloadsInternal(input, std::forward<SplitFn>(split_one),
                                       {});
}

}  // namespace llm_edgeflow
