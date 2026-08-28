#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/pipeline_diagnostic.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "core/thread_pool.h"

namespace alg_framework {

/**
 * @brief 算法管线调度核心引擎 (Pipeline)
 *
 * 支持显式 DAG (有向无环图) 依赖声明 (`depends_on`)、
 * Kahn 算法拓扑分层波前排序与成环死锁检测，
 * 并支持配置驱动的【顺序调度 (Sequential)】与【多分支异步波前并发调度
 * (Parallel)】。
 */
class Pipeline {
 public:
  enum class ExecutionMode { SEQUENTIAL, PARALLEL };

  /**
   * @brief Pipeline 实例状态机 (R1-ACC-002 一次性构建与就绪保护)
   */
  enum class State {
    kEmpty = 0,  ///< 新建空实例，允许发起且仅允许发起一次构建
    kBuilding,  ///< 正在执行构建（解析、预检、物化）
    kReady,     ///< 构建成功，允许执行 Execute 和 Control
    kFailed,    ///< 构建失败，不可再次构建或执行
  };

  Pipeline();
  ~Pipeline() = default;

  /**
   * @brief 从 JSON 配置文件构建整条管线 (包含严格校验、模型加载、DAG
   * 拓扑分层与执行器组装)
   */
  bool BuildFromConfigFile(const std::string& config_file_path,
                           PipelineDiagnostic* diagnostic = nullptr,
                           ValidationPolicy policy = ValidationPolicy::kStrict);
  bool BuildFromJson(const nlohmann::json& root_config,
                     PipelineDiagnostic* diagnostic = nullptr,
                     ValidationPolicy policy = ValidationPolicy::kStrict);

  /**
   * @brief 按照拓扑排序/波前序列执行单次批次管线推理
   */
  int Execute(AlgContext* req_ctx);

  /**
   * @brief 运行时动态控制
   */
  int Control(int cmd, const std::string& json_param);

  State GetState() const { return state_; }
  bool IsReady() const { return state_ == State::kReady; }

  SessionContext& GetSessionContext() { return session_ctx_; }
  const SessionContext& GetSessionContext() const { return session_ctx_; }
  const std::string& GetBizName() const { return biz_name_; }
  const std::string& GetBusinessName() const { return biz_name_; }
  ExecutionMode GetExecutionMode() const { return execution_mode_; }
  const std::vector<std::string>& GetTopologicalOrder() const {
    return topological_order_;
  }
  const std::vector<std::vector<std::string>>& GetTopologicalLayers() const {
    return topological_layers_ids_;
  }
  const ValidatedPipelinePlan& GetPlan() const { return plan_; }

 private:
  bool BuildInternal(const nlohmann::json& root_config,
                     PipelineDiagnostic* diagnostic, ValidationPolicy policy);

  friend class PipelineConfigTest;
  std::function<void()> test_internal_hook_;

  State state_ = State::kEmpty;
  std::string biz_name_;
  std::string business_name_;
  ExecutionMode execution_mode_ = ExecutionMode::SEQUENTIAL;
  size_t max_parallel_workers_ = 4;
  SessionContext session_ctx_;
  ValidatedPipelinePlan plan_;

  std::vector<std::unique_ptr<INode>> nodes_;
  std::vector<std::vector<INode*>> node_layers_;
  std::vector<std::string> topological_order_;
  std::vector<std::vector<std::string>> topological_layers_ids_;

  std::unique_ptr<ThreadPool> thread_pool_;
};

}  // namespace alg_framework
