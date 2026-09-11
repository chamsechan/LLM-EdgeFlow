#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "adapter/biz_results.h"
#include "adapter/operator_value_type.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow::test_support {

struct NestedOutputParameters {
  int32_t kind = 1;
  uint32_t capacity = 3;
  bool reject_hit = false;
};

// Neutral test carrier: the same outer type can contain either nested layout.
struct NestedOutputPayload {
  uint32_t capacity = 0;
  uint32_t count = 0;
  void* values = nullptr;
};

struct NestedOutputEnvelope {
  uint64_t request_id = 0;
  int32_t allocator_tag = 0;
  int32_t kind = 0;
  void* payload = nullptr;
};

inline std::atomic<int> nested_allocations{0};
inline std::atomic<int> nested_resets{0};
inline std::atomic<int> nested_destroys{0};

inline bool ParseNestedOutput(const std::string& text,
                              NestedOutputParameters* parameters,
                              std::string* error) {
  const auto reject = [&]() {
    if (error) *error = "Invalid nested output params";
    return false;
  };
  const auto requested = nlohmann::json::parse(text);
  if (!requested.is_object() || !parameters) return reject();
  for (const auto& item : requested.items()) {
    if (item.key() != "kind" && item.key() != "capacity" &&
        item.key() != "reject_hit") {
      return reject();
    }
  }
  const auto kind = requested.value("kind", nlohmann::json(1));
  const auto capacity = requested.value("capacity", nlohmann::json(3));
  const auto reject_hit = requested.value("reject_hit", nlohmann::json(false));
  if (!kind.is_number_integer() || (kind != 1 && kind != 2) ||
      !capacity.is_number_integer() || capacity < 1 || capacity > 65536 ||
      !reject_hit.is_boolean()) {
    return reject();
  }
  parameters->kind = kind.get<int32_t>();
  parameters->capacity = capacity.get<uint32_t>();
  parameters->reject_hit = reject_hit.get<bool>();
  return true;
}

inline OperatorValueTypeBinding MakeNestedOutputBinding(int32_t tag = 1) {
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = "test_nested_out";
  binding.external_c_type_name = "NestedOutputEnvelope";
  binding.direction = IoDirection::kOutput;
  binding.normalize_parameters =
      MakeOutputParameterParser<NestedOutputParameters>(ParseNestedOutput);
  binding.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec& spec, size_t* bytes, std::string*) {
        if (!bytes) return false;
        *bytes = sizeof(NestedOutputEnvelope) + sizeof(NestedOutputPayload) +
                 spec.Parameters<NestedOutputParameters>().capacity *
                     sizeof(int32_t);
        return true;
      };
  binding.allocate_external = [tag](const ResolvedOutputPoolSpec& spec,
                                    OwnedExternalBlock* block, std::string*) {
    auto* root = block->Own(std::make_unique<NestedOutputEnvelope>());
    block->raw_struct = root;
    root->allocator_tag = tag;
    const auto& parameters = spec.Parameters<NestedOutputParameters>();
    root->kind = parameters.kind;
    auto* payload = block->Own(std::make_unique<NestedOutputPayload>());
    root->payload = payload;
    payload->capacity = parameters.capacity;
    if (root->kind == 1) {
      payload->values =
          block->OwnArray(std::make_unique<int32_t[]>(payload->capacity));
    } else {
      payload->values =
          block->OwnArray(std::make_unique<float[]>(payload->capacity));
    }
    ++nested_allocations;
    return 0;
  };
  binding.reset_external = [](void* ptr, const ResolvedOutputPoolSpec&) {
    auto* root = static_cast<NestedOutputEnvelope*>(ptr);
    auto* payload = static_cast<NestedOutputPayload*>(root->payload);
    root->request_id = 0;
    payload->count = 0;
    if (root->kind == 1) {
      std::fill_n(static_cast<int32_t*>(payload->values), payload->capacity, 0);
    } else {
      std::fill_n(static_cast<float*>(payload->values), payload->capacity,
                  0.0f);
    }
    ++nested_resets;
  };
  binding.destroy_external = [](OwnedExternalBlock* block) {
    ++nested_destroys;
    block->Destroy();
  };
  return binding;
}

inline int ConvertNestedOutput(const void* internal, void* external,
                               const ResolvedOutputPoolSpec& spec,
                               std::string* error) {
  const auto& result = *static_cast<const KeywordResult*>(internal);
  const auto& parameters = spec.Parameters<NestedOutputParameters>();
  auto& root = *static_cast<NestedOutputEnvelope*>(external);
  auto& payload = *static_cast<NestedOutputPayload*>(root.payload);
  if (root.kind != parameters.kind || payload.capacity != parameters.capacity ||
      root.request_id != 0 || payload.count != 0) {
    if (error) *error = "Allocation, conversion and reset contracts disagree";
    return -4;
  }
  // Fail after modifying this slot, so rollback must reset every acquired slot.
  root.request_id = result.request_id;
  payload.count = payload.capacity;
  if (parameters.reject_hit && result.is_hit) {
    if (error) *error = "Configured nested conversion rejected a hit";
    return -4;
  }
  for (uint32_t i = 0; i < payload.count; ++i) {
    const int32_t value = root.allocator_tag * 100 + result.is_hit * 10 + i;
    if (root.kind == 1) {
      static_cast<int32_t*>(payload.values)[i] = value;
    } else {
      static_cast<float*>(payload.values)[i] = value + 0.5f;
    }
  }
  return 0;
}

}  // namespace llm_edgeflow::test_support
