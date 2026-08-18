#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/node_base.h"
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

  struct DagNodeMeta {
    std::string id;
    std::string node_type;
    std::vector<std::string> depends_on;
    nlohmann::json custom_config;
  };

  Pipeline();
  ~Pipeline() = default;

  /**
   * @brief 从 JSON 配置文件构建整条管线 (包含多模型加载、DAG
   * 拓扑分层与执行器组装)
   */
  bool BuildFromConfigFile(const std::string& config_file_path);
  bool BuildFromJson(const nlohmann::json& root_config);

  /**
   * @brief 按照拓扑排序/波前序列执行单次批次管线推理
   */
  int Execute(AlgContext* req_ctx);

  /**
   * @brief 运行时动态控制
   */
  int Control(int cmd, const std::string& json_param);

  SessionContext& GetSessionContext() { return session_ctx_; }
  const std::string& GetBusinessName() const { return business_name_; }
  ExecutionMode GetExecutionMode() const { return execution_mode_; }
  const std::vector<std::string>& GetTopologicalOrder() const {
    return topological_order_;
  }
  const std::vector<std::vector<std::string>>& GetTopologicalLayers() const {
    return topological_layers_ids_;
  }

 private:
  bool ResolveDagTopologicalSort(
      const std::vector<DagNodeMeta>& raw_nodes,
      std::vector<std::vector<DagNodeMeta>>* sorted_layers);

  std::string business_name_;
  ExecutionMode execution_mode_ = ExecutionMode::SEQUENTIAL;
  size_t max_parallel_workers_ = 4;
  SessionContext session_ctx_;

  std::vector<std::unique_ptr<INode>> nodes_;
  std::vector<std::vector<INode*>> node_layers_;
  std::vector<std::string> topological_order_;
  std::vector<std::vector<std::string>> topological_layers_ids_;

  std::unique_ptr<ThreadPool> thread_pool_;
};

}  // namespace alg_framework
