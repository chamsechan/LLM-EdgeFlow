#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "core/alg_context.h"
#include "core/session_context.h"

namespace llm_edgeflow {

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
   * @brief 节点初始化，配置、Session 与已校验端口绑定均由上下文提供。
   */
  virtual bool Init(const NodeInitContext& init_ctx) = 0;

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

}  // namespace llm_edgeflow
