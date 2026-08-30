#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "demo/common/demo_options.h"

namespace alg_demo {

using DemoRunFunction = int (*)(const DemoOptions& options);

struct DemoDescriptor {
  std::string biz_name;       // 业务标识名 (如 entity_extract, doc_qa)
  std::string display_title;  // 终端展示标题 (如 "实体/名词提取业务")
  DemoRunFunction run = nullptr;

  DemoDescriptor() = default;
  DemoDescriptor(std::string name, std::string title, DemoRunFunction func)
      : biz_name(std::move(name)), display_title(std::move(title)), run(func) {}
};

class DemoRegistry {
 public:
  static DemoRegistry& Instance();

  /**
   * @brief 注册业务 Demo 描述符
   * @param descriptor 业务描述符 (拒绝空名、空函数或重复注册)
   * @return true 注册成功, false 注册失败 (冲突或非法)
   */
  bool Register(DemoDescriptor descriptor);

  /**
   * @brief 查找业务 Demo
   * @param biz_name 业务名
   * @return 匹配的描述符指针, 若未找到返回 nullptr
   */
  const DemoDescriptor* Find(std::string_view biz_name) const;

  /**
   * @brief 列出所有已注册的业务描述符
   */
  std::vector<DemoDescriptor> ListDescriptors() const;

  /**
   * @brief 列出所有已注册的业务名称列表
   */
  std::vector<std::string> ListBizNames() const;

  /**
   * @brief 是否发生过注册冲突
   */
  bool HasConflict() const;

  /**
   * @brief 清空注册表 (仅用于单元测试重置)
   */
  void ResetForTesting();

 private:
  DemoRegistry() = default;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, DemoDescriptor> descriptors_;
  bool has_conflict_ = false;
};

/**
 * @brief 业务 Demo 静态自动注册辅助类
 */
class DemoRegisterHelper {
 public:
  DemoRegisterHelper(const char* name, const char* title,
                     DemoRunFunction func) {
    DemoRegistry::Instance().Register({name, title, func});
  }
};

#define REGISTER_DEMO_BIZ(biz_name, title, run_func)                           \
  static ::alg_demo::DemoRegisterHelper g_demo_reg_##run_func(biz_name, title, \
                                                              run_func);

}  // namespace alg_demo
