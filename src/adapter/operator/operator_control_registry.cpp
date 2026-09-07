#include "adapter/operator/operator_control_registry.h"

#include <cmath>
#include <cstring>

#include "contracts/control_payload.h"
#include "core/common_contracts.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {

int OperatorControlRegistry::ResolveControlParam(
    llm_edgeflow::operator_api::ControlCommand command, void* control_param,
    int* out_cmd_id, std::string* out_json_str,
    std::string* error_msg) noexcept {
  try {
    if (!out_cmd_id || !out_json_str) {
      if (error_msg) *error_msg = "Null output pointer for control resolution";
      return -2;
    }

    if (!control_param) {
      if (error_msg) *error_msg = "Null control_param pointer";
      return -2;
    }

    constexpr size_t kMaxStringLength = 65536;

    switch (command) {
      case llm_edgeflow::operator_api::ControlCommand::kUpdateRules:
      case llm_edgeflow::operator_api::ControlCommand::kJson: {
        const char* payload = nullptr;
        int cmd = kControlCmdUpdateRules;
        if (command == llm_edgeflow::operator_api::ControlCommand::kJson) {
          const auto* param =
              static_cast<const llm_edgeflow::operator_api::ControlJsonParam*>(
                  control_param);
          cmd = param->cmd_id;
          payload = param->json_param_str;
          if (cmd <= 0) {
            if (error_msg)
              *error_msg = "ControlJsonParam::cmd_id must be positive";
            return -2;
          }
        } else {
          payload =
              static_cast<
                  const llm_edgeflow::operator_api::ControlUpdateRulesParam*>(
                  control_param)
                  ->rules_json_str;
        }
        if (!payload) {
          if (error_msg) {
            *error_msg = "Control JSON payload is null";
          }
          return -2;
        }
        size_t len = strnlen(payload, kMaxStringLength);
        if (len == 0 || len >= kMaxStringLength) {
          if (error_msg) {
            *error_msg =
                "Control JSON payload length invalid (empty "
                "or exceeds 64KB)";
          }
          return -2;
        }
        nlohmann::json parsed;
        if (!ParseControlPayload(std::string(payload, len),
                                 {{"type", "object"}}, &parsed, error_msg))
          return -2;
        *out_cmd_id = cmd;
        *out_json_str = parsed.dump();
        return 0;
      }

      case llm_edgeflow::operator_api::ControlCommand::kSwitchPrompt: {
        const auto* param = static_cast<
            const llm_edgeflow::operator_api::ControlSwitchPromptParam*>(
            control_param);
        if (!param->prompt_template_str) {
          if (error_msg) {
            *error_msg =
                "ControlSwitchPromptParam::prompt_template_str is null";
          }
          return -2;
        }
        size_t len = strnlen(param->prompt_template_str, kMaxStringLength);
        if (len == 0 || len >= kMaxStringLength) {
          if (error_msg) {
            *error_msg =
                "ControlSwitchPromptParam::prompt_template_str length invalid "
                "(empty or exceeds 64KB)";
          }
          return -2;
        }
        std::string prompt_id = "";
        if (param->prompt_id) {
          size_t id_len = strnlen(param->prompt_id, 256);
          if (id_len >= 256) {
            if (error_msg) {
              *error_msg =
                  "ControlSwitchPromptParam::prompt_id exceeds 256 bytes";
            }
            return -2;
          }
          prompt_id = param->prompt_id;
        }
        nlohmann::json j;
        j["template"] = param->prompt_template_str;
        if (!prompt_id.empty()) {
          j["prompt_id"] = prompt_id;
        }
        *out_cmd_id = kControlCmdUpdatePrompt;
        *out_json_str = j.dump();
        return 0;
      }

      case llm_edgeflow::operator_api::ControlCommand::kUpdateThreshold: {
        const auto* param = static_cast<
            const llm_edgeflow::operator_api::ControlUpdateThresholdParam*>(
            control_param);
        if (!std::isfinite(param->threshold) || param->threshold < 0.0f ||
            param->threshold > 1.0f) {
          if (error_msg) {
            *error_msg =
                "ControlUpdateThresholdParam::threshold must be a finite float "
                "in range [0.0, 1.0]: " +
                std::to_string(param->threshold);
          }
          return -2;
        }
        std::string cat = "";
        if (param->category_or_rule_name) {
          size_t cat_len = strnlen(param->category_or_rule_name, 256);
          if (cat_len >= 256) {
            if (error_msg) {
              *error_msg =
                  "ControlUpdateThresholdParam::category_or_rule_name exceeds "
                  "256 bytes";
            }
            return -2;
          }
          cat = param->category_or_rule_name;
        }
        nlohmann::json j;
        j["threshold"] = param->threshold;
        if (!cat.empty()) {
          j["category"] = cat;
        }
        *out_cmd_id = kControlCmdUpdateThreshold;
        *out_json_str = j.dump();
        return 0;
      }

      default: {
        if (error_msg) {
          *error_msg = "Unsupported or unknown ControlCommand: " +
                       std::to_string(static_cast<int>(command));
        }
        return -2;
      }
    }
  } catch (const std::exception& e) {
    if (error_msg) *error_msg = std::string("Exception: ") + e.what();
    return -99;
  } catch (...) {
    if (error_msg) *error_msg = "Unknown exception in ResolveControlParam";
    return -100;
  }
}

}  // namespace llm_edgeflow
