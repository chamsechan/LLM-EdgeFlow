#pragma once

#include <string_view>

#include "adapter/operator_value_type.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
/**
 * @brief CompanyAny 白名单类型描述符
 */
struct CompanyAnyTypeDescriptor {
  int32_t type_id = 0;
  size_t element_size = 0;
  size_t alignment = 0;
  const char* debug_name = nullptr;
};

/**
 * @brief 根据 type_id 查找 CompanyAny 元素类型描述 (白名单)
 */
const CompanyAnyTypeDescriptor* FindCompanyAnyType(int32_t type_id) noexcept;

/**
 * @brief 校验 CompanyString 合法性 (带显式长度、上限与嵌入 NUL 检查)
 */
int ValidateCompanyString(const CompanyString* str, size_t max_bytes,
                          const char* field_name, std::string* err) noexcept;

/**
 * @brief 校验 CompanyBuffer 合法性
 */
int ValidateCompanyBuffer(const CompanyBuffer* buf, size_t max_bytes,
                          const char* field_name, std::string* err) noexcept;

/**
 * @brief 校验 CompanyAny 合法性 (受类型白名单与尺寸乘法方程校验)
 */
int ValidateCompanyAnyPayload(const CompanyAny* any, size_t max_any_bytes,
                              const char* field_name,
                              std::string* err) noexcept;

DECLARE_EXTERNAL_TYPE_TRAITS(CompanyString, "CompanyString");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyBuffer, "CompanyBuffer");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyAny, "CompanyAny");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyFrame, "CompanyFrame");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOdOutput, "CompanyOdOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorAuditInput,
                             "CompanyOperatorAuditInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorAuditOutput,
                             "CompanyOperatorAuditOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorKeywordInput,
                             "CompanyOperatorKeywordInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorKeywordOutput,
                             "CompanyOperatorKeywordOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorEntityInput,
                             "CompanyOperatorEntityInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorEntityOutput,
                             "CompanyOperatorEntityOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorDocInput,
                             "CompanyOperatorDocInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorDocOutput,
                             "CompanyOperatorDocOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorAudioInput,
                             "CompanyOperatorAudioInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorAudioOutput,
                             "CompanyOperatorAudioOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorRerankInput,
                             "CompanyOperatorRerankInput");
DECLARE_EXTERNAL_TYPE_TRAITS(CompanyOperatorRerankOutput,
                             "CompanyOperatorRerankOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(int, "int");

bool ValidatePlatformMetadata(uint32_t count, int32_t type_id) noexcept;

namespace operator_value_detail {
CompanyString* AllocateNestedCompanyString(uint32_t capacity,
                                           OwnedExternalBlock* block);
CompanyAny* AllocateNestedCompanyAny(uint32_t count, int32_t type_id,
                                     OwnedExternalBlock* block);
void ResetNestedCompanyString(CompanyString* value) noexcept;
void ResetNestedCompanyAny(CompanyAny* value) noexcept;
bool ComputeStandardOutputBlockPayloadBytes(size_t root_bytes,
                                            const ResolvedOutputPoolSpec& spec,
                                            size_t* bytes,
                                            std::string* error) noexcept;
}  // namespace operator_value_detail

// 每个池化字符串只声明一次，用于容量校验、分配和重置。
// 成员指针使描述符与具体的 C 结构绑定。
template <typename T>
struct OutputStringField {
  std::string name;
  CompanyString* T::*member;
  OutputCapacityFieldConfig capacity;
};

template <typename T, typename ResetScalars>
OperatorValueTypeBinding MakePooledOutputBinding(
    const char* suffix, std::vector<OutputStringField<T>> string_fields,
    ResetScalars reset_scalars, CompanyAny* T::*metadata_field = nullptr,
    uint32_t max_metadata_elements = 0) {
  static_assert(std::is_nothrow_invocable_v<ResetScalars, T&>);
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = suffix;
  binding.external_c_type_name = HostTypeName<T>();
  binding.direction = IoDirection::kOutput;
  for (size_t i = 0; i < string_fields.size(); ++i) {
    const auto& field = string_fields[i];
    if (field.name.empty() || field.member == nullptr ||
        !binding.output_layout.string_capacity_fields
             .emplace(field.name, field.capacity)
             .second) {
      throw std::invalid_argument(
          "Pooled output requires unique named string fields");
    }
    for (size_t j = 0; j < i; ++j) {
      if (string_fields[j].member == field.member) {
        throw std::invalid_argument(
            "Pooled output string member is declared twice");
      }
    }
  }
  if (!metadata_field && max_metadata_elements != 0) {
    throw std::invalid_argument("Metadata capacity requires a metadata member");
  }
  binding.output_layout.max_metadata_elements = max_metadata_elements;
  binding.output_layout.validate_metadata = ValidatePlatformMetadata;
  binding.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec& spec, size_t* out_bytes,
         std::string* err) noexcept {
        return operator_value_detail::ComputeStandardOutputBlockPayloadBytes(
            sizeof(T), spec, out_bytes, err);
      };
  binding.allocate_external = [string_fields, metadata_field, reset_scalars](
                                  const ResolvedOutputPoolSpec& spec,
                                  OwnedExternalBlock* block,
                                  std::string*) -> int {
    // 分配前先预留：一个根块，每个嵌套字段再加一个包装和一个数据缓冲区。
    // OwnedExternalBlock 负责回滚部分失败。
    block->cleanups.reserve(1 + 2 * string_fields.size() +
                            (metadata_field ? 2 : 0));
    auto* raw = block->Own(std::make_unique<T>());
    reset_scalars(*raw);
    for (const auto& field : string_fields) {
      raw->*field.member = operator_value_detail::AllocateNestedCompanyString(
          spec.GetCapacity(field.name), block);
    }
    if (metadata_field) {
      raw->*metadata_field = operator_value_detail::AllocateNestedCompanyAny(
          spec.meta_num, spec.metadata_type_id, block);
    }
    block->raw_struct = raw;
    return 0;
  };
  binding.reset_external = [string_fields, metadata_field, reset_scalars](
                               void* ptr,
                               const ResolvedOutputPoolSpec&) noexcept {
    if (!ptr) return;
    auto* raw = static_cast<T*>(ptr);
    // 只重置值；嵌套存储和元数据类型在复用时保留。
    reset_scalars(*raw);
    for (const auto& field : string_fields) {
      operator_value_detail::ResetNestedCompanyString(raw->*field.member);
    }
    if (metadata_field)
      operator_value_detail::ResetNestedCompanyAny(raw->*metadata_field);
  };
  binding.destroy_external = [](OwnedExternalBlock* block) noexcept {
    if (block) block->Destroy();
  };
  return binding;
}

int CopyToOperatorString(std::string_view src, CompanyString* dest,
                         uint32_t capacity, const char* field_name,
                         std::string* error) noexcept;

}  // namespace llm_edgeflow
