#include "adapter/platform_value_binding.h"

#include <cstring>
#include <limits>

#include "adapter/operator/operator_value_type_registry.h"
#include "contracts/diagnostic.h"

namespace llm_edgeflow::operator_value_detail {

CompanyString* AllocateNestedCompanyString(uint32_t capacity,
                                           OwnedExternalBlock* block) {
  auto value = std::make_unique<CompanyString>();
  value->data = block->OwnArray(
      std::make_unique<char[]>(static_cast<size_t>(capacity) + 1));
  return block->Own(std::move(value));
}

CompanyAny* AllocateNestedCompanyAny(uint32_t count, int32_t type_id,
                                     OwnedExternalBlock* block) {
  if (count == 0 || type_id == 0) return nullptr;
  const auto* descriptor = FindCompanyAnyType(type_id);
  size_t bytes = 0;
  if (!descriptor || descriptor->element_size == 0 ||
      !CheckedMultiply(count, descriptor->element_size, &bytes)) {
    throw std::invalid_argument("Invalid pooled metadata layout");
  }
  auto value = std::make_unique<CompanyAny>();
  value->type_id = type_id;
  value->data = block->OwnArray(std::make_unique<uint8_t[]>(bytes));
  return block->Own(std::move(value));
}

void ResetNestedCompanyString(CompanyString* value) noexcept {
  if (!value) return;
  value->length = 0;
  if (value->data) value->data[0] = '\0';
}

void ResetNestedCompanyAny(CompanyAny* value) noexcept {
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
          !CheckedAdd(string_bytes, sizeof(CompanyString), &string_bytes) ||
          !CheckedAdd(block_bytes, string_bytes, &block_bytes)) {
        if (err) {
          *err = spec.type + " capacity calculation overflowed for field '" +
                 field + "'";
        }
        return false;
      }
    }

    if (spec.meta_num > 0) {
      const auto* metadata_desc = FindCompanyAnyType(spec.metadata_type_id);
      if (!metadata_desc || metadata_desc->element_size == 0) {
        if (err) *err = "Metadata type is not registered";
        return false;
      }
      size_t metadata_payload = 0;
      size_t metadata_bytes = 0;
      if (!CheckedMultiply(spec.meta_num, metadata_desc->element_size,
                           &metadata_payload) ||
          !CheckedAdd(metadata_payload, sizeof(CompanyAny), &metadata_bytes) ||
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
int CopyToOperatorString(std::string_view src, CompanyString* dest,
                         uint32_t capacity, const char* field_name,
                         std::string* err) noexcept {
  try {
    if (!dest || !dest->data) {
      if (err)
        *err = std::string(field_name ? field_name : "string") +
               " in destination pool block is null";
      return -4;
    }
    const size_t len = src.size();
    if (len > capacity ||
        len > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
      if (err)
        *err = std::string(field_name ? field_name : "string") +
               " output length (" + std::to_string(len) +
               ") exceeds pool capacity (" + std::to_string(capacity) + ")";
      return -4;
    }
    if (len != 0) std::memcpy(dest->data, src.data(), len);
    dest->data[len] = '\0';
    dest->length = static_cast<int32_t>(len);
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return -4;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception in CopyToOperatorString");
    return -4;
  }
}

}  // namespace llm_edgeflow

namespace llm_edgeflow {
bool ValidatePlatformMetadata(uint32_t count, int32_t type_id) noexcept {
  return count == 0 ? type_id == 0
                    : type_id != 0 && FindCompanyAnyType(type_id);
}
}  // namespace llm_edgeflow

namespace llm_edgeflow {
namespace {

const CompanyAnyTypeDescriptor kBuiltinAnyTypes[] = {
    {0, 0, 1, "none"},
    {1, sizeof(float), alignof(float), "float32"},
    {2, sizeof(int32_t), alignof(int32_t), "int32"},
    {3, sizeof(uint8_t), alignof(uint8_t), "uint8"},
    {4, sizeof(int64_t), alignof(int64_t), "int64"},
    {5, sizeof(double), alignof(double), "float64"},
};

}  // namespace

const CompanyAnyTypeDescriptor* FindCompanyAnyType(int32_t type_id) noexcept {
  for (const auto& item : kBuiltinAnyTypes) {
    if (item.type_id == type_id) {
      return &item;
    }
  }
  return nullptr;
}

int ValidateCompanyString(const CompanyString* str, size_t max_bytes,
                          const char* field_name, std::string* err) noexcept {
  try {
    const char* name = field_name ? field_name : "string";
    if (!str) {
      if (err) *err = std::string(name) + " pointer is null";
      return -3;
    }
    if (str->length < 0) {
      if (err)
        *err = std::string(name) + " has negative length " +
               std::to_string(str->length);
      return -3;
    }
    if (static_cast<size_t>(str->length) > max_bytes) {
      if (err)
        *err = std::string(name) + " length " + std::to_string(str->length) +
               " exceeds max limit " + std::to_string(max_bytes);
      return -3;
    }
    if (str->length > 0) {
      if (!str->data) {
        if (err)
          *err = std::string(name) + " length is " +
                 std::to_string(str->length) + " but data pointer is null";
        return -3;
      }
      for (int32_t i = 0; i < str->length; ++i) {
        if (str->data[i] == '\0') {
          if (err)
            *err = std::string(name) +
                   " contains forbidden embedded NUL at byte offset " +
                   std::to_string(i);
          return -3;
        }
      }
    }
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return -3;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception in ValidateCompanyString");
    return -3;
  }
}

int ValidateCompanyBuffer(const CompanyBuffer* buf, size_t max_bytes,
                          const char* field_name, std::string* err) noexcept {
  try {
    const char* name = field_name ? field_name : "buffer";
    if (!buf) {
      if (err) *err = std::string(name) + " pointer is null";
      return -3;
    }
    if (buf->length < 0) {
      if (err)
        *err = std::string(name) + " has negative length " +
               std::to_string(buf->length);
      return -3;
    }
    if (static_cast<size_t>(buf->length) > max_bytes) {
      if (err)
        *err = std::string(name) + " length exceeds max limit " +
               std::to_string(max_bytes);
      return -3;
    }
    if (buf->length > 0 && !buf->data) {
      if (err) *err = std::string(name) + " data pointer is null";
      return -3;
    }
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return -3;
  } catch (...) {
    SetDiagnosticNoexcept(err, "Unknown exception in ValidateCompanyBuffer");
    return -3;
  }
}

int ValidateCompanyAnyPayload(const CompanyAny* any, size_t max_any_bytes,
                              const char* field_name,
                              std::string* err) noexcept {
  try {
    const char* name = field_name ? field_name : "any";
    if (!any) {
      if (err) *err = std::string(name) + " pointer is null";
      return -3;
    }
    if (any->element_count < 0 || any->byte_length < 0) {
      if (err) {
        *err = std::string(name) + " has negative count or length: count=" +
               std::to_string(any->element_count) +
               ", length=" + std::to_string(any->byte_length);
      }
      return -3;
    }
    if (static_cast<size_t>(any->byte_length) > max_any_bytes) {
      if (err) {
        *err = std::string(name) + " byte_length " +
               std::to_string(any->byte_length) + " exceeds max limit " +
               std::to_string(max_any_bytes);
      }
      return -3;
    }
    if (any->type_id == 0) {
      if (any->element_count != 0 || any->byte_length != 0) {
        if (err) {
          *err = std::string(name) + " has type_id=0 but non-zero count/length";
        }
        return -3;
      }
      return 0;
    }

    const auto* desc = FindCompanyAnyType(any->type_id);
    if (!desc) {
      if (err) {
        *err = std::string(name) + " has unknown or unwhitelisted type_id " +
               std::to_string(any->type_id);
      }
      return -3;
    }

    size_t expected_bytes = 0;
    if (!CheckedMultiply(static_cast<size_t>(any->element_count),
                         desc->element_size, &expected_bytes)) {
      if (err) {
        *err = std::string(name) + " element_count multiplication overflowed";
      }
      return -3;
    }
    if (expected_bytes != static_cast<size_t>(any->byte_length)) {
      if (err) {
        *err = std::string(name) + " size equation mismatch: expected " +
               std::to_string(expected_bytes) + " bytes for " +
               std::to_string(any->element_count) + " elements of type " +
               desc->debug_name + ", but byte_length is " +
               std::to_string(any->byte_length);
      }
      return -3;
    }
    if (any->byte_length > 0 && !any->data) {
      if (err) {
        *err = std::string(name) + " non-empty payload has null data";
      }
      return -3;
    }
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(err, e.what());
    return -3;
  } catch (...) {
    SetDiagnosticNoexcept(err,
                          "Unknown exception in ValidateCompanyAnyPayload");
    return -3;
  }
}

}  // namespace llm_edgeflow
