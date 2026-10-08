#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/operator_io_contracts.h"
#include "contracts/parameter_set.h"
#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/port_definition.h"
#include "core/validated_node_plan.h"
#include "platform_mock/operator_data_types.h"

namespace llm_edgeflow {

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

/**
 * @brief 外部宿主输入批次同步只读视图
 */
class ExternalInputBatchView {
 public:
  size_t count = 0;
  std::unordered_map<std::string, std::vector<std::shared_ptr<void>>> slots;
  std::unordered_map<std::string, std::string> slot_types;

  template <typename T>
  const T* GetSlot(const std::string& slot_name, size_t index) const {
    auto it = slots.find(slot_name);
    if (it == slots.end() || index >= it->second.size()) return nullptr;
    if constexpr (!std::is_void_v<T>) {
      auto type = slot_types.find(slot_name);
      if (type == slot_types.end() ||
          type->second != ExternalTypeTraits<T>::TypeName())
        return nullptr;
    }
    return static_cast<const T*>(it->second[index].get());
  }
};

/** 同步 Encode 借用租约池中的槽位和不可变规格。 */
class ExternalOutputBatchView {
 public:
  size_t count = 0;
  std::unordered_map<std::string, std::vector<void*>> leased_slots;
  std::unordered_map<std::string, std::string> slot_types;
  std::unordered_map<std::string, const ResolvedOutputPoolSpec*> pool_specs;

  const ResolvedOutputPoolSpec* GetPoolSpec(
      const std::string& slot_name) const {
    auto it = pool_specs.find(slot_name);
    return it == pool_specs.end() ? nullptr : it->second;
  }

  template <typename T>
  T* GetSlot(const std::string& slot_name, size_t index) const {
    auto it = leased_slots.find(slot_name);
    if (it == leased_slots.end() || index >= it->second.size()) return nullptr;
    if constexpr (!std::is_void_v<T>) {
      auto type = slot_types.find(slot_name);
      if (type == slot_types.end() ||
          type->second != ExternalTypeTraits<T>::TypeName())
        return nullptr;
    }
    return static_cast<T*>(it->second[index]);
  }
};

/**
 * @brief 输入解码选项与调用诊断上下文
 */
struct InputDecodeOptions {
  std::string type;  // 宿主结构体（map key 后缀）
  std::string name;  // 业务名
  // 每次调用独有的表，归 Operator 所有。尽管 options 为 const，Converter
  // 仍按输入顺序在此写入每行的外部请求 ID；结构体不带 request_id 成员时
  // 保持为空（多项输入的配对见 5.5）。
  std::vector<uint64_t>* request_ids = nullptr;
  // 本句柄解析好的 converter 参数，同一句柄的所有调用只读共享。
  const ParameterValues* params = nullptr;

  // 日志与诊断中的登记标识，形如 "doc_in/doc_qa"。
  std::string Label() const { return type + "/" + name; }

  template <typename P>
  const P& Params() const {
    if (!params) throw std::logic_error("Converter parameters are not set");
    return params->Get<P>();
  }
};

/**
 * @brief 输出编码选项与调用诊断上下文
 */
struct OutputEncodeOptions {
  std::string type;
  std::string name;
  // 同一张调用表的只读视图：第 i 项是第 i 个输入行的外部 ID。
  const std::vector<uint64_t>* request_ids = nullptr;
  const ParameterValues* params = nullptr;

  std::string Label() const { return type + "/" + name; }

  template <typename P>
  const P& Params() const {
    if (!params) throw std::logic_error("Converter parameters are not set");
    return params->Get<P>();
  }
};

// 单批 Process 的条数上限的框架常量；有效上限再按输出池深度收紧。
inline constexpr size_t kMaxProcessBatchSize = 64;
// 保留的业务名：表示该结构体的默认处理，不核对 service_type。
inline constexpr char kCommonIoName[] = "common";

/**
 * @brief 外部槽位定义：一个登记只对应一个宿主结构体
 */
struct ExternalSlotDefinition {
  std::string type_id;  // 宿主结构名，例如 "CompanyOperatorDocInput"
  std::string type_suffix;  // 宿主 map key 后缀，即登记的 type
  bool required = true;
  // 以下仅用于输出槽，由 converter 固定，不进入配置：
  std::string allocator;  // 空：结构体的标准布局；非空：命名布局
  std::string allocator_params;  // 命名布局的参数（JSON 文本），注册审计时解析
  uint32_t metadata_count = 0;  // 结构体带 metadata 成员时分配的元素数
  int32_t metadata_type_id = 0;

  // 交给布局解析的参数文本；未声明时为 "{}"。
  std::string AllocatorParamsText() const {
    return allocator_params.empty() ? std::string("{}") : allocator_params;
  }
};

// 统一输入/输出转换回调函数指针类型
using DecodeInputFn = int (*)(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              AlgContext* context, AdapterStatus* status);

using EncodeOutputFn = int (*)(AlgContext* context,
                               const OutputEncodeOptions& options,
                               ExternalOutputBatchView* destination,
                               size_t* written_count, AdapterStatus* status);

/**
 * @brief 输入转换器登记：一种（宿主结构体，业务）的载荷格式
 */
struct InputConverterDefinition {
  std::string type;  // 宿主结构体（map key 后缀）
  std::string name;  // 业务名；kCommonIoName 表示默认处理
  std::optional<int32_t>
      service_type;  // name 对应的取值；common 或结构体没有该成员时为空
  ExternalSlotDefinition slot;
  std::vector<NodePortDefinition> logical_ports;  // 发布的内部逻辑输出端口
  ParameterSet params;                            // 默认没有参数
  DecodeInputFn decode_fn = nullptr;

  std::string Label() const { return type + "/" + name; }
};

/**
 * @brief 输出转换器登记：一种（宿主结构体，业务）的载荷格式
 */
struct OutputConverterDefinition {
  std::string type;
  std::string name;
  std::optional<int32_t> service_type;
  ExternalSlotDefinition slot;
  std::vector<NodePortDefinition> logical_ports;  // 消费的内部逻辑输入端口
  ParameterSet params;
  EncodeOutputFn encode_fn = nullptr;

  std::string Label() const { return type + "/" + name; }
};

}  // namespace llm_edgeflow
