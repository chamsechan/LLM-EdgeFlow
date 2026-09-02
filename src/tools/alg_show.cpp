#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

// 终端 ANSI 颜色定义
#define COLOR_CYAN "\033[96m"
#define COLOR_BLUE "\033[94m"
#define COLOR_YELLOW "\033[93m"
#define COLOR_MAGENTA "\033[95m"
#define COLOR_RED "\033[91m"
#define COLOR_BOLD "\033[1m"
#define COLOR_DIM "\033[2m"
#define COLOR_RESET "\033[0m"

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cout
        << "用法 (纯 C++ 零依赖原生版本，适用于任意嵌入式/边缘 Linux 系统):"
        << std::endl;
    std::cout << "  ./build/alg_show <path_to_config.json>" << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  ./build/alg_show configs/pipeline_doc_qa.json" << std::endl;
    std::cout << "  ./build/alg_show configs/pipeline_dialogue_audit.json"
              << std::endl;
    return 0;
  }

  std::string cfg_path = argv[1];
  std::ifstream f(cfg_path);
  if (!f.is_open()) {
    // 尝试相对路径
    f.open("../" + cfg_path);
    if (!f.is_open()) {
      std::cerr << COLOR_RED << "[Error] 无法打开配置文件: " << cfg_path
                << COLOR_RESET << std::endl;
      return 1;
    }
  }

  nlohmann::json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    std::cerr << COLOR_RED << "[Error] JSON 解析失败: " << e.what()
              << COLOR_RESET << std::endl;
    return 2;
  }

  std::string biz_name = j.value("biz_name", "unnamed_biz");
  auto models = j.value("models", nlohmann::json::array());
  auto pipeline = j.value("pipeline", nlohmann::json::array());

  std::cout << "\n"
            << COLOR_BOLD << COLOR_CYAN
            << "LLM-EdgeFlow Declared Pipeline Viewer (Native Standalone)"
            << COLOR_RESET << "\n"
            << "ConfigFile: " << cfg_path << "\n"
            << "BizName: " << biz_name << "\n\n";

  // 1. 模型资源池
  std::cout << COLOR_BOLD << "[ 1. 边缘设备挂载模型池 (ModelManager) ]"
            << COLOR_RESET << "\n";
  if (models.empty()) {
    std::cout << "  " << COLOR_DIM
              << "└── (纯规则业务，无需加载任何模型，零显存/内存开销)"
              << COLOR_RESET << "\n\n";
  } else {
    for (size_t i = 0; i < models.size(); ++i) {
      std::string prefix = (i == models.size() - 1) ? "  └──" : "  ├──";
      std::string mid = models[i].value("model_id", "unknown");
      std::string model_type = models[i].value("model_type", "unknown");
      std::string backend = models[i].value("backend", "unknown");
      std::string mpath = models[i].value("model_path", "");
      size_t pos = mpath.find_last_of("/\\");
      if (pos != std::string::npos) mpath = mpath.substr(pos + 1);

      int max_b = 1;
      if (models[i].contains("model_config")) {
        max_b = models[i]["model_config"].value("max_batch_size", 1);
      }

      std::cout << prefix << " " << COLOR_MAGENTA << "🧠 " << mid << COLOR_RESET
                << " (" << COLOR_CYAN << "Model: " << model_type
                << ", Backend: " << backend << COLOR_RESET << ", "
                << COLOR_YELLOW << "FixedMaxBatch: " << max_b << COLOR_RESET
                << ", Path: " << COLOR_DIM << mpath << COLOR_RESET << ")\n";
    }
    std::cout << "\n";
  }

  // 2. 显式 DAG 声明。这里不推导拓扑或重复 PipelineValidator 规则。
  std::cout << COLOR_BOLD
            << "[ 2. 显式 DAG 节点与依赖 (Declared Nodes & Dependencies) ]"
            << COLOR_RESET << "\n";
  std::cout
      << COLOR_DIM
      << "  本工具仅展示 JSON 声明；请使用 alg_pipeline_tool validate/plan "
         "获取校验后的执行计划。"
      << COLOR_RESET << "\n\n";

  for (size_t i = 0; i < pipeline.size(); ++i) {
    const auto& node = pipeline[i];
    std::string node_id = node.value("id", "<missing-id>");
    std::string ntype = node.value("node_type", "UnknownNode");
    std::string depends = node.contains("depends_on")
                              ? node["depends_on"].dump()
                              : "<missing-depends_on>";
    std::string bind_m = node.contains("config") && node["config"].is_object()
                             ? node["config"].value("bind_model", "")
                             : "";

    std::string card_color = bind_m.empty() ? COLOR_BLUE : COLOR_MAGENTA;

    std::cout << "  " << card_color << "[" << i << "] " << node_id
              << COLOR_RESET << "\n"
              << "      node_type: " << ntype << "\n"
              << "      depends_on: " << depends << "\n";

    if (!bind_m.empty()) {
      std::cout << "      bind_model: " << COLOR_MAGENTA << bind_m
                << COLOR_RESET << "\n";
    }

    if (node.contains("config") && !node["config"].empty()) {
      std::cout << "      config: " << COLOR_YELLOW << node["config"].dump()
                << COLOR_RESET << "\n";
    }
    std::cout << "\n";
  }

  return 0;
}
