#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "engine/model_registry.h"
#include "nlohmann/json.hpp"

namespace {
void ShowIo(const nlohmann::json& document, const char* direction) {
  bool first = true;
  for (const auto& entry : document.at("io").at(direction)) {
    if (!first) std::cout << ", ";
    first = false;
    std::cout << entry.at("type").get<std::string>() << "/"
              << entry.at("name").get<std::string>();
  }
}

void ShowParameters(const nlohmann::json& owner, const std::string& path) {
  if (!owner.contains("params")) return;
  for (const auto& [name, value] : owner.at("params").items())
    std::cout << "  " << path << ".params." << name << "=" << value.dump()
              << "\n";
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: alg_show PIPELINE_JSON\n";
    return 2;
  }
  try {
    std::ifstream stream(argv[1]);
    if (!stream) throw std::runtime_error("Cannot open Pipeline JSON");
    const auto document = nlohmann::json::parse(stream);
    std::cout << "Pipeline: " << std::filesystem::absolute(argv[1]) << "\n";
    std::cout << "io: ";
    ShowIo(document, "input");
    std::cout << " -> ";
    ShowIo(document, "output");
    std::cout << "\n";
    for (const auto& model :
         document.value("models", nlohmann::json::array())) {
      const auto category = model.at("type").get<std::string>();
      const auto backend = model.at("backend").at("type").get<std::string>();
      const auto implementations =
          llm_edgeflow::ModelRegistry::Instance().FindImplementation(category,
                                                                     backend);
      std::cout << "Model " << model.at("name").get<std::string>() << ": "
                << category << " / "
                << (implementations.size() == 1
                        ? implementations.front().impl_name
                        : "unavailable in this build")
                << " / " << backend << " / "
                << model.at("file").get<std::string>() << "\n";
      ShowParameters(model, model.at("name").get<std::string>());
      ShowParameters(model.at("backend"), "backend");
    }
    for (const auto& node : document.at("pipeline")) {
      const auto name = node.at("name").get<std::string>();
      std::cout << "Node " << name << ": " << node.at("type").get<std::string>()
                << "\n";
      const auto inputs = node.value("inputs", nlohmann::json::object());
      for (const auto& [port, source] : inputs.items())
        std::cout << "  " << source.get<std::string>() << " -> " << name << "."
                  << port << "\n";
      for (const auto& dependency :
           node.value("depends_on", nlohmann::json::array()))
        std::cout << "  order: " << dependency.get<std::string>() << " -> "
                  << name << "\n";
    }
    for (const auto& entry : document.at("io").at("output")) {
      const auto inputs = entry.value("inputs", nlohmann::json::object());
      for (const auto& [port, source] : inputs.items())
        std::cout << "  " << source.get<std::string>() << " -> output." << port
                  << " (" << entry.at("type").get<std::string>() << "/"
                  << entry.at("name").get<std::string>() << ")\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
