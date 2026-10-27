#pragma once

#include <string_view>

#include "adapter/operator_value_type.h"
#include "edgeflow/operator/types.h"

namespace llm_edgeflow {
/**
 * @brief MockAny 白名单类型描述符
 */
struct MockAnyTypeDescriptor {
  int32_t type_id = 0;
  size_t element_size = 0;
  size_t alignment = 0;
  const char* debug_name = nullptr;
};

/**
 * @brief 根据 type_id 查找 MockAny 元素类型描述（白名单）
 */
const MockAnyTypeDescriptor* FindMockAnyType(int32_t type_id) noexcept;

/**
 * @brief 按 envelope 层级校验文本请求：root byte_size/type/num_frame →
 * data pool byte_size/data_type → 文本 byte_size/长度上限/指针/嵌入 NUL
 */
int ValidateMockInputText(const MockInput& request, size_t max_bytes,
                          const char* field_name, std::string* err) noexcept;

/**
 * @brief 读取已通过 ValidateMockInputText 的文本（借用请求内存）
 */
std::string_view MockInputText(const MockInput& request) noexcept;

/**
 * @brief 校验 MockString 合法性（带显式长度、上限与嵌入 NUL 检查）
 */
int ValidateMockString(const MockString* str, size_t max_bytes,
                       const char* field_name, std::string* err) noexcept;

/**
 * @brief 校验 MockAny 合法性（受类型白名单与尺寸乘法方程校验）
 */
int ValidateMockAnyPayload(const MockAny* any, size_t max_any_bytes,
                           const char* field_name, std::string* err) noexcept;

DECLARE_EXTERNAL_TYPE_TRAITS(MockString, "MockString");
DECLARE_EXTERNAL_TYPE_TRAITS(MockFrameInput, "MockFrameInput");
DECLARE_EXTERNAL_TYPE_TRAITS(MockInput, "MockInput");
DECLARE_EXTERNAL_TYPE_TRAITS(MockOutput, "MockOutput");
DECLARE_EXTERNAL_TYPE_TRAITS(MockAuditInput, "MockAuditInput");
DECLARE_EXTERNAL_TYPE_TRAITS(MockDocInput, "MockDocInput");
DECLARE_EXTERNAL_TYPE_TRAITS(MockAudioInput, "MockAudioInput");
DECLARE_EXTERNAL_TYPE_TRAITS(MockRerankInput, "MockRerankInput");

bool ValidatePlatformMetadata(uint32_t count, int32_t type_id) noexcept;

namespace operator_value_detail {
// 分配嵌套 MockTextOutput 与 capacity + 1 字节缓冲区，byte_size 设为 sizeof。
MockTextOutput* AllocateNestedTextOutput(uint32_t capacity,
                                         OwnedExternalBlock* block);
MockAny* AllocateNestedMockAny(uint32_t count, int32_t type_id,
                               OwnedExternalBlock* block);
// 只清空 length 与首字节，保留 byte_size 和缓冲区。
void ResetNestedTextOutput(MockTextOutput* value) noexcept;
void ResetNestedMockAny(MockAny* value) noexcept;
// root + 每个文本字段 (sizeof(MockTextOutput) + capacity + 1) + 可选元数据。
bool ComputeStandardOutputBlockPayloadBytes(size_t root_bytes,
                                            const ResolvedOutputPoolSpec& spec,
                                            size_t* bytes,
                                            std::string* error) noexcept;
}  // namespace operator_value_detail

// 每个池化文本字段只声明一次，用于容量校验、分配和重置；capacity 是
// binding 硬上限，实际容量由 Converter 参数经 Audit 归一化后冻结。
// 成员指针使描述符与具体的 C 结构绑定。
template <typename T>
struct OutputTextField {
  std::string name;
  MockTextOutput* T::*member;
  OutputCapacityFieldConfig capacity;
};

// reset_scalars 在分配与每次复用时写根结构标量 (byte_size、type 等协议字段
// 置为固定值，业务字段清零)，嵌套文本由框架清空。
template <typename T, typename ResetScalars>
OperatorValueTypeBinding MakePooledOutputBinding(
    const char* suffix, std::vector<OutputTextField<T>> text_fields,
    ResetScalars reset_scalars, MockAny* T::*metadata_field = nullptr,
    uint32_t max_metadata_elements = 0) {
  static_assert(std::is_nothrow_invocable_v<ResetScalars, T&>);
  OperatorValueTypeBinding binding;
  binding.canonical_suffix = suffix;
  binding.external_c_type_name = HostTypeName<T>();
  binding.direction = IoDirection::kOutput;
  for (size_t i = 0; i < text_fields.size(); ++i) {
    const auto& field = text_fields[i];
    if (field.name.empty() || field.member == nullptr ||
        !binding.output_layout.string_capacity_fields
             .emplace(field.name, field.capacity)
             .second) {
      throw std::invalid_argument(
          "Pooled output requires unique named text fields");
    }
    for (size_t j = 0; j < i; ++j) {
      if (text_fields[j].member == field.member) {
        throw std::invalid_argument(
            "Pooled output text member is declared twice");
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
  binding.allocate_external = [text_fields, metadata_field, reset_scalars](
                                  const ResolvedOutputPoolSpec& spec,
                                  OwnedExternalBlock* block,
                                  std::string*) -> int {
    // 分配前先预留：一个根块，每个嵌套字段再加一个包装和一个数据缓冲区。
    // OwnedExternalBlock 负责回滚部分失败。
    block->cleanups.reserve(1 + 2 * text_fields.size() +
                            (metadata_field ? 2 : 0));
    auto* raw = block->Own(std::make_unique<T>());
    reset_scalars(*raw);
    for (const auto& field : text_fields) {
      raw->*field.member = operator_value_detail::AllocateNestedTextOutput(
          spec.GetCapacity(field.name), block);
    }
    if (metadata_field) {
      raw->*metadata_field = operator_value_detail::AllocateNestedMockAny(
          spec.meta_num, spec.metadata_type_id, block);
    }
    block->raw_struct = raw;
    return 0;
  };
  binding.reset_external = [text_fields, metadata_field, reset_scalars](
                               void* ptr,
                               const ResolvedOutputPoolSpec&) noexcept {
    if (!ptr) return;
    auto* raw = static_cast<T*>(ptr);
    // 只重置值；嵌套存储和元数据类型在复用时保留。
    reset_scalars(*raw);
    for (const auto& field : text_fields) {
      operator_value_detail::ResetNestedTextOutput(raw->*field.member);
    }
    if (metadata_field)
      operator_value_detail::ResetNestedMockAny(raw->*metadata_field);
  };
  binding.destroy_external = [](OwnedExternalBlock* block) noexcept {
    if (block) block->Destroy();
  };
  return binding;
}

// 先校验嵌套布局并清空旧值；src 超过 capacity 时直接失败，不写入部分内容。
// 成功才复制字节、写结尾 NUL 并设置 length。
int CopyToMockTextOutput(std::string_view src, MockTextOutput* dest,
                         uint32_t capacity, const char* field_name,
                         std::string* error) noexcept;

}  // namespace llm_edgeflow
