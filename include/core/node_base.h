#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "core/alg_context.h"
#include "core/session_context.h"

namespace alg_framework {

struct ValidatedNodePlan;

/**
 * @brief 运行时动态控制状态
 */
enum class NodeControlStatus {
  kUnsupported = 0,
  kHandled = 1,
  kFailed = 2,
};

/**
 * @brief 运行时动态控制执行结果
 */
struct NodeControlResult {
  NodeControlStatus status = NodeControlStatus::kUnsupported;
  int code = 0;
  std::string message;

  static NodeControlResult Unsupported() {
    return {NodeControlStatus::kUnsupported, 0, ""};
  }
  static NodeControlResult Handled(int code = 0, std::string message = "") {
    return {NodeControlStatus::kHandled, code, std::move(message)};
  }
  static NodeControlResult Failed(int code, std::string message) {
    return {NodeControlStatus::kFailed, code, std::move(message)};
  }
};

/**
 * @brief 节点初始化上下文 (包含已通过静态校验的端口绑定与 SessionContext)
 */
struct NodeInitContext {
  const ValidatedNodePlan* plan = nullptr;
  const nlohmann::json* config = nullptr;
  SessionContext* session_ctx = nullptr;
};

/**
 * @brief 算法管线算子节点纯虚基类
 */
class INode {
 public:
  virtual ~INode() = default;

  /**
   * @brief 节点初始化 (新标准签名，接收完整上下文)
   */
  virtual bool Init(const NodeInitContext& init_ctx) {
    if (!init_ctx.session_ctx) return false;
    static const nlohmann::json empty_cfg = nlohmann::json::object();
    const nlohmann::json& cfg = init_ctx.config ? *init_ctx.config : empty_cfg;
    return Init(cfg, init_ctx.session_ctx);
  }

  /**
   * @brief 兼容旧式初始化的辅助重载
   */
  virtual bool Init(const nlohmann::json& node_config,
                    SessionContext* session_ctx) {
    (void)node_config;
    (void)session_ctx;
    return true;
  }

  /**
   * @brief 节点执行计算
   * @param req_ctx 单次请求级黑板上下文
   * @return 0 成功, 非 0 错误码
   */
  virtual int Process(AlgContext* req_ctx) = 0;

  /**
   * @brief 运行时动态控制通知
   */
  virtual NodeControlResult Control(int cmd, const std::string& json_param) {
    (void)cmd;
    (void)json_param;
    return NodeControlResult::Unsupported();
  }

  /**
   * @brief 获取节点名称
   */
  virtual const std::string& Name() const = 0;
};

}  // namespace alg_framework
