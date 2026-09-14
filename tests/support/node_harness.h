#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "contracts/inference_payloads.h"
#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/node_interface.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "core/validated_node_plan.h"
#include "engine/backend_registry.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

class NodeHarnessResult {
 public:
  NodeHarnessResult() = default;
  NodeHarnessResult(NodeHarnessResult&&) noexcept = default;
  NodeHarnessResult& operator=(NodeHarnessResult&&) noexcept = default;

  static NodeHarnessResult InitFailed(std::string diag) {
    NodeHarnessResult r;
    r.ok_ = false;
    r.init_failed_ = true;
    r.diagnostic_ = std::move(diag);
    return r;
  }

  static NodeHarnessResult ProcessFailed(
      int code, std::string diag, std::unique_ptr<AlgContext> ctx = nullptr,
      std::unordered_map<std::string, std::string> output_keys = {}) {
    NodeHarnessResult r;
    r.ok_ = false;
    r.process_code_ = code;
    r.diagnostic_ = std::move(diag);
    r.ctx_ = std::move(ctx);
    r.output_keys_ = std::move(output_keys);
    return r;
  }

  static NodeHarnessResult Success(
      std::unique_ptr<AlgContext> ctx,
      std::unordered_map<std::string, std::string> output_keys) {
    NodeHarnessResult r;
    r.ok_ = true;
    r.ctx_ = std::move(ctx);
    r.output_keys_ = std::move(output_keys);
    return r;
  }

  bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }
  const std::string& diagnostic() const noexcept { return diagnostic_; }
  int process_code() const noexcept { return process_code_; }
  bool init_failed() const noexcept { return init_failed_; }

  template <typename T>
  const T* Output(const std::string& logical_port_name) const {
    if (!ctx_) return nullptr;
    auto it = output_keys_.find(logical_port_name);
    if (it == output_keys_.end()) {
      return ctx_->Read<T>(logical_port_name);
    }
    return ctx_->Read<T>(it->second);
  }

  std::vector<std::string> TextValues(
      const std::string& logical_port_name) const {
    const auto* batch = Output<TextBatch>(logical_port_name);
    if (!batch) {
      throw std::runtime_error("Port '" + logical_port_name +
                               "' missing or not TextBatch in AlgContext");
    }
    std::vector<std::string> result;
    result.reserve(batch->size());
    for (const auto& item : *batch) {
      result.push_back(item.data);
    }
    return result;
  }

  const AlgContext* Context() const noexcept { return ctx_.get(); }

 private:
  bool ok_ = false;
  bool init_failed_ = false;
  int process_code_ = 0;
  std::string diagnostic_;
  std::unique_ptr<AlgContext> ctx_;
  std::unordered_map<std::string, std::string> output_keys_;
};

class NodeHarness {
 public:
  explicit NodeHarness(std::string node_type)
      : node_type_(std::move(node_type)), config_(nlohmann::json::object()) {}

  NodeHarness& Config(nlohmann::json config) {
    config_ = std::move(config);
    Reset();
    return *this;
  }

  void Reset() {
    node_.reset();
    session_ctx_.reset();
    input_keys_.clear();
    output_keys_.clear();
    init_diagnostic_.clear();
    initialized_ = false;
  }

  NodeHarness& TextInput(const std::string& logical_port_name,
                         const std::vector<std::string>& payloads,
                         uint64_t start_req_id = 101) {
    TextBatch batch;
    batch.reserve(payloads.size());
    for (size_t i = 0; i < payloads.size(); ++i) {
      uint64_t req_id = start_req_id + (i >= 1 ? 1 : 0);
      uint32_t sub_id = (i >= 1 ? static_cast<uint32_t>(i - 1) : 0);
      batch.emplace_back(req_id, sub_id, payloads[i]);
    }
    custom_inputs_[logical_port_name] =
        [batch = std::move(batch)](AlgContext& ctx, const std::string& key) {
          ctx.Publish(key, batch);
        };
    return *this;
  }

  NodeHarness& TextInputWithBatch(const std::string& logical_port_name,
                                  TextBatch batch) {
    custom_inputs_[logical_port_name] =
        [batch = std::move(batch)](AlgContext& ctx, const std::string& key) {
          ctx.Publish(key, batch);
        };
    return *this;
  }

  template <typename T>
  NodeHarness& CustomInput(const std::string& logical_port_name, T value) {
    custom_inputs_[logical_port_name] = [val = std::move(value)](
                                            AlgContext& ctx,
                                            const std::string& key) mutable {
      ctx.Publish(key, std::move(val));
    };
    return *this;
  }

  NodeHarness& BindModel(std::string model_id, std::shared_ptr<IModel> model) {
    models_[std::move(model_id)] = std::move(model);
    Reset();
    return *this;
  }

  NodeHarness& DisablePlan() {
    use_plan_ = false;
    Reset();
    return *this;
  }

  NodeHarness& OmitPortFromPlan(std::string logical_port_name) {
    omitted_ports_.insert(std::move(logical_port_name));
    Reset();
    return *this;
  }

  bool EnsureInitialized() {
    if (initialized_) return true;

    if (!NodeRegistry::Instance().Has(node_type_)) {
      init_diagnostic_ = "Node type '" + node_type_ + "' is not registered";
      return false;
    }

    node_ = NodeRegistry::Instance().Create(node_type_);
    if (!node_) {
      init_diagnostic_ = "Failed to create node '" + node_type_ + "'";
      return false;
    }

    session_ctx_ = std::make_unique<SessionContext>();
    for (const auto& [mid, model] : models_) {
      session_ctx_->GetModelManager().RegisterModel(
          mid, model, "harness_rev", model ? model->ModelType() : "mock",
          model ? model->Capability() : "llm", "mock");
    }

    const auto definition = PipelineCatalog::FindNode(node_type_);
    input_keys_.clear();
    output_keys_.clear();

    NodeInitContext init_ctx;
    init_ctx.config = &config_;
    init_ctx.session_ctx = session_ctx_.get();
    init_diagnostic_.clear();
    init_ctx.diagnostic = &init_diagnostic_;

    if (use_plan_) {
      if (definition) {
        nlohmann::json doc = nlohmann::json::object();
        std::string biz = "harness_biz";
        doc["biz_name"] = biz;
        doc["models"] = nlohmann::json::array();

        nlohmann::json normalized_config;
        if (!ValidateAndNormalizeFields(definition->config_fields, config_,
                                        &normalized_config, nullptr)) {
          normalized_config = config_;
        }
        std::unordered_set<std::string> declared_models;
        for (const auto& dep : definition->model_dependencies) {
          if (normalized_config.contains(dep.config_field) &&
              normalized_config[dep.config_field].is_string()) {
            std::string mid =
                normalized_config[dep.config_field].get<std::string>();
            if (!declared_models.insert(mid).second) continue;
            const std::string mtype = "harness_dummy_m_" + dep.capability;
            const std::string mbackend = "harness_dummy_b_" + dep.capability;
            if (!ModelRegistry::Instance().Has(mtype)) {
              ModelDefinition model_definition;
              model_definition.model_type = mtype;
              model_definition.capability = dep.capability;
              model_definition.required_protocol =
                  ExecutionProtocol::kTensorGraph;
              model_definition.concurrency = InferenceConcurrency::kConcurrent;
              ModelRegistry::Instance().Register(
                  model_definition, [](const auto&, auto*) { return nullptr; });
            }
            if (!BackendRegistry::Instance().Has(mbackend)) {
              BackendDefinition backend_definition;
              backend_definition.backend_type = mbackend;
              backend_definition.supported_protocols = {
                  ExecutionProtocol::kTensorGraph};
              backend_definition.concurrency =
                  InferenceConcurrency::kConcurrent;
              BackendRegistry::Instance().Register(backend_definition,
                                                   []() { return nullptr; });
            }
            doc["models"].push_back({
                {"model_id", mid},
                {"capability", dep.capability},
                {"model_type", mtype},
                {"backend", mbackend},
                {"model_path", "mock.bin"},
                {"model_config", nlohmann::json::object()},
                {"backend_config", nlohmann::json::object()},
            });
          }
        }

        nlohmann::json node_json = nlohmann::json::object();
        node_json["id"] = "harness_node";
        node_json["node_type"] = node_type_;
        node_json["depends_on"] = nlohmann::json::array();
        node_json["config"] = config_;

        nlohmann::json ports_json = nlohmann::json::object();
        ports_json["inputs"] = nlohmann::json::object();
        ports_json["outputs"] = nlohmann::json::object();
        for (const auto& in_def : definition->inputs) {
          if (omitted_ports_.count(in_def.logical_name) == 0) {
            ports_json["inputs"][in_def.logical_name] =
                "bk_in_" + in_def.logical_name;
          }
        }
        for (const auto& out_def : definition->outputs) {
          ports_json["outputs"][out_def.logical_name] =
              "bk_out_" + out_def.logical_name;
        }
        node_json["ports"] = std::move(ports_json);
        doc["pipeline"] = nlohmann::json::array({std::move(node_json)});

        auto plan_res = PipelineValidator::ValidateAndPlan(
            doc, ValidationPolicy::kPrivateExtensionCompatible);
        if (!plan_res.report.ok) {
          std::string err_msg = "Configuration validation failed:";
          for (const auto& d : plan_res.report.diagnostics) {
            err_msg += " [" + d.path + "] " + d.message;
          }
          init_diagnostic_ = err_msg;
          return false;
        }

        auto it = plan_res.node_plans.find("harness_node");
        if (it != plan_res.node_plans.end()) {
          plan_ = std::move(it->second);
          for (const auto& p : plan_.ports) {
            if (p.direction == PortDirection::kInput) {
              input_keys_[p.logical_name] = p.blackboard_key;
            } else if (p.direction == PortDirection::kOutput) {
              output_keys_[p.logical_name] = p.blackboard_key;
            }
          }
        }
      }
      init_ctx.plan = &plan_;
    } else {
      if (definition) {
        for (const auto& in_def : definition->inputs) {
          input_keys_[in_def.logical_name] = in_def.logical_name;
        }
        for (const auto& out_def : definition->outputs) {
          output_keys_[out_def.logical_name] = out_def.logical_name;
        }
      }
    }

    if (!node_->Init(init_ctx)) {
      if (init_diagnostic_.empty()) init_diagnostic_ = "Node Init failed";
      return false;
    }

    initialized_ = true;
    return true;
  }

  NodeControlResult Control(int cmd, const std::string& json_param) {
    if (!EnsureInitialized()) {
      return NodeControlResult::Failed(
          node_error::control::kInvalidRequest,
          init_diagnostic_.empty() ? "Node Init failed" : init_diagnostic_);
    }
    return node_->Control(cmd, json_param);
  }

  NodeControlResult Control(int cmd, const char* json_param) {
    return Control(cmd, std::string(json_param ? json_param : ""));
  }

  NodeControlResult Control(int cmd, const nlohmann::json& payload) {
    return Control(cmd, payload.dump());
  }

  INode* GetNode() const noexcept { return node_.get(); }

  NodeHarnessResult Run() {
    if (!EnsureInitialized()) {
      return NodeHarnessResult::InitFailed(
          init_diagnostic_.empty() ? "Node Init failed" : init_diagnostic_);
    }

    auto req_ctx = std::make_unique<AlgContext>();
    for (auto& [logical_name, publisher] : custom_inputs_) {
      std::string key = input_keys_.count(logical_name)
                            ? input_keys_[logical_name]
                            : logical_name;
      publisher(*req_ctx, key);
    }

    int ret = node_->Process(req_ctx.get());
    if (ret != 0) {
      std::string msg = "Process returned " + std::to_string(ret);
      if (!req_ctx->IsOk()) {
        msg += ": " + req_ctx->GetErrorMessage();
      }
      return NodeHarnessResult::ProcessFailed(ret, msg, std::move(req_ctx),
                                              output_keys_);
    }

    return NodeHarnessResult::Success(std::move(req_ctx), output_keys_);
  }

 private:
  std::string node_type_;
  nlohmann::json config_;
  bool use_plan_ = true;
  std::unordered_map<std::string,
                     std::function<void(AlgContext&, const std::string&)>>
      custom_inputs_;
  std::unordered_map<std::string, std::shared_ptr<IModel>> models_;
  std::unordered_set<std::string> omitted_ports_;

  std::unique_ptr<INode> node_;
  std::unique_ptr<SessionContext> session_ctx_;
  ValidatedNodePlan plan_;
  std::unordered_map<std::string, std::string> input_keys_;
  std::unordered_map<std::string, std::string> output_keys_;
  std::string init_diagnostic_;
  bool initialized_ = false;
};

}  // namespace llm_edgeflow
