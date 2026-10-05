#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "contracts/diagnostic.h"

namespace llm_edgeflow {

// 可选的编写辅助：为普通参数 struct 提供一份字段列表和一个语义解析器。
// 不读文件，也不序列化 JSON。
template <typename Parameters>
class NodeConfigParser {
 public:
  using ParseFn =
      std::function<bool(const nlohmann::json&, Parameters*, std::string*)>;

  NodeConfigParser(std::vector<ConfigFieldDefinition> fields, ParseFn parse)
      : fields_(std::move(fields)), parse_(std::move(parse)) {}

  const std::vector<ConfigFieldDefinition>& Fields() const noexcept {
    return fields_;
  }

  // 用于原始 Node 配置或已解码的 Control payload。复用与 PipelineValidator
  // 和 AuthorNode 相同的字段校验与默认值。
  std::optional<Parameters> Parse(const nlohmann::json& config,
                                  std::string* error = nullptr) const noexcept {
    if (error) error->clear();
    try {
      nlohmann::json normalized;
      std::vector<ConfigFieldValidationError> errors;
      if (!ValidateAndNormalizeFields(fields_, config, &normalized, &errors)) {
        SetDiagnosticNoexcept(error, errors.empty()
                                         ? "Invalid Node configuration"
                                         : errors.front().message);
        return std::nullopt;
      }
      return ParseNormalized(normalized, error);
    } catch (const std::exception& exception) {
      SetDiagnosticNoexcept(error, exception.what());
    } catch (...) {
      SetDiagnosticNoexcept(error,
                            "Unknown exception parsing Node configuration");
    }
    return std::nullopt;
  }

  // 仅用于已按 Fields() 校验并填充默认值的输入，如 Definition 的
  // validate_config 回调或 AuthorNode 初始化。直接转发现有 JSON 对象，
  // 不再复制或归一化。解析器负责语义检查，并须返回自有的参数数据。
  std::optional<Parameters> ParseNormalized(
      const nlohmann::json& config,
      std::string* error = nullptr) const noexcept {
    if (error) error->clear();
    try {
      if (!parse_) {
        SetDiagnosticNoexcept(error, "Missing Node configuration parser");
        return std::nullopt;
      }
      Parameters next{};
      if (!parse_(config, &next, error)) {
        if (error && error->empty())
          SetDiagnosticNoexcept(error, "Invalid Node configuration");
        return std::nullopt;
      }
      return std::optional<Parameters>(std::move(next));
    } catch (const std::exception& exception) {
      SetDiagnosticNoexcept(error, exception.what());
    } catch (...) {
      SetDiagnosticNoexcept(error,
                            "Unknown exception parsing Node configuration");
    }
    return std::nullopt;
  }

 private:
  std::vector<ConfigFieldDefinition> fields_;
  ParseFn parse_;
};

}  // namespace llm_edgeflow
