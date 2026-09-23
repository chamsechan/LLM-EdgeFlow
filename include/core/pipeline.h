#pragma once

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "core/alg_context.h"
#include "core/node_interface.h"
#include "core/pipeline_diagnostic.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"

namespace llm_edgeflow {

class ThreadPool;

/**
 * @brief 算法管线调度核心引擎 (Pipeline)
 *
 * 支持显式 DAG (有向无环图) 依赖声明 (`depends_on`)、
 * 消费 PipelineValidator 生成的已验证拓扑计划，不重复解析或排序。
 * 支持配置驱动的【顺序调度 (Sequential)】与【多分支异步波前并发调度
 * (Parallel)】。
 */
class Pipeline {
 public:
  enum class ExecutionMode { kSequential, kParallel };

  /**
   * @brief Pipeline 实例状态机 (R1-ACC-002 一次性构建与就绪保护)
   */
  enum class State {
    kEmpty = 0,  ///< 新建空实例，允许发起且仅允许发起一次构建
    kBuilding,  ///< 正在执行构建（计划预检、模型与节点物化）
    kReady,     ///< 构建成功，允许执行 Execute 和 Control
    kFailed,    ///< 构建失败，不可再次构建或执行
  };

  Pipeline();
  ~Pipeline();

  /**
   * @brief 从已验证的管线计划构建整条管线 (接收所有权，避免重复验证与物化)
   */
  bool BuildFromPlan(std::unique_ptr<ValidatedPipelinePlan> plan,
                     PipelineDiagnostic* diagnostic = nullptr);

  /**
   * @brief 按照拓扑排序/波前序列执行单次批次管线推理
   */
  int Execute(AlgContext* req_ctx);

  /**
   * @brief 运行时动态控制
   */
  // Calls must be externally serialized with Execute/Control. Broadcast updates
  // are not transactional; error identifies failed instances. A JSON envelope
  // {"$edgeflow_control":1,"node_id":"id","payload":{...}} targets one
  // instance.
  int Control(int cmd, const std::string& json_param,
              std::string* error = nullptr);

  State GetState() const { return state_; }
  bool IsReady() const { return state_ == State::kReady; }

  SessionContext& GetSessionContext() { return *session_ctx_; }
  const SessionContext& GetSessionContext() const { return *session_ctx_; }
  const std::string& GetBizName() const { return plan_->config.biz_name; }
  ExecutionMode GetExecutionMode() const { return execution_mode_; }
  const std::vector<std::string>& GetTopologicalOrder() const {
    return plan_->report.topological_order;
  }
  const std::vector<std::vector<std::string>>& GetTopologicalLayers() const {
    return plan_->report.topological_layers;
  }
  const ValidatedPipelinePlan& GetPlan() const { return *plan_; }

 private:
  struct NodeExecutionResult {
    int code = 0;
    std::string message;
  };
  static NodeExecutionResult ExecuteNodeSafely(INode* node, AlgContext* req_ctx,
                                               std::string_view node_id);

  friend class PipelineConfigTest;
  std::function<void()> test_internal_hook_;

  State state_ = State::kEmpty;
  ExecutionMode execution_mode_ = ExecutionMode::kSequential;
  // Heap ownership keeps addresses handed to initialized Nodes stable while a
  // fully staged runtime assembly is committed into this façade.
  std::unique_ptr<SessionContext> session_ctx_;
  std::unique_ptr<ValidatedPipelinePlan> plan_;

  std::vector<std::unique_ptr<INode>> nodes_;
  std::vector<std::vector<INode*>> node_layers_;

  std::unique_ptr<ThreadPool> thread_pool_;
};

}  // namespace llm_edgeflow
