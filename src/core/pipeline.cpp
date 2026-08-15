#include "core/pipeline.h"

#include <fstream>
#include <iostream>

#include "core/node_registry.h"
#include "engine/engine_registry.h"

namespace alg_framework {

Pipeline::Pipeline() : business_name_("default_biz") {}

bool Pipeline::BuildFromConfigFile(const std::string& config_file_path) {
  std::ifstream ifs(config_file_path);
  if (!ifs.is_open()) {
    std::cerr << "[Pipeline] Failed to open config file: " << config_file_path
              << std::endl;
    return false;
  }

  try {
    nlohmann::json root_json;
    ifs >> root_json;
    return BuildFromJson(root_json);
  } catch (const std::exception& e) {
    std::cerr << "[Pipeline] JSON parse exception in " << config_file_path
              << ": " << e.what() << std::endl;
    return false;
  }
}

bool Pipeline::BuildFromJson(const nlohmann::json& root_config) {
  business_name_ = root_config.value("business_name", "unnamed_biz");

  // 1. 加载配置中声明的所有模型 (支持多模型加载到 ModelManager)
  if (root_config.contains("models") && root_config["models"].is_array()) {
    for (const auto& model_cfg : root_config["models"]) {
      std::string model_id = model_cfg.value("model_id", "");
      std::string engine_type = model_cfg.value("engine_type", "");
      std::string model_path = model_cfg.value("model_path", "");
      nlohmann::json custom_cfg =
          model_cfg.value("config", nlohmann::json::object());

      if (model_id.empty() || engine_type.empty()) {
        std::cerr << "[Pipeline] Invalid model config: model_id or engine_type "
                     "is empty"
                  << std::endl;
        return false;
      }

      auto engine = EngineFactory::Instance().Create(engine_type);
      if (!engine) {
        std::cerr << "[Pipeline] Unknown engine_type: " << engine_type
                  << " for model: " << model_id << std::endl;
        return false;
      }

      if (!engine->Load(model_path, custom_cfg)) {
        std::cerr << "[Pipeline] Failed to load model: " << model_id
                  << " at path: " << model_path << std::endl;
        return false;
      }

      session_ctx_.GetModelManager().RegisterModel(
          model_id, std::shared_ptr<IModelEngine>(std::move(engine)));
      std::cout << "[Pipeline] Successfully loaded model [" << model_id
                << "] with engine [" << engine_type << "]" << std::endl;
    }
  }

  // 2. 组装流水线各个算子节点
  if (!root_config.contains("pipeline") ||
      !root_config["pipeline"].is_array()) {
    std::cerr << "[Pipeline] Missing 'pipeline' array in configuration"
              << std::endl;
    return false;
  }

  nodes_.clear();
  for (const auto& node_cfg : root_config["pipeline"]) {
    std::string node_type = node_cfg.value("node_type", "");
    nlohmann::json custom_cfg =
        node_cfg.value("config", nlohmann::json::object());

    if (node_type.empty()) {
      std::cerr << "[Pipeline] node_type is empty in pipeline config"
                << std::endl;
      return false;
    }

    auto node = NodeFactory::Instance().Create(node_type);
    if (!node) {
      std::cerr << "[Pipeline] Unregistered node_type: " << node_type
                << std::endl;
      return false;
    }

    // 初始化节点 (传入私有 config 以及 session_ctx)
    if (!node->Init(custom_cfg, &session_ctx_)) {
      std::cerr << "[Pipeline] Node Init failed: " << node_type << std::endl;
      return false;
    }

    nodes_.push_back(std::move(node));
    std::cout << "[Pipeline] Initialized node [" << node_type << "]"
              << std::endl;
  }

  return true;
}

int Pipeline::Execute(AlgContext* req_ctx) {
  if (!req_ctx) return -1;

  for (size_t i = 0; i < nodes_.size(); ++i) {
    int ret = nodes_[i]->Process(req_ctx);
    if (ret != 0) {
      std::cerr << "[Pipeline] Node [" << nodes_[i]->Name()
                << "] failed with error code: " << ret
                << ", msg: " << req_ctx->GetErrorMessage() << std::endl;
      return ret;
    }
  }

  return 0;
}

int Pipeline::Control(int cmd, const std::string& json_param) {
  std::cout << "[Pipeline] Control cmd received: " << cmd
            << ", params: " << json_param << std::endl;
  int last_ret = 0;
  for (auto& node : nodes_) {
    int ret = node->Control(cmd, json_param);
    if (ret != 0) {
      last_ret = ret;
    }
  }
  return last_ret;
}

}  // namespace alg_framework
