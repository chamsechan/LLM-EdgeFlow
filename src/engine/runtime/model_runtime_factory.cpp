#include "engine/model_runtime_factory.h"

#include <algorithm>
#include <filesystem>
#include <utility>

#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

namespace {

void SetDiagnostic(std::string* diagnostic, const char* message) noexcept {
  if (!diagnostic) return;
  try {
    *diagnostic = message;
  } catch (...) {
  }
}

}  // namespace

std::shared_ptr<IModel> ModelRuntimeFactory::Create(
    const ModelLoadSpec& spec, std::string* diagnostic) noexcept {
  try {
    // 1. 查找 BackendDefinition 与 ModelDefinition 元数据定义
    auto backend_def_opt = BackendRegistry::Instance().Find(spec.backend_type);
    if (!backend_def_opt) {
      if (diagnostic)
        *diagnostic = "Unknown backend type: " + spec.backend_type;
      return nullptr;
    }

    auto model_def_opt = ModelRegistry::Instance().Find(spec.model_type);
    if (!model_def_opt) {
      if (diagnostic) *diagnostic = "Unknown model type: " + spec.model_type;
      return nullptr;
    }

    // 在创建 Provider/加载模型产生副作用之前完成静态协议兼容性校验。
    const auto& declared_protocols = backend_def_opt->supported_protocols;
    if (std::find(declared_protocols.begin(), declared_protocols.end(),
                  model_def_opt->required_protocol) ==
        declared_protocols.end()) {
      if (diagnostic) {
        *diagnostic =
            "Backend " + spec.backend_type + " does not support model " +
            spec.model_type + " required protocol (" +
            ExecutionProtocolName(model_def_opt->required_protocol) + ")";
      }
      return nullptr;
    }

    // 2. 创建后端工厂 Provider
    auto backend =
        BackendRegistry::Instance().Create(spec.backend_type, diagnostic);
    if (!backend) {
      if (diagnostic && diagnostic->empty()) {
        *diagnostic = "Failed to instantiate backend: " + spec.backend_type;
      }
      return nullptr;
    }
    if (backend->BackendType() != spec.backend_type) {
      if (diagnostic) {
        *diagnostic = "Backend provider type mismatch: expected " +
                      spec.backend_type + ", got " + backend->BackendType();
      }
      return nullptr;
    }

    // 3. 加载后端会话
    BackendLoadSpec load_spec;
    load_spec.model_path = spec.model_path;
    load_spec.backend_config = spec.backend_config;
    load_spec.requested_protocol = model_def_opt->required_protocol;
    load_spec.execution_target = spec.execution_target;

    std::string backend_diag;
    auto session = backend->Load(load_spec, &backend_diag);
    if (!session) {
      if (diagnostic) {
        *diagnostic = "Backend (" + spec.backend_type +
                      ") failed to load model: " + spec.model_path;
        if (!backend_diag.empty()) {
          *diagnostic += " (diagnostic: " + backend_diag + ")";
        }
      }
      return nullptr;
    }

    // 4. 校验 Session 身份与支持协议集合
    if (session->BackendType() != spec.backend_type) {
      if (diagnostic) {
        *diagnostic = "Backend session type mismatch: expected " +
                      spec.backend_type + ", got " + session->BackendType();
      }
      return nullptr;
    }

    const auto& supported = backend_def_opt->supported_protocols;
    if (std::find(supported.begin(), supported.end(), session->Protocol()) ==
        supported.end()) {
      if (diagnostic) {
        *diagnostic = "Session protocol (" +
                      std::string(ExecutionProtocolName(session->Protocol())) +
                      ") not in backend supported protocols";
      }
      return nullptr;
    }

    const InferenceConcurrency session_concurrency = session->Concurrency();
    if (!IsConcurrencyCompatible(backend_def_opt->concurrency,
                                 session_concurrency)) {
      if (diagnostic) {
        *diagnostic =
            "Backend session concurrency (" +
            std::string(InferenceConcurrencyName(session_concurrency)) +
            ") is stricter than backend definition (" +
            std::string(
                InferenceConcurrencyName(backend_def_opt->concurrency)) +
            ")";
      }
      return nullptr;
    }

    // 5. 校验协议与模型要求匹配
    if (session->Protocol() != model_def_opt->required_protocol) {
      if (diagnostic) {
        *diagnostic = "Backend session protocol (" +
                      std::string(ExecutionProtocolName(session->Protocol())) +
                      ") does not match model required protocol (" +
                      std::string(ExecutionProtocolName(
                          model_def_opt->required_protocol)) +
                      ")";
      }
      return nullptr;
    }

    // 6. 校验批处理策略合法性
    BatchPolicy policy = session->GetBatchPolicy();
    if (policy.max_batch_size == 0 ||
        (policy.fixed_batch_size != 0 &&
         policy.fixed_batch_size != policy.max_batch_size)) {
      if (diagnostic) {
        *diagnostic = "Invalid session BatchPolicy (max=" +
                      std::to_string(policy.max_batch_size) +
                      ", fixed=" + std::to_string(policy.fixed_batch_size) +
                      ")";
      }
      return nullptr;
    }

    // 7. 推导 model_resource_root
    std::string model_resource_root;
    try {
      if (!spec.model_path.empty()) {
        std::filesystem::path p(spec.model_path);
        model_resource_root = p.parent_path().string();
      }
    } catch (...) {
      model_resource_root.clear();
    }

    // 8. 创建模型语义实例
    ModelCreateContext create_ctx;
    create_ctx.backend_session = session;
    create_ctx.model_resource_root = model_resource_root;
    create_ctx.model_config = spec.model_config;

    std::string model_diag;
    auto model = ModelRegistry::Instance().Create(spec.model_type, create_ctx,
                                                  &model_diag);
    if (!model) {
      if (diagnostic) {
        *diagnostic = "Failed to instantiate model: " + spec.model_type;
        if (!model_diag.empty()) {
          *diagnostic += " (diagnostic: " + model_diag + ")";
        }
      }
      return nullptr;
    }

    // 9. 校验模型身份、能力与并发模型一致性
    if (model->ModelType() != spec.model_type) {
      if (diagnostic) {
        *diagnostic = "Model identity mismatch: expected " + spec.model_type +
                      ", got " + model->ModelType();
      }
      return nullptr;
    }
    if (model->Capability() != model_def_opt->capability) {
      if (diagnostic) {
        *diagnostic = "Model capability mismatch: expected " +
                      model_def_opt->capability + ", got " +
                      model->Capability();
      }
      return nullptr;
    }
    if (model->Concurrency() != model_def_opt->concurrency) {
      if (diagnostic) {
        *diagnostic =
            "Model concurrency mismatch: expected " +
            std::string(InferenceConcurrencyName(model_def_opt->concurrency)) +
            ", got " +
            std::string(InferenceConcurrencyName(model->Concurrency()));
      }
      return nullptr;
    }

    return model;
  } catch (const std::exception& e) {
    (void)e;
    SetDiagnostic(diagnostic, "Exception in ModelRuntimeFactory::Create");
    return nullptr;
  } catch (...) {
    SetDiagnostic(diagnostic,
                  "Unknown exception in ModelRuntimeFactory::Create");
    return nullptr;
  }
}

}  // namespace llm_edgeflow
