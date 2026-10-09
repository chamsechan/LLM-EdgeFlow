#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "adapter/io_catalog.h"
#include "core/diagnostic_code.h"
#include "core/pipeline_catalog.h"
#include "edgeflow/operator/interface.h"
#include "nlohmann/json.hpp"
#include "pipeline_document_validation.h"

namespace {

using llm_edgeflow::DiagnosticCode;
using llm_edgeflow::DiagnosticCodeName;
using llm_edgeflow::PipelineCatalog;

nlohmann::json PipelineError(DiagnosticCode code, const std::string& message) {
  return {{"ok", false},
          {"diagnostics",
           nlohmann::json::array({{{"code", DiagnosticCodeName(code)},
                                   {"path", "/"},
                                   {"message", message},
                                   {"severity", "error"}}})}};
}

nlohmann::json ToolError(const std::string& code, const std::string& message,
                         const std::string& path = "/") {
  return {{"ok", false},
          {"diagnostics", nlohmann::json::array({{{"code", code},
                                                  {"path", path},
                                                  {"message", message},
                                                  {"severity", "error"}}})}};
}

// 构建/测试注册指引保留在 CLI 中；SDK/Core 诊断保持与工具无关。
// 被包装的部署错误另行提供其原始错误码。
void PrintRegistrationHint(const nlohmann::json& response,
                           const std::string& source_code = {}) {
#ifdef LLM_EDGEFLOW_TOOL_HAS_TEST_REGISTRATIONS
  (void)response;
  (void)source_code;
#else
  auto unknown_registration = [](const std::string& code) {
    return code == "UNKNOWN_MODEL_TYPE" || code == "UNKNOWN_BACKEND";
  };
  auto has_unknown_registration = [&](const nlohmann::json& report) {
    auto diagnostics = report.find("diagnostics");
    if (diagnostics == report.end() || !diagnostics->is_array()) return false;
    for (const auto& diagnostic : *diagnostics) {
      if (diagnostic.is_object() &&
          unknown_registration(diagnostic.value("code", "")))
        return true;
    }
    return false;
  };
  bool needs_hint =
      unknown_registration(source_code) || has_unknown_registration(response);
  auto validation = response.find("validation");
  if (validation != response.end() && validation->is_object()) {
    needs_hint = needs_hint || has_unknown_registration(*validation);
  }
  if (needs_hint) {
    std::cerr << "提示：当前 alg_pipeline_tool 只包含本次构建启用的生产注册。\n"
              << "  - 可选 Backend 需要用对应的构建预设重新构建，见 "
                 "doc/VERIFIABLE_SELECTION.md#构建变体。\n"
              << "  - 使用测试 Model/Backend 的配置（例如 demo/fixtures/mock/ "
                 "下的方案）"
                 "请改用 alg_pipeline_tool_test，见 "
                 "tools/pipeline_studio/README.md#校验工具选择。\n";
  }
#endif
}

bool ReadJson(const std::string& path, nlohmann::json* output,
              std::string* error) {
  try {
    if (path == "--stdin") {
      std::cin >> *output;
    } else {
      std::ifstream stream(path);
      if (!stream.is_open()) {
        if (error) *error = "Cannot open JSON file: " + path;
        return false;
      }
      stream >> *output;
    }
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

int UnsupportedDuringRefactor(const std::string& command) {
  std::cerr << "Unsupported during staged refactor: " << command
            << "; CLI support will be updated in step 9.\n";
  return 2;
}

void Usage() {
  std::cerr << "Usage:\n"
            << "  alg_pipeline_tool catalog\n"
            << "  alg_pipeline_tool describe-node NODE_TYPE\n"
            << "  alg_pipeline_tool describe-model MODEL_TYPE\n"
            << "  alg_pipeline_tool describe-backend BACKEND_TYPE\n"
            << "  alg_pipeline_tool validate FILE|--stdin [--explain]\n"
            << "  alg_pipeline_tool plan FILE|--stdin [--explain]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  const std::string command = argv[1];

  if (command == "export-schema" || command == "init" || command == "edit" ||
      command == "resolve-conf" || command == "validate-io" ||
      command == "list-solutions") {
    return UnsupportedDuringRefactor(command);
  }

  if (command == "catalog") {
    if (argc != 2) return UnsupportedDuringRefactor("catalog filtering");
    const auto snapshot = PipelineCatalog::Snapshot();
    if (snapshot.node_registry_has_conflict) {
      std::string message = "Node registry contains registration conflicts";
      for (const auto& err : snapshot.node_registry_errors) {
        message += ": " + err;
      }
      std::cout
          << PipelineError(DiagnosticCode::kRegistryConflict, message).dump(2)
          << std::endl;
      return 1;
    }
    auto result = llm_edgeflow::IoCatalog::ToJson();
    result["ok"] = true;
    std::cout << result.dump(2) << std::endl;
    return 0;
  }

  if (command == "describe-node") {
    if (argc != 3) {
      Usage();
      return 2;
    }
    const auto definition = PipelineCatalog::FindNode(argv[2]);
    if (!definition) {
      std::cout
          << PipelineError(DiagnosticCode::kUnknownNodeType, argv[2]).dump(2)
          << std::endl;
      return 1;
    }
    auto result = PipelineCatalog::NodeToJson(*definition);
    result["ok"] = true;
    std::cout << result.dump(2) << std::endl;
    return 0;
  }

  if (command == "describe-model" || command == "describe-backend") {
    if (argc != 3) {
      Usage();
      return 2;
    }
    nlohmann::json result;
    if (command == "describe-model") {
      const auto definition = PipelineCatalog::FindModel(argv[2]);
      if (!definition) {
        const auto error =
            PipelineError(DiagnosticCode::kUnknownModelType, argv[2]);
        std::cout << error.dump(2) << std::endl;
        PrintRegistrationHint(error);
        return 1;
      }
      result = PipelineCatalog::ModelToJson(*definition);
    } else {
      const auto definition = PipelineCatalog::FindBackend(argv[2]);
      if (!definition) {
        const auto error =
            PipelineError(DiagnosticCode::kUnknownBackend, argv[2]);
        std::cout << error.dump(2) << std::endl;
        PrintRegistrationHint(error);
        return 1;
      }
      result = PipelineCatalog::BackendToJson(*definition);
    }
    result["ok"] = true;
    std::cout << result.dump(2) << std::endl;
    return 0;
  }

  if (command == "validate" || command == "plan") {
    if (argc < 3 || argc > 4) {
      Usage();
      return 2;
    }
    bool explain = false;
    std::string file;
    for (int i = 2; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--explain") {
        explain = true;
      } else if (file.empty()) {
        file = arg;
      } else {
        Usage();
        return 2;
      }
    }
    if (file.empty()) {
      Usage();
      return 2;
    }
    nlohmann::json root;
    std::string error;
    if (!ReadJson(file, &root, &error)) {
      std::cout << ToolError("JSON_READ", error).dump(2) << std::endl;
      return 1;
    }
    const auto ops =
        llm_edgeflow::operator_api::Get_LLM_EDGEFLOW_OperatorTable();
    if (ops.Init != nullptr && ops.Init() != 0) {
      std::cout << PipelineError(
                       DiagnosticCode::kRegistryConflict,
                       llm_edgeflow::operator_api::GetOperatorLastError())
                       .dump(2)
                << std::endl;
      return 1;
    }
    struct OpsGuard {
      llm_edgeflow::operator_api::OperatorFunc ops;
      ~OpsGuard() {
        if (ops.DeInit != nullptr) ops.DeInit();
      }
    } ops_guard{ops};

    using llm_edgeflow::DocumentValidationMode;
    DocumentValidationMode mode =
        (command == "validate") ? (explain ? DocumentValidationMode::kExplain
                                           : DocumentValidationMode::kValidate)
                                : DocumentValidationMode::kPlan;

    try {
      const std::string pipeline_dir =
          file == "--stdin"
              ? ""
              : std::filesystem::absolute(file).parent_path().string();
      auto result =
          llm_edgeflow::ValidatePipelineDocument(root, mode, pipeline_dir);
      std::cout << result.response.dump(2) << std::endl;
      PrintRegistrationHint(result.response);
      return result.ok ? 0 : 1;
    } catch (const std::exception& error) {
      std::cout << ToolError("INTERNAL_EXCEPTION", error.what()).dump(2)
                << std::endl;
      return 1;
    } catch (...) {
      std::cout << ToolError("INTERNAL_EXCEPTION", "Unknown internal exception")
                       .dump(2)
                << std::endl;
      return 1;
    }
  }

  Usage();
  return 2;
}
