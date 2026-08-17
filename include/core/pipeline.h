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
 */
class Pipeline {
 public:
  Pipeline();
  ~Pipeline() = default;

  /**
   * @brief 从 JSON 配置文件构建整条管线 (包含多模型加载与多算子节点组装)
   */
  bool BuildFromConfigFile(const std::string& config_file_path);
  bool BuildFromJson(const nlohmann::json& root_config);

  /**
   * @brief 执行单次批次管线推理
   */
  int Execute(AlgContext* req_ctx);

  /**
   * @brief 运行时动态控制
   */
  int Control(int cmd, const std::string& json_param);

  SessionContext& GetSessionContext() { return session_ctx_; }
  const std::string& GetBusinessName() const { return business_name_; }

 private:
  std::string business_name_;
  SessionContext session_ctx_;
  std::vector<std::unique_ptr<INode>> nodes_;
};

}  // namespace alg_framework
