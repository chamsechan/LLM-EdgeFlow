#pragma once

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "contracts/control_payload.h"
#include "contracts/parameters.h"
#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "nodes/configuration_snapshot.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_result.h"

namespace llm_edgeflow {

// 字段 Control 命令：payload 至少给出一个受控参数；给出的参数整体替换，
// 没给出的保持不变，替换后重新执行 Prepare、Validate。payload 的格式由
// 受控参数的声明生成。
class FieldControlCommand {
 public:
  FieldControlCommand(int cmd_id, std::string name,
                      std::vector<std::string> field_names,
                      std::string description = "")
      : cmd_id_(cmd_id),
        name_(std::move(name)),
        field_names_(std::move(field_names)),
        description_(std::move(description)) {}

  int Id() const noexcept { return cmd_id_; }
  const std::string& Name() const noexcept { return name_; }
  const std::vector<std::string>& FieldNames() const noexcept {
    return field_names_;
  }
  const std::string& Description() const noexcept { return description_; }

  FieldControlCommand& Description(std::string desc) {
    description_ = std::move(desc);
    return *this;
  }

  template <typename ParamsT>
  nlohmann::json GenerateSchema(const Parameters<ParamsT>& params) const {
    nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"minProperties", 1},
    };
    nlohmann::json props = nlohmann::json::object();
    for (const auto& fname : field_names_) {
      const auto* binding = params.FindBinding(fname);
      if (!binding) continue;
      props[fname] = ConfigFieldJsonSchema(binding->ToFieldDefinition());
    }
    schema["properties"] = std::move(props);
    return schema;
  }

  template <typename ParamsT>
  ControlCommandDefinition ToCommandDefinition(
      const Parameters<ParamsT>& params) const {
    return ControlCommandDefinition(cmd_id_, name_,
                                    description_.empty() ? name_ : description_,
                                    GenerateSchema(params),
                                    /*hot_swap=*/true);
  }

  template <typename ParamsT>
  NodeControlResult Execute(const Parameters<ParamsT>& params,
                            const std::string& json_param,
                            const BindingFacts& facts,
                            ConfigurationSnapshot<ParamsT>& snapshot) const {
    if constexpr (!std::is_copy_constructible_v<ParamsT>) {
      return NodeControlResult::Unsupported();
    } else {
      nlohmann::json payload;
      std::string err;
      auto cmd_def = ToCommandDefinition(params);
      if (!ParseControlPayload(json_param, cmd_def.payload_schema, &payload,
                               &err)) {
        return NodeControlResult::Failed(node_error::control::kInvalidRequest,
                                         err);
      }
      return snapshot.Update(
          [&](const ParamsT& current) -> NodeResult<ParamsT> {
            ParamsT next = current;
            for (const auto& field_name : field_names_) {
              if (!payload.contains(field_name)) continue;
              const auto* binding = params.FindBinding(field_name);
              nlohmann::json normalized;
              std::vector<ConfigFieldValidationError> field_errors;
              if (!binding) {
                return NodeResult<ParamsT>::Failure(
                    NodeErrorKind::kBusinessError,
                    "Field '" + field_name + "' is not bound in Parameters",
                    node_error::control::kInvalidRequest);
              }
              // 与配置使用同一套校验；控制命令没给出的参数不补默认值。
              auto definition = binding->ToFieldDefinition();
              definition.required = false;
              if (!NormalizeFieldValue(definition, payload[field_name],
                                       &normalized, &field_errors)) {
                const auto& first = field_errors.front();
                return NodeResult<ParamsT>::Failure(
                    NodeErrorKind::kBusinessError,
                    "Field '" + first.path + "': " + first.message,
                    node_error::control::kInvalidRequest);
              }
              std::string assign_err;
              if (!params.AssignField(field_name, normalized, &next,
                                      &assign_err)) {
                return NodeResult<ParamsT>::Failure(
                    NodeErrorKind::kBusinessError, assign_err,
                    node_error::control::kInvalidRequest);
              }
            }
            std::string val_err;
            if (!params.ValidateState(&next, facts, &val_err)) {
              return NodeResult<ParamsT>::Failure(
                  NodeErrorKind::kBusinessError,
                  val_err.empty()
                      ? "Parameter validation failed after control update"
                      : val_err,
                  node_error::control::kInvalidRequest);
            }
            return NodeResult<ParamsT>::Success(std::move(next));
          });
    }
  }

 private:
  int cmd_id_;
  std::string name_;
  std::vector<std::string> field_names_;
  std::string description_;
};

inline FieldControlCommand ReplaceFields(int cmd_id, std::string name,
                                         std::vector<std::string> field_names,
                                         std::string description = "") {
  return FieldControlCommand(cmd_id, std::move(name), std::move(field_names),
                             std::move(description));
}

template <typename ParamsT, typename ModelsT = void>
inline void ValidateControlCommands(
    const std::vector<FieldControlCommand>& commands,
    const Parameters<ParamsT>& params, const ModelsT* models = nullptr) {
  std::unordered_set<int> seen_ids;
  std::unordered_set<std::string> seen_names;
  for (const auto& cmd : commands) {
    if (cmd.Id() <= 0) {
      throw std::invalid_argument(
          "Control command ID must be a positive integer");
    }
    if (cmd.Name().empty()) {
      throw std::invalid_argument("Control command name cannot be empty");
    }
    if (!seen_ids.insert(cmd.Id()).second) {
      throw std::invalid_argument("Duplicate control command ID: " +
                                  std::to_string(cmd.Id()));
    }
    if (!seen_names.insert(cmd.Name()).second) {
      throw std::invalid_argument("Duplicate control command name: " +
                                  cmd.Name());
    }
    if (cmd.FieldNames().empty()) {
      throw std::invalid_argument("Control command '" + cmd.Name() +
                                  "' must specify at least one field");
    }
    std::unordered_set<std::string> seen_fields;
    for (const auto& f : cmd.FieldNames()) {
      if (!seen_fields.insert(f).second) {
        throw std::invalid_argument("Duplicate field '" + f +
                                    "' in control command '" + cmd.Name() +
                                    "'");
      }
      const auto* binding = params.FindBinding(f);
      if (!binding) {
        throw std::invalid_argument("Field '" + f + "' in control command '" +
                                    cmd.Name() +
                                    "' is not bound in Parameters");
      }
      if constexpr (!std::is_void_v<ModelsT>) {
        if (models) {
          for (const auto& b : models->Bindings()) {
            if (b && b->ConfigField() == f) {
              throw std::invalid_argument(
                  "Field '" + f +
                  "' is bound to a model and cannot be controlled");
            }
          }
        }
      }
    }
  }
  if (params.HasParser() && !commands.empty() && !params.HasPrepare()) {
    throw std::invalid_argument(
        "Spec with WithParser and WithControls requires an explicit Prepare "
        "function");
  }
}

}  // namespace llm_edgeflow
