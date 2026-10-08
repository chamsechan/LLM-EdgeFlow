#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "contracts/registry_conflicts.h"
#include "edgeflow/log.h"
#include "engine/backend_interface.h"
#include "engine/inference_definition.h"
#include "engine/model_interface.h"

namespace llm_edgeflow {

/**
 * @brief 模型创建上下文参数
 */
struct ModelCreateContext {
  std::shared_ptr<IBackendSession> backend_session;
  std::string model_resource_root;
  // 本模型名下已校验的参数，由 ModelRuntimeFactory 解析一次。
  std::shared_ptr<const ParameterValues> params;

  // 取回声明时使用的参数结构体；未提供参数或类型不符时抛出 std::logic_error。
  template <typename P>
  const P& Params() const {
    if (!params) {
      throw std::logic_error("ModelCreateContext has no parameters");
    }
    return params->Get<P>();
  }
};

/**
 * @brief 模型语义实现注册表 (ModelRegistry)
 */
class ModelRegistry {
 public:
  using Creator = std::function<std::shared_ptr<IModel>(
      const ModelCreateContext&, std::string* diagnostic)>;

  static ModelRegistry& Instance();

  bool Register(const ModelDefinition& definition, Creator creator) noexcept;

  std::optional<ModelDefinition> Find(
      const std::string& model_type) const noexcept;

  std::shared_ptr<IModel> Create(
      const std::string& model_type, const ModelCreateContext& context,
      std::string* diagnostic = nullptr) const noexcept;

  bool Has(const std::string& model_type) const noexcept;

  std::vector<std::string> ListTypes() const;

  std::vector<ModelDefinition> ListDefinitions() const;

  bool HasConflict() const noexcept;

  std::vector<std::string> GetConflictErrors() const;

 private:
  struct Entry {
    ModelDefinition definition;
    Creator creator;
  };

  ModelRegistry() = default;
  void RecordConflict(std::string_view error) noexcept;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  RegistryConflicts conflicts_;
};

#define REGISTER_MODEL_WITH_DEFINITION(ModelClass, ...)                      \
  static bool _registered_model_##ModelClass = []() noexcept {               \
    try {                                                                    \
      const auto definition = (__VA_ARGS__);                                 \
      return ::llm_edgeflow::ModelRegistry::Instance().Register(             \
          definition,                                                        \
          [](const ::llm_edgeflow::ModelCreateContext& ctx,                  \
             std::string* diag) -> std::shared_ptr<::llm_edgeflow::IModel> { \
            return ModelClass::Create(ctx, diag);                            \
          });                                                                \
    } catch (const std::exception& e) {                                      \
      ALG_LOG_ERROR("[ModelRegistry] Failed to register %s: %s\n",           \
                    #ModelClass, e.what());                                  \
      return false;                                                          \
    } catch (...) {                                                          \
      ALG_LOG_ERROR("[ModelRegistry] Failed to register %s\n", #ModelClass); \
      return false;                                                          \
    }                                                                        \
  }()

}  // namespace llm_edgeflow
