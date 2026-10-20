#include "adapter/operator/mock/platform_value_binding.h"

#include <cstring>
#include <limits>

#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/diagnostic.h"
#include "contracts/error_codes.h"

namespace llm_edgeflow::operator_value_detail {

MockTextOutput* AllocateNestedTextOutput(uint32_t capacity,
                                         OwnedExternalBlock* block) {
  auto value = std::make_unique<MockTextOutput>();
  value->byte_size = static_cast<int32_t>(sizeof(MockTextOutput));
  value->value = block->OwnArray(
      std::make_unique<char[]>(static_cast<size_t>(capacity) + 1));
  return block->Own(std::move(value));
}

MockAny* AllocateNestedMockAny(uint32_t count, int32_t type_id,
                               OwnedExternalBlock* block) {
  if (count == 0 || type_id == 0) return nullptr;
  const auto* descriptor = FindMockAnyType(type_id);
  size_t bytes = 0;
  if (!descriptor || descriptor->element_size == 0 ||
      !CheckedMultiply(count, descriptor->element_size, &bytes)) {
    throw std::invalid_argument("Invalid pooled metadata layout");
  }
  auto value = std::make_unique<MockAny>();
  value->type_id = type_id;
  value->data = block->OwnArray(std::make_unique<uint8_t[]>(bytes));
  return block->Own(std::move(value));
}

void ResetNestedTextOutput(MockTextOutput* value) noexcept {
  if (!value) return;
  value->length = 0;
  if (value->value) value->value[0] = '\0';
}

void ResetNestedMockAny(MockAny* value) noexcept {
  if (!value) return;
  value->element_count = 0;
  value->byte_length = 0;
}

bool ComputeStandardOutputBlockPayloadBytes(size_t root_struct_bytes,
                                            const ResolvedOutputPoolSpec& spec,
                                            size_t* out_bytes,
                                            std::string* err) noexcept {
  try {
    if (!out_bytes) {
      if (err) *err = "Null output block payload pointer";
      return false;
    }
    *out_bytes = 0;

    size_t block_bytes = root_struct_bytes;
    for (const auto& [field, capacity] : spec.capacities) {
      size_t string_bytes = 0;
      if (!CheckedAdd(static_cast<size_t>(capacity), 1, &string_bytes) ||
          !CheckedAdd(string_bytes, sizeof(MockTextOutput), &string_bytes) ||
          !CheckedAdd(block_bytes, string_bytes, &block_bytes)) {
        if (err) {
          *err = spec.type + " capacity calculation overflowed for field '" +
                 field + "'";
        }
        return false;
      }
    }

    if (spec.meta_num > 0) {
      const auto* metadata_desc = FindMockAnyType(spec.metadata_type_id);
      if (!metadata_desc || metadata_desc->element_size == 0) {
        if (err) *err = "Metadata type is not registered";
        return false;
      }
      size_t metadata_payload = 0;
      size_t metadata_bytes = 0;
      if (!CheckedMultiply(spec.meta_num, metadata_desc->element_size,
                           &metadata_payload) ||
          !CheckedAdd(metadata_payload, sizeof(MockAny), &metadata_bytes) ||
          !CheckedAdd(block_bytes, metadata_bytes, &block_bytes)) {
        if (err) *err = spec.type + " metadata calculation overflowed";
        return false;
      }
    }

    *out_bytes = block_bytes;
    return true;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return false;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown output budget error");
    return false;
  }
}

}  // namespace llm_edgeflow::operator_value_detail

namespace llm_edgeflow {
int CopyToMockTextOutput(std::string_view src, MockTextOutput* dest,
                         uint32_t capacity, const char* field_name,
                         std::string* err) noexcept {
  try {
    const char* name = field_name ? field_name : "text";
    if (!dest || !dest->value ||
        dest->byte_size != static_cast<int32_t>(sizeof(MockTextOutput))) {
      if (err)
        *err = std::string(name) +
               " in destination pool block has invalid "
               "layout";
      return error::kInvalidOutput;
    }
    dest->length = 0;
    dest->value[0] = '\0';
    const size_t len = src.size();
    if (len > capacity ||
        len > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      if (err)
        *err = std::string(name) + " output length (" + std::to_string(len) +
               ") exceeds pool capacity (" + std::to_string(capacity) + ")";
      return error::kInvalidOutput;
    }
    if (len != 0) std::memcpy(dest->value, src.data(), len);
    dest->value[len] = '\0';
    dest->length = static_cast<int32_t>(len);
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return error::kInvalidOutput;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception in CopyToMockTextOutput");
    return error::kInvalidOutput;
  }
}

}  // namespace llm_edgeflow

namespace llm_edgeflow {
bool ValidatePlatformMetadata(uint32_t count, int32_t type_id) noexcept {
  return count == 0 ? type_id == 0 : type_id != 0 && FindMockAnyType(type_id);
}
}  // namespace llm_edgeflow

namespace llm_edgeflow {
namespace {

const MockAnyTypeDescriptor kBuiltinAnyTypes[] = {
    {0, 0, 1, "none"},
    {1, sizeof(float), alignof(float), "float32"},
    {2, sizeof(int32_t), alignof(int32_t), "int32"},
    {3, sizeof(uint8_t), alignof(uint8_t), "uint8"},
    {4, sizeof(int64_t), alignof(int64_t), "int64"},
    {5, sizeof(double), alignof(double), "float64"},
};

}  // namespace

const MockAnyTypeDescriptor* FindMockAnyType(int32_t type_id) noexcept {
  for (const auto& item : kBuiltinAnyTypes) {
    if (item.type_id == type_id) {
      return &item;
    }
  }
  return nullptr;
}

namespace {
template <typename T>
bool HasByteSize(int32_t byte_size) {
  return byte_size == static_cast<int32_t>(sizeof(T));
}

int CheckTextBytes(const char* data, int32_t length, size_t max_bytes,
                   const std::string& name, std::string* err) {
  if (length < 0) {
    if (err) *err = name + " has negative length " + std::to_string(length);
    return error::kInvalidInput;
  }
  if (static_cast<size_t>(length) > max_bytes) {
    if (err)
      *err = name + " length " + std::to_string(length) +
             " exceeds max limit " + std::to_string(max_bytes);
    return error::kInvalidInput;
  }
  if (length == 0) return 0;
  if (!data) {
    if (err)
      *err = name + " length is " + std::to_string(length) +
             " but data pointer is null";
    return error::kInvalidInput;
  }
  if (const void* nul = std::memchr(data, '\0', static_cast<size_t>(length))) {
    if (err)
      *err = name + " contains forbidden embedded NUL at byte offset " +
             std::to_string(static_cast<const char*>(nul) - data);
    return error::kInvalidInput;
  }
  return 0;
}
}  // namespace

int ValidateMockInputText(const MockInput& input, size_t max_bytes,
                          const char* field_name, std::string* err) noexcept {
  try {
    const std::string name = field_name ? field_name : "MockInput";
    if (!HasByteSize<MockInput>(input.byte_size)) {
      if (err)
        *err = name + ".byte_size " + std::to_string(input.byte_size) +
               " does not match MockInput layout";
      return error::kInvalidInput;
    }
    if (input.type != kMockInputMultidata || input.num_frame != 1) {
      if (err)
        *err = name + " requires type kMockInputMultidata and num_frame 1";
      return error::kInvalidInput;
    }
    const MockDataPool* pool = input.data_pool;
    if (!pool || !HasByteSize<MockDataPool>(pool->byte_size)) {
      if (err) *err = name + ".data_pool is null or has invalid byte_size";
      return error::kInvalidInput;
    }
    if (pool->data_type != kMockDataText) {
      if (err)
        *err = name + ".data_pool.data_type " +
               std::to_string(pool->data_type) + " is not kMockDataText";
      return error::kInvalidInput;
    }
    const auto* text = static_cast<const MockTextInput*>(pool->data_ptr);
    if (!text || !HasByteSize<MockTextInput>(text->byte_size)) {
      if (err)
        *err = name + ".data_pool.data_ptr is null or has invalid byte_size";
      return error::kInvalidInput;
    }
    return CheckTextBytes(text->value, text->length, max_bytes, name + ".text",
                          err);
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return error::kInvalidInput;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception in ValidateMockInputText");
    return error::kInvalidInput;
  }
}

std::string_view MockInputText(const MockInput& input) noexcept {
  const auto* text =
      static_cast<const MockTextInput*>(input.data_pool->data_ptr);
  return text->length == 0
             ? std::string_view{}
             : std::string_view(text->value, static_cast<size_t>(text->length));
}

int ValidateMockString(const MockString* str, size_t max_bytes,
                       const char* field_name, std::string* err) noexcept {
  try {
    const std::string name = field_name ? field_name : "string";
    if (!str) {
      if (err) *err = name + " pointer is null";
      return error::kInvalidInput;
    }
    return CheckTextBytes(str->data, str->length, max_bytes, name, err);
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return error::kInvalidInput;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception in ValidateMockString");
    return error::kInvalidInput;
  }
}

int ValidateMockAnyPayload(const MockAny* any, size_t max_any_bytes,
                           const char* field_name, std::string* err) noexcept {
  try {
    const char* name = field_name ? field_name : "any";
    if (!any) {
      if (err) *err = std::string(name) + " pointer is null";
      return error::kInvalidInput;
    }
    if (any->element_count < 0 || any->byte_length < 0) {
      if (err) {
        *err = std::string(name) + " has negative count or length: count=" +
               std::to_string(any->element_count) +
               ", length=" + std::to_string(any->byte_length);
      }
      return error::kInvalidInput;
    }
    if (static_cast<size_t>(any->byte_length) > max_any_bytes) {
      if (err) {
        *err = std::string(name) + " byte_length " +
               std::to_string(any->byte_length) + " exceeds max limit " +
               std::to_string(max_any_bytes);
      }
      return error::kInvalidInput;
    }
    if (any->type_id == 0) {
      if (any->element_count != 0 || any->byte_length != 0) {
        if (err) {
          *err = std::string(name) + " has type_id=0 but non-zero count/length";
        }
        return error::kInvalidInput;
      }
      return 0;
    }

    const auto* desc = FindMockAnyType(any->type_id);
    if (!desc) {
      if (err) {
        *err = std::string(name) + " has unknown or unwhitelisted type_id " +
               std::to_string(any->type_id);
      }
      return error::kInvalidInput;
    }

    size_t expected_bytes = 0;
    if (!CheckedMultiply(static_cast<size_t>(any->element_count),
                         desc->element_size, &expected_bytes)) {
      if (err) {
        *err = std::string(name) + " element_count multiplication overflowed";
      }
      return error::kInvalidInput;
    }
    if (expected_bytes != static_cast<size_t>(any->byte_length)) {
      if (err) {
        *err = std::string(name) + " size equation mismatch: expected " +
               std::to_string(expected_bytes) + " bytes for " +
               std::to_string(any->element_count) + " elements of type " +
               desc->debug_name + ", but byte_length is " +
               std::to_string(any->byte_length);
      }
      return error::kInvalidInput;
    }
    if (any->byte_length > 0 && !any->data) {
      if (err) {
        *err = std::string(name) + " non-empty payload has null data";
      }
      return error::kInvalidInput;
    }
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return error::kInvalidInput;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception in ValidateMockAnyPayload");
    return error::kInvalidInput;
  }
}

}  // namespace llm_edgeflow
