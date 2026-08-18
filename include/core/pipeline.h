#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/node_base.h"
#include "core/session_context.h"

namespace alg_framework {

/**
 * @brief 算法管线调度核心引擎 (Pipeline)
 *
 * 支持显式 DAG (有向无环图) 依赖声明 (`depends_on`)、
 * Kahn 算法拓扑排序与成环死锁检测，同时向下兼容线性自然顺序管线。
 */
class Pipeline {
 public:
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
   * 拓扑排序与多算子组装)
   */
  bool BuildFromConfigFile(const std::string& config_file_path);
  bool BuildFromJson(const nlohmann::json& root_config);

  /**
   * @brief 按照拓扑排序序列执行单次批次管线推理
   */
  int Execute(AlgContext* req_ctx);

  /**
   * @brief 运行时动态控制
   */
  int Control(int cmd, const std::string& json_param);

  SessionContext& GetSessionContext() { return session_ctx_; }
  const std::string& GetBusinessName() const { return business_name_; }
  const std::vector<std::string>& GetTopologicalOrder() const {
    return topological_order_;
  }

 private:
  bool ResolveDagTopologicalSort(const std::vector<DagNodeMeta>& raw_nodes,
                                 std::vector<DagNodeMeta>* sorted_nodes);

  std::string business_name_;
  SessionContext session_ctx_;
  std::vector<std::unique_ptr<INode>> nodes_;
  std::vector<std::string> topological_order_;
};

}  // namespace alg_framework
