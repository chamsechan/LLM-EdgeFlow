#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

// 终端 ANSI 颜色定义
#define COLOR_CYAN "\033[96m"
#define COLOR_BLUE "\033[94m"
#define COLOR_GREEN "\033[92m"
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
            << "╔══════════════════════════════════════════════════════════════"
               "════════════════════╗\n"
            << "║  Alg-SDK Pipeline & DAG Visualizer (Embedded Native C++ "
               "Standalone Edition)     ║\n"
            << "║  ConfigFile: " << std::left << std::setw(30) << cfg_path
            << " BizName: " << std::setw(26) << biz_name << " ║\n"
            << "╚══════════════════════════════════════════════════════════════"
               "════════════════════╝"
            << COLOR_RESET << "\n\n";

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

  // 2. DAG 拓扑流图
  std::cout << COLOR_BOLD
            << "[ 2. 边缘端数据流向与 DAG 拓扑流图 (Data Flow & Nodes) ]"
            << COLOR_RESET << "\n\n";
  std::cout << "   " << COLOR_GREEN << "[外部请求输入: vector<void*> inputs]"
            << COLOR_RESET << "\n";
  std::cout << "         │\n";
  std::cout << "         │  " << COLOR_DIM << "📥 解包原始结构体注入 AlgContext"
            << COLOR_RESET << "\n";
  std::cout << "         ▼\n";

  for (size_t i = 0; i < pipeline.size(); ++i) {
    const auto& node = pipeline[i];
    std::string ntype = node.value("node_type", "UnknownNode");
    std::string bind_m = node.value("bind_model", "");
    if (bind_m.empty() && node.contains("config")) {
      bind_m = node["config"].value("bind_model", "");
    }

    std::string card_color = bind_m.empty() ? COLOR_BLUE : COLOR_MAGENTA;

    std::cout << "   " << card_color << "┌─ #" << (i < 9 ? "0" : "") << (i + 1)
              << " " << ntype << " ──────────────────────────────────────────┐"
              << COLOR_RESET << "\n";

    if (!bind_m.empty()) {
      std::cout << "   " << card_color << "│" << COLOR_RESET << "  "
                << COLOR_MAGENTA << "🧠 绑定硬件模型: " << bind_m
                << " (SessionContext 依赖注入)" << COLOR_RESET << "\n";
    } else {
      std::cout << "   " << card_color << "│" << COLOR_RESET << "  "
                << COLOR_CYAN << "⚡ 算子属性: 纯 CPU / 内存私有状态规则逻辑"
                << COLOR_RESET << "\n";
    }

    if (node.contains("config") && !node["config"].empty()) {
      std::cout << "   " << card_color << "│" << COLOR_RESET << "  "
                << COLOR_YELLOW << "⚙️ 私有参数: " << node["config"].dump()
                << COLOR_RESET << "\n";
    }

    std::cout
        << "   " << card_color
        << "└─────────────────────────────────────────────────────────────┘"
        << COLOR_RESET << "\n";

    if (i < pipeline.size() - 1) {
      std::cout << "         │\n";
      std::cout << "         │  " << COLOR_DIM
                << "📦 传递 TraceableItem 样本溯源数据" << COLOR_RESET << "\n";
      std::cout << "         ▼\n";
    } else {
      std::cout << "         │\n";
      std::cout << "         │  " << COLOR_DIM
                << "📤 打包提取特征回写外部结构体" << COLOR_RESET << "\n";
      std::cout << "         ▼\n";
      std::cout
          << "   " << COLOR_GREEN
          << "[外部响应输出: vector<void*> outputs (状态: SUCCESS 200 OK)]"
          << COLOR_RESET << "\n\n";
    }
  }

  return 0;
}
