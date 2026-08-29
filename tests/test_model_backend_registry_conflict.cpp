#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "core/pipeline_catalog.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace alg_framework {
namespace {

ModelRegistry::Creator NullModelCreator() {
  return [](const ModelCreateContext&, std::string*) {
    return std::shared_ptr<IModel>{};
  };
}

BackendRegistry::Creator NullBackendCreator() {
  return []() { return std::unique_ptr<IInferenceBackend>{}; };
}

ModelDefinition ValidModelDefinition(std::string model_type) {
  ModelDefinition definition;
  definition.model_type = std::move(model_type);
  definition.capability = "embedding";
  definition.description = "original";
  definition.required_protocol = ExecutionProtocol::kTensorGraph;
  definition.concurrency = InferenceConcurrency::kConcurrent;
  return definition;
}

BackendDefinition ValidBackendDefinition(std::string backend_type) {
  BackendDefinition definition;
  definition.backend_type = std::move(backend_type);
  definition.description = "original";
  definition.supported_protocols = {ExecutionProtocol::kTensorGraph};
  definition.concurrency = InferenceConcurrency::kConcurrent;
  return definition;
}

TEST(ModelBackendRegistryConflictTest, DefinitionValidationIsFailClosed) {
  auto& model_registry = ModelRegistry::Instance();
  auto& backend_registry = BackendRegistry::Instance();

  auto model = ValidModelDefinition("");
  EXPECT_FALSE(model_registry.Register(model, NullModelCreator()));

  model = ValidModelDefinition("empty_capability_model");
  model.capability.clear();
  EXPECT_FALSE(model_registry.Register(model, NullModelCreator()));

  model = ValidModelDefinition("null_model_creator");
  EXPECT_FALSE(model_registry.Register(model, {}));

  model = ValidModelDefinition("invalid_model_protocol");
  model.required_protocol = static_cast<ExecutionProtocol>(999);
  EXPECT_FALSE(model_registry.Register(model, NullModelCreator()));

  model = ValidModelDefinition("invalid_model_concurrency");
  model.concurrency = static_cast<InferenceConcurrency>(999);
  EXPECT_FALSE(model_registry.Register(model, NullModelCreator()));

  model = ValidModelDefinition("invalid_model_schema");
  model.config_fields = {ConfigFieldDefinition(
      "threads", ConfigValueKind::kInteger, false, 4, 10, 2)};
  EXPECT_FALSE(model_registry.Register(model, NullModelCreator()));

  model = ValidModelDefinition("invalid_model_kind");
  model.config_fields = {
      ConfigFieldDefinition("value", static_cast<ConfigValueKind>(999), false)};
  EXPECT_FALSE(model_registry.Register(model, NullModelCreator()));

  auto backend = ValidBackendDefinition("");
  EXPECT_FALSE(backend_registry.Register(backend, NullBackendCreator()));

  backend = ValidBackendDefinition("empty_protocol_backend");
  backend.supported_protocols.clear();
  EXPECT_FALSE(backend_registry.Register(backend, NullBackendCreator()));

  backend = ValidBackendDefinition("null_backend_creator");
  EXPECT_FALSE(backend_registry.Register(backend, {}));

  backend = ValidBackendDefinition("invalid_backend_protocol");
  backend.supported_protocols = {static_cast<ExecutionProtocol>(999)};
  EXPECT_FALSE(backend_registry.Register(backend, NullBackendCreator()));

  backend = ValidBackendDefinition("duplicate_backend_protocol");
  backend.supported_protocols = {ExecutionProtocol::kTensorGraph,
                                 ExecutionProtocol::kTensorGraph};
  EXPECT_FALSE(backend_registry.Register(backend, NullBackendCreator()));

  backend = ValidBackendDefinition("invalid_backend_concurrency");
  backend.concurrency = static_cast<InferenceConcurrency>(999);
  EXPECT_FALSE(backend_registry.Register(backend, NullBackendCreator()));

  auto original_model = ValidModelDefinition("atomic_model");
  ASSERT_TRUE(model_registry.Register(original_model, NullModelCreator()));
  auto duplicate_model = original_model;
  duplicate_model.description = "duplicate";
  EXPECT_FALSE(model_registry.Register(duplicate_model, NullModelCreator()));
  const auto stored_model = model_registry.Find(original_model.model_type);
  ASSERT_TRUE(stored_model.has_value());
  EXPECT_EQ(stored_model->description, "original");

  auto original_backend = ValidBackendDefinition("atomic_backend");
  ASSERT_TRUE(
      backend_registry.Register(original_backend, NullBackendCreator()));
  auto duplicate_backend = original_backend;
  duplicate_backend.description = "duplicate";
  EXPECT_FALSE(
      backend_registry.Register(duplicate_backend, NullBackendCreator()));
  const auto stored_backend =
      backend_registry.Find(original_backend.backend_type);
  ASSERT_TRUE(stored_backend.has_value());
  EXPECT_EQ(stored_backend->description, "original");

  PipelineCatalog::ClearForTesting();
  EXPECT_TRUE(model_registry.Has(original_model.model_type));
  EXPECT_TRUE(backend_registry.Has(original_backend.backend_type));

  EXPECT_TRUE(model_registry.HasConflict());
  EXPECT_TRUE(backend_registry.HasConflict());
  EXPECT_FALSE(model_registry.GetConflictErrors().empty());
  EXPECT_FALSE(backend_registry.GetConflictErrors().empty());
}

}  // namespace
}  // namespace alg_framework
