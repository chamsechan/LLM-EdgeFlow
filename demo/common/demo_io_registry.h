#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "demo/common/demo_options.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "nlohmann/json.hpp"
#include "platform_mock/operator_data_types.h"

namespace alg_demo {

/**
 * @brief 一次运行的全部请求。由请求构造填写，由公共流程分批提交。
 */
struct DemoRequestBatch {
  // 每条请求一个 NamedIo，key 为 DemoIoKey(输入项)，即 "<name>.<type>"。
  std::vector<llm_edgeflow::operator_api::NamedIo> requests;
  // 每条请求供结果显示读取的信息（如 rerank
  // 的候选文本）。可选，缺失时降级显示。
  std::vector<nlohmann::json> request_info;
  // 持有载体内存，覆盖整个运行过程。
  std::shared_ptr<void> storage;
};

/**
 * @brief 按输入载体组合构造请求。
 * @param inputs ResolveOperatorConfigIo 返回的输入项，顺序与配置相同。
 * @return 0 成功，4 数据集错误。
 */
using BuildRequestsFn = int (*)(
    const DemoOptions& options,
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
    DemoRequestBatch* out);

/**
 * @brief 按输出结构显示一份结果，并填写 results.jsonl 中 output 的字段。
 * @param output 宿主输出结构指针，生命周期只覆盖本次调用。
 * @param request_info 对应请求的可选信息。
 */
using ShowResultFn = void (*)(const void* output,
                              const nlohmann::json& request_info,
                              uint64_t* request_id, int32_t* status,
                              nlohmann::json* sample_output);

struct DemoInputTag {};
struct DemoOutputTag {};

/**
 * @brief 载体注册表：键是宿主结构名，值是对应的 Demo 回调。
 */
template <typename Fn, typename Tag>
class DemoCarrierRegistry {
 public:
  static DemoCarrierRegistry& Instance();

  /**
   * @brief 注册载体回调
   * @return false 表示键为空、回调为空或重复注册。
   */
  bool Register(std::string carrier, Fn fn);

  /** @brief 查找回调；未注册时返回 nullptr。 */
  Fn Find(std::string_view carrier) const;

  /** @brief 已注册的载体键，按字典序。 */
  std::vector<std::string> List() const;

  bool HasConflict() const;

  /** @brief 清空注册表 (仅用于单元测试重置)。 */
  void ResetForTesting();

  /** @brief 取出全部登记 (仅用于单元测试还原)。 */
  std::map<std::string, Fn> Snapshot() const;

 private:
  DemoCarrierRegistry() = default;
  mutable std::mutex mutex_;
  std::map<std::string, Fn, std::less<>> entries_;
  bool has_conflict_ = false;
};

/** @brief 请求构造注册表，键为 "结构名[,结构名...]"，按 io.input 顺序。 */
using DemoInputRegistry = DemoCarrierRegistry<BuildRequestsFn, DemoInputTag>;
/** @brief 结果显示注册表，键为单个输出结构名。 */
using DemoOutputRegistry = DemoCarrierRegistry<ShowResultFn, DemoOutputTag>;

/**
 * @brief 输入载体组合的注册键：各输入项的结构名按顺序用逗号拼接。
 */
std::string DemoInputCarrierKey(
    const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs);

/**
 * @brief Demo 提交与读取某一项时使用的 map key，即 "<name>.<type>"。
 */
std::string DemoIoKey(const llm_edgeflow::operator_api::OperatorIoEntry& entry);

/**
 * @brief 宿主结构体 service_type 成员的取值：取自 ResolveOperatorConfigIo
 * 返回的 输入项；该项没有取值（common 或结构体没有该成员）时为 0。
 */
inline int32_t DemoServiceType(
    const llm_edgeflow::operator_api::OperatorIoEntry& entry) {
  return entry.service_type.value_or(0);
}

/** @brief 借用 std::string 的字节，生命周期由调用方保证。 */
inline CompanyString BorrowCompanyString(const std::string& text) {
  return {static_cast<int32_t>(text.size()), const_cast<char*>(text.data())};
}

/** @brief 把宿主字符串复制成 std::string；空指针得到空串。 */
inline std::string CopyCompanyString(const CompanyString* text) {
  if (!text || !text->data || text->length <= 0) return {};
  return std::string(text->data, static_cast<size_t>(text->length));
}

class DemoInputRegistrar {
 public:
  DemoInputRegistrar(const char* external_types, BuildRequestsFn fn) {
    DemoInputRegistry::Instance().Register(external_types, fn);
  }
};

class DemoOutputRegistrar {
 public:
  DemoOutputRegistrar(const char* type_name, ShowResultFn fn) {
    DemoOutputRegistry::Instance().Register(type_name, fn);
  }
};

#define ALG_DEMO_CONCAT_INNER(a, b) a##b
#define ALG_DEMO_CONCAT(a, b) ALG_DEMO_CONCAT_INNER(a, b)

// 例：REGISTER_DEMO_INPUT("CompanyOperatorDocInput", BuildDocRequests)；
// 多项输入用逗号拼接结构名：("CompanyFrame,CompanyString", ...)。
#define REGISTER_DEMO_INPUT(external_type, build_fn)           \
  static const ::alg_demo::DemoInputRegistrar ALG_DEMO_CONCAT( \
      g_demo_input_registrar_, __LINE__)(external_type, build_fn)

// 例：REGISTER_DEMO_OUTPUT("CompanyOperatorDocOutput", ShowDocResult)
#define REGISTER_DEMO_OUTPUT(type_name, show_fn)                \
  static const ::alg_demo::DemoOutputRegistrar ALG_DEMO_CONCAT( \
      g_demo_output_registrar_, __LINE__)(type_name, show_fn)

}  // namespace alg_demo
