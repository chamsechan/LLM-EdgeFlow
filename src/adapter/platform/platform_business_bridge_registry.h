#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/platform/platform_value_type_registry.h"
#include "company_alg_interface.h"

namespace alg_framework {

enum class IoDirection { kInput, kOutput };

/**
 * @brief Process 执行期局部影子 DTO 存储器
 * (保证执行期间指针生命周期与地址绝对稳定)
 */
struct ProcessLocalShadowStorage {
  std::deque<std::string> strings;
  std::deque<std::vector<float>> float_vectors;
  std::vector<std::shared_ptr<void>> shadow_dtos;

  const char* StoreString(const CompanyString* cs) {
    if (!cs || cs->length <= 0 || !cs->data) {
      strings.emplace_back("");
      return strings.back().c_str();
    }
    strings.emplace_back(cs->data, cs->length);
    return strings.back().c_str();
  }

  const char* StoreOptionalString(const CompanyString* cs) {
    if (!cs) return nullptr;
    if (cs->length <= 0 || !cs->data) {
      strings.emplace_back("");
      return strings.back().c_str();
    }
    strings.emplace_back(cs->data, cs->length);
    return strings.back().c_str();
  }

  template <typename T>
  T* AllocateShadowDto() {
    auto dto = std::make_shared<T>();
    T* raw = dto.get();
    shadow_dtos.push_back(std::move(dto));
    return raw;
  }
};

/**
 * @brief 业务逻辑槽位定义
 */
struct PlatformBusinessSlot {
  std::string logical_name;  // 业务逻辑槽位名 (业务与方向内唯一)
  std::string type_suffix;   // 规范类型后缀
  IoDirection direction = IoDirection::kInput;
  bool required = true;
};

using ConvertSampleInputFn = std::function<int(
    const std::unordered_map<std::string, const void*>& slots_by_logical_name,
    ProcessLocalShadowStorage& storage, const void** out_internal_dto,
    std::string* err)>;

using ConvertSampleOutputFn =
    std::function<int(const void* internal_dto, void* external_output_struct,
                      const ResolvedOutputPoolSpec& spec, std::string* err)>;

using CreateShadowOutputDtoFn =
    std::function<void*(ProcessLocalShadowStorage& storage)>;

/**
 * @brief 业务桥接描述符
 */
struct PlatformBusinessBridgeDescriptor {
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  std::string biz_name;
  std::vector<PlatformBusinessSlot> input_slots;
  std::vector<PlatformBusinessSlot> output_slots;
  ConvertSampleInputFn convert_sample_input;
  ConvertSampleOutputFn convert_sample_output;
  CreateShadowOutputDtoFn create_shadow_output_dto;
};

/**
 * @brief 平台业务桥接注册表 (SSOT)
 */
class PlatformBusinessBridgeRegistry {
 public:
  static PlatformBusinessBridgeRegistry& Instance();

  /**
   * @brief 注册业务桥接描述符
   */
  bool RegisterBridge(PlatformBusinessBridgeDescriptor desc);

  /**
   * @brief 获取业务桥接描述符
   */
  const PlatformBusinessBridgeDescriptor* GetBridge(
      CompanyAlgBizType biz_type) const;

  /**
   * @brief 全局初始化与一致性原子审计 (返回 -6 若存在任何冲突或缺漏)
   */
  int GlobalInit();

  /**
   * @brief 检查是否存在冲突
   */
  bool HasConflict() const { return has_conflict_; }

  /**
   * @brief 辅助函数：将 C 字符串安全复制至池化 CompanyString
   */
  static int CopyToPooledString(const char* src, CompanyString* dest,
                                uint32_t capacity, const char* field_name,
                                std::string* err) noexcept;

 private:
  PlatformBusinessBridgeRegistry();
  void RegisterBuiltinBridges();

  bool has_conflict_ = false;
  bool audited_ = false;
  std::unordered_map<int32_t, PlatformBusinessBridgeDescriptor>
      bridges_by_biz_type_;
};

#define REGISTER_PLATFORM_BUSINESS_BRIDGE(BridgeRegisterFn)             \
  namespace {                                                           \
  struct AutoRegister_##BridgeRegisterFn {                              \
    AutoRegister_##BridgeRegisterFn() {                                 \
      BridgeRegisterFn(                                                 \
          ::alg_framework::PlatformBusinessBridgeRegistry::Instance()); \
    }                                                                   \
  } g_auto_reg_##BridgeRegisterFn;                                      \
  }

}  // namespace alg_framework
