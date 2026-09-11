#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "contracts/diagnostic.h"

namespace llm_edgeflow {

// Optional authoring helper: one field list and one semantic parser for an
// ordinary parameter struct. It neither reads files nor serializes JSON.
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

  // For raw Node configuration or an already decoded Control payload. Reuses
  // the same field validation/defaults as PipelineValidator and ModelBoundNode.
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

  // Only for input already validated/defaulted with Fields(), such as a
  // Definition's validate_config callback or ModelBoundNode::InitModelNode.
  // Forwards the existing JSON object directly; does not copy or normalize it
  // again. The parser owns semantic checks and must return owned parameter
  // data.
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
