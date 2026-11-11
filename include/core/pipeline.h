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
 * @brief Control 失败发生的阶段，供接入适配层映射公开返回码
 */
enum class ControlFailureStage {
  kNone,  ///< 成功，或 Pipeline 未就绪
  kRequest,  ///< 信封、目标节点或载荷 schema 被拒绝，未更新任何节点
  kUnsupported,  ///< 没有节点声明或处理该命令
  kNode,         ///< 节点拒绝或未能应用更新
};

/**
 * @brief 算法管线调度核心引擎
 *
 * 按数据依赖和额外顺序约束（`depends_on`）构成的 DAG 执行，直接消费
 * PipelineValidator 生成的已验证拓扑计划，不重复解析或排序。
 * 由配置选择顺序调度（Sequential）或多分支波前并发调度（Parallel）。
 */
class Pipeline {
 public:
  enum class ExecutionMode { kSequential, kParallel };

  /**
   * @brief Pipeline 实例状态机：一次性构建与就绪保护
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
   * @brief 从已验证的管线计划构建整条管线（接收所有权，避免重复验证与物化）
   * 仅允许对空实例尝试一次。模型、Node 和执行器全部就绪后才提交运行时；
   * 本次构建失败使实例进入 kFailed，不能重试，诊断由可选输出参数接收。
   */
  bool BuildFromPlan(std::unique_ptr<ValidatedPipelinePlan> plan,
                     PipelineDiagnostic* diagnostic = nullptr);

  /**
   * @brief 按照拓扑排序/波前序列执行单次批次管线推理
   * req_ctx 须非空且实例已就绪。层内已提交任务全部结束后才退出本次调用；
   * 失败时停止执行后续层并记录诊断，已发布的请求数据不回滚。
   */
  int Execute(AlgContext* req_ctx);

  /**
   * @brief 按命令声明向目标实例或全部匹配实例分发运行时控制
   * 所有目标的载荷 schema 预检通过后才开始更新；failure_stage 区分请求拒绝、
   * 不支持命令及 Node 执行失败，供接入适配层映射错误。
   * 调用须与 Execute/Control 在外部串行化。广播更新不具事务性，error 会指出
   * 失败的实例。以下 JSON 信封只作用于一个实例：
   * {"$edgeflow_control":1,"node":"name","payload":{...}}
   */
  int Control(int cmd, const std::string& json_param,
              std::string* error = nullptr,
              ControlFailureStage* failure_stage = nullptr);

  State GetState() const { return state_; }
  bool IsReady() const { return state_ == State::kReady; }

  SessionContext& GetSessionContext() { return *session_ctx_; }
  const SessionContext& GetSessionContext() const { return *session_ctx_; }
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
  /**
   * @brief 隔离线程诊断，将执行错误关联到 Pipeline 中的 Node 实例名。
   * 返回本次调用的结果，避免读取其他并行任务写入的最终错误状态。
   */
  static NodeExecutionResult ExecuteNodeSafely(INode* node, AlgContext* req_ctx,
                                               std::string_view node_name);

  State state_ = State::kEmpty;
  ExecutionMode execution_mode_ = ExecutionMode::kSequential;
  // 使用堆所有权，确保完整暂存的运行时组件提交到本门面时，
  // 已交给已初始化 Node 的地址保持不变。
  std::unique_ptr<SessionContext> session_ctx_;
  std::unique_ptr<ValidatedPipelinePlan> plan_;

  std::vector<std::unique_ptr<INode>> nodes_;
  std::vector<std::vector<INode*>> node_layers_;

  std::unique_ptr<ThreadPool> thread_pool_;
};

}  // namespace llm_edgeflow
