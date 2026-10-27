#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "contracts/config_schema_validation.h"
#include "core/alg_context.h"
#include "core/node_registry.h"
#include "core/session_context.h"
#include "core/validated_node_plan.h"

namespace llm_edgeflow::test_support {

struct NodeBinding {
  std::string name;
  std::string key;
  std::string lifetime{};
};

// 算法测试提供显式 key 并使用生产声明。拓扑、边界闭合和数据流校验由
// Validator 测试单独负责。
struct NodeProbe {
  std::string diagnostic;
  ValidatedNodePlan plan;
  std::unique_ptr<INode> node;

  bool Init(const std::string& type, const nlohmann::json& params,
            SessionContext& session, const std::vector<NodeBinding>& inputs,
            const std::vector<NodeBinding>& outputs) {
    node.reset();
    plan = {};
    diagnostic.clear();
    const auto definition = NodeRegistry::Instance().Find(type);
    if (!definition) {
      diagnostic = "Unregistered Node: " + type;
      return false;
    }
    std::vector<ConfigFieldValidationError> errors;
    if (!ValidateAndNormalizeFields(definition->config_fields, params,
                                    &plan.normalized_params, &errors)) {
      if (!errors.empty())
        diagnostic = errors.front().path + ": " + errors.front().message;
      return false;
    }
    plan.node.name = type;
    plan.node.node_type = type;
    const auto bind = [&](const std::vector<NodeBinding>& bindings,
                          const std::vector<NodePortDefinition>& declarations,
                          PortDirection direction) {
      for (const auto& binding : bindings) {
        const auto port = std::find_if(
            declarations.begin(), declarations.end(), [&](const auto& item) {
              return item.logical_name == binding.name;
            });
        if (port == declarations.end()) {
          diagnostic = "Undeclared port: " + binding.name;
          return false;
        }
        std::string lifetime =
            binding.lifetime.empty() ? port->lifetime : binding.lifetime;
        if (direction == PortDirection::kOutput && binding.lifetime.empty() &&
            !port->lifetime_from_input.empty()) {
          const auto* anchor = plan.FindPort(port->lifetime_from_input);
          if (anchor) lifetime = anchor->lifetime;
        }
        plan.ports.push_back({binding.name, binding.key, port->type_id,
                              port->cardinality, port->provenance_policy,
                              std::move(lifetime), direction});
      }
      return true;
    };
    if (!bind(inputs, definition->inputs, PortDirection::kInput) ||
        !bind(outputs, definition->outputs, PortDirection::kOutput))
      return false;
    for (const auto& dependency : definition->model_dependencies) {
      plan.model_bindings.push_back(
          {dependency.name, dependency.model_type, dependency.config_field,
           plan.normalized_params.at(dependency.config_field)
               .get<std::string>()});
    }
    node = NodeRegistry::Instance().Create(type);
    if (!node) {
      diagnostic = "Node creator returned null: " + type;
      return false;
    }
    // Init 成功前 node 保持为空，调用方拿不到模型绑定或参数准备失败的 node。
    if (!node->Init({&plan, &session, &diagnostic})) {
      node.reset();
      return false;
    }
    return true;
  }
};

inline bool RegisterNodeModel(SessionContext& session, std::string name,
                              std::shared_ptr<IModel> model) {
  ModelRegistration registration;
  registration.model_name = std::move(name);
  registration.impl_name = model->ImplName();
  registration.model_type = model->ModelType();
  registration.model = std::move(model);
  return session.GetModelManager().RegisterBatch({std::move(registration)});
}

inline std::string NodeInputKey(const std::string& port) {
  return "input." + port;
}
inline std::string NodeOutputKey(const std::string& port) {
  return "output." + port;
}

// 一个逻辑输入端口的请求值。
struct NodeInput {
  template <typename Batch>
  NodeInput(std::string port, Batch batch)
      : port(std::move(port)),
        publish([batch = std::move(batch)](AlgContext& context,
                                           const std::string& key) {
          return context.Publish(key, batch);
        }) {}

  std::string port;
  std::function<bool(AlgContext&, const std::string&)> publish;
};

// 一次 Process 调用。输出视图属于此处持有的请求上下文，在本对象存活期间
// 保持有效。
class NodeRun {
 public:
  int status = 0;
  std::string diagnostic;

  template <typename Batch>
  const Batch* Output(const std::string& port) const {
    return context_->Read<Batch>(NodeOutputKey(port));
  }
  bool Published(const std::string& port) const {
    return context_->Has(NodeOutputKey(port));
  }

 private:
  friend class NodeUnderTest;
  std::unique_ptr<AlgContext> context_ = std::make_unique<AlgContext>();
};

// 业务 Node 测试的默认入口：作者给出逻辑端口名，提供输入、参数和模型替身；
// Session、存储 key 和请求上下文由这里管理。需要共享 Session、覆盖端口
// 生命周期或在同一上下文中串联多个 Node 的测试，直接使用 NodeProbe。
class NodeUnderTest {
 public:
  std::string diagnostic;

  bool AddModel(std::string name, std::shared_ptr<IModel> model) {
    return RegisterNodeModel(session_, std::move(name), std::move(model));
  }

  bool Init(const std::string& type, const nlohmann::json& params,
            const std::vector<std::string>& inputs,
            const std::vector<std::string>& outputs) {
    const auto bind = [](const std::vector<std::string>& ports,
                         std::string (*key)(const std::string&)) {
      std::vector<NodeBinding> bindings;
      for (const auto& port : ports) bindings.push_back({port, key(port)});
      return bindings;
    };
    const bool ok =
        probe_.Init(type, params, session_, bind(inputs, &NodeInputKey),
                    bind(outputs, &NodeOutputKey));
    diagnostic = probe_.diagnostic;
    return ok;
  }

  NodeRun Process(std::initializer_list<NodeInput> inputs) {
    NodeRun run;
    if (!probe_.node) {
      ADD_FAILURE() << "Process before a successful Init";
      run.status = -1;
      return run;
    }
    for (const auto& input : inputs) {
      if (!probe_.plan.FindPort(input.port, PortDirection::kInput) ||
          !input.publish(*run.context_, NodeInputKey(input.port))) {
        ADD_FAILURE() << "Input port not connected or given twice: "
                      << input.port;
        run.status = -1;
        return run;
      }
    }
    run.status = probe_.node->Process(run.context_.get());
    run.diagnostic = run.context_->GetErrorMessage();
    return run;
  }

  NodeControlResult Control(int command, const std::string& payload) {
    if (!probe_.node) {
      ADD_FAILURE() << "Control before a successful Init";
      return NodeControlResult::Unsupported();
    }
    return probe_.node->Control(command, payload);
  }

 private:
  SessionContext session_;
  NodeProbe probe_;
};

}  // namespace llm_edgeflow::test_support
