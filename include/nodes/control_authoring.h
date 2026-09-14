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
#include "core/common_contracts.h"
#include "core/node_definition.h"
#include "nodes/configuration_snapshot.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_result.h"
#include "nodes/parameter_binding.h"

namespace llm_edgeflow {

enum class ControlFieldStrategy {
  kPatch,
  kReplace,
};

class FieldControlCommand {
 public:
  FieldControlCommand(ControlFieldStrategy strategy, int cmd_id,
                      std::string name, std::vector<std::string> field_names,
                      std::string description = "")
      : strategy_(strategy),
        cmd_id_(cmd_id),
        name_(std::move(name)),
        field_names_(std::move(field_names)),
        description_(std::move(description)) {}

  ControlFieldStrategy Strategy() const noexcept { return strategy_; }
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

  FieldControlCommand& SharedId(bool shared) {
    shared_id_ = shared;
    return *this;
  }

  bool IsSharedId() const noexcept { return shared_id_; }

  template <typename ParamsT>
  nlohmann::json GenerateSchema(const Parameters<ParamsT>& params) const {
    nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
    };
    nlohmann::json props = nlohmann::json::object();
    nlohmann::json req = nlohmann::json::array();
    for (const auto& fname : field_names_) {
      const auto* binding = params.FindBinding(fname);
      if (!binding) continue;
      ConfigFieldDefinition def = binding->ToFieldDefinition();
      nlohmann::json prop = nlohmann::json::object();
      switch (def.kind) {
        case ConfigValueKind::kString:
          prop["type"] = "string";
          if (!def.enum_values.empty()) {
            prop["enum"] = def.enum_values;
          }
          break;
        case ConfigValueKind::kBoolean:
          prop["type"] = "boolean";
          break;
        case ConfigValueKind::kInteger:
          prop["type"] = "integer";
          if (def.minimum.has_value()) prop["minimum"] = *def.minimum;
          if (def.maximum.has_value()) prop["maximum"] = *def.maximum;
          break;
        case ConfigValueKind::kNumber:
          prop["type"] = "number";
          if (def.minimum.has_value()) prop["minimum"] = *def.minimum;
          if (def.maximum.has_value()) prop["maximum"] = *def.maximum;
          break;
        case ConfigValueKind::kArray:
          prop["type"] = "array";
          prop["items"] = {{"type", "string"}};
          break;
        default:
          break;
      }
      if (!def.semantic.empty()) {
        prop["description"] = def.semantic;
      }
      if (!def.default_value.is_null()) {
        prop["default"] = def.default_value;
      }
      props[fname] = std::move(prop);
      if (strategy_ == ControlFieldStrategy::kReplace) {
        req.push_back(fname);
      }
    }
    schema["properties"] = std::move(props);
    if (strategy_ == ControlFieldStrategy::kReplace) {
      schema["required"] = std::move(req);
    } else {
      schema["minProperties"] = 1;
    }
    return schema;
  }

  template <typename ParamsT>
  ControlCommandDefinition ToCommandDefinition(
      const Parameters<ParamsT>& params) const {
    ControlCommandDefinition def(cmd_id_, name_,
                                 description_.empty() ? name_ : description_,
                                 GenerateSchema(params), /*is_hot_swap=*/true);
    def.shared_id = shared_id_;
    return def;
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
              if (strategy_ == ControlFieldStrategy::kReplace) {
                if (!payload.contains(field_name)) {
                  return NodeResult<ParamsT>::Failure(
                      NodeErrorKind::kBusinessError,
                      "Missing required field in control payload: " +
                          field_name,
                      node_error::control::kInvalidRequest);
                }
                std::string assign_err;
                if (!params.AssignField(field_name, payload[field_name], &next,
                                        &assign_err)) {
                  return NodeResult<ParamsT>::Failure(
                      NodeErrorKind::kBusinessError, assign_err,
                      node_error::control::kInvalidRequest);
                }
              } else {  // kPatch
                if (payload.contains(field_name)) {
                  std::string assign_err;
                  if (!params.AssignField(field_name, payload[field_name],
                                          &next, &assign_err)) {
                    return NodeResult<ParamsT>::Failure(
                        NodeErrorKind::kBusinessError, assign_err,
                        node_error::control::kInvalidRequest);
                  }
                }
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
  ControlFieldStrategy strategy_;
  int cmd_id_;
  std::string name_;
  std::vector<std::string> field_names_;
  std::string description_;
  bool shared_id_ = false;
};

inline FieldControlCommand ReplaceFields(int cmd_id, std::string name,
                                         std::vector<std::string> field_names,
                                         std::string description = "") {
  return FieldControlCommand(ControlFieldStrategy::kReplace, cmd_id,
                             std::move(name), std::move(field_names),
                             std::move(description));
}

inline FieldControlCommand PatchFields(int cmd_id, std::string name,
                                       std::vector<std::string> field_names,
                                       std::string description = "") {
  return FieldControlCommand(ControlFieldStrategy::kPatch, cmd_id,
                             std::move(name), std::move(field_names),
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
      ConfigFieldDefinition def = binding->ToFieldDefinition();
      if (def.kind != ConfigValueKind::kString &&
          def.kind != ConfigValueKind::kBoolean &&
          def.kind != ConfigValueKind::kInteger &&
          def.kind != ConfigValueKind::kNumber &&
          def.kind != ConfigValueKind::kArray) {
        throw std::invalid_argument(
            "Field '" + f + "' has unsupported kind for control command");
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
