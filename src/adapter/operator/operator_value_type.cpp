#include "adapter/operator_value_type.h"

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
