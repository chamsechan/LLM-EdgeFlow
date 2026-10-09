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
  bool required = true;
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
inline constexpr size_t kMaxProcessBatchSize = 64;
inline constexpr char kCommonIoName[] = "common";

struct InputDecodeOptions {
  std::string type;
  std::string name;
  std::vector<uint64_t>* request_ids = nullptr;
  const ParameterValues* params = nullptr;

  std::string Label() const { return type + "/" + name; }
  template <typename P>
  const P& Params() const {
    if (!params) throw std::logic_error("Missing parsed input parameters");
    return params->Get<P>();
  }
};

struct OutputEncodeOptions {
  std::string type;
  std::string name;
  const std::vector<uint64_t>* request_ids = nullptr;
  const ParameterValues* params = nullptr;

  std::string Label() const { return type + "/" + name; }
  template <typename P>
  const P& Params() const {
    if (!params) throw std::logic_error("Missing parsed output parameters");
    return params->Get<P>();
  }
};

struct ExternalSlotDefinition {
  std::string type_id;
  std::string type_suffix;
  bool required = true;
  std::string allocator;
  std::string allocator_params;
  uint32_t metadata_count = 0;
  int32_t metadata_type_id = 0;
};

using DecodeInputFn = int (*)(const ExternalInputBatchView& source,
                              const InputDecodeOptions& options,
                              AlgContext* context, AdapterStatus* status);
using EncodeOutputFn = int (*)(AlgContext* context,
                               const OutputEncodeOptions& options,
                               ExternalOutputBatchView* destination,
                               size_t* written_count, AdapterStatus* status);

struct InputConverterDefinition {
  std::string type;
  std::string name;
  std::optional<int32_t> service_type;
  ExternalSlotDefinition slot;
  std::vector<NodePortDefinition> logical_ports;
  ParameterSet params;
  DecodeInputFn decode_fn = nullptr;
  std::string Label() const { return type + "/" + name; }
};

struct OutputConverterDefinition {
  std::string type;
  std::string name;
  std::optional<int32_t> service_type;
  ExternalSlotDefinition slot;
  std::vector<NodePortDefinition> logical_ports;
  ParameterSet params;
  EncodeOutputFn encode_fn = nullptr;
  std::string Label() const { return type + "/" + name; }
};

}  // namespace llm_edgeflow
