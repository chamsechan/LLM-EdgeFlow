#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "core/alg_context.h"
#include "core/session_context.h"

namespace alg_framework {

/**
 * @brief 算法管线算子节点纯虚基类
 *
 * 开发者编写业务时：
 * 1. 继承 INode
 * 2. 在类内定义私有业务数据（词典、规则表、正则、成员变量）
 * 3. 实现 Init()（读取私有配置、从 SessionContext 绑定模型）
 * 4. 实现 Process()（处理 RequestContext 中的瞬态数据）
 * 5. 使用 REGISTER_NODE 注册
 */
class INode {
 public:
  virtual ~INode() = default;

  /**
   * @brief 节点初始化
   * @param node_config 当前节点的私有 JSON 配置
   * @param session_ctx 句柄会话上下文 (可从中获取句柄持有的各个 ModelEngine)
   * @return true 成功, false 失败
   */
  virtual bool Init(const nlohmann::json& node_config,
                    SessionContext* session_ctx) = 0;

  /**
   * @brief 节点执行计算
   * @param req_ctx 单次请求级黑板上下文
   * @return 0 成功, 非 0 错误码
   */
  virtual int Process(AlgContext* req_ctx) = 0;

  /**
   * @brief 运行时动态控制通知 (如动态下发/更新词表、切换模式等)
   */
  virtual int Control(int cmd, const std::string& json_param) {
    (void)cmd;
    (void)json_param;
    return 0;
  }

  /**
   * @brief 获取节点名称
   */
  virtual const std::string& Name() const = 0;
};

}  // namespace alg_framework
