#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {

ModelDefinition ModelDefinitionFor(std::string name) {
  ModelDefinition definition;
  definition.impl_name = std::move(name);
  definition.model_type = "registry_probe";
  definition.description = "literal model";
  definition.required_protocol = ExecutionProtocol::kFixture;
  definition.fixture_backends = {"registry_backend"};
  return definition;
}
BackendDefinition BackendDefinitionFor(std::string name) {
  BackendDefinition definition;
  definition.backend_type = std::move(name);
  definition.description = "literal backend";
  definition.supported_protocols = {ExecutionProtocol::kFixture};
  return definition;
}
auto NullModel() {
  return [](const ModelCreateContext&,
            std::string*) -> std::shared_ptr<IModel> { return nullptr; };
}
auto NullBackend() {
  return []() -> std::unique_ptr<IInferenceBackend> { return nullptr; };
}
void Check(bool condition) {
  if (!condition) std::_Exit(1);
}

TEST(ModelExecutionRegistryTest, DefinitionsAreIndependentSortedSnapshots) {
  auto& models = ModelRegistry::Instance();
  auto& backends = BackendRegistry::Instance();
  auto model = ModelDefinitionFor("registry_zeta");
  model.model_type = "registry_snapshot";
  auto backend = BackendDefinitionFor("registry_zeta");
  ASSERT_TRUE(models.Register(model, NullModel()));
  ASSERT_TRUE(backends.Register(backend, NullBackend()));
  model.description = "changed caller";
  backend.description = "changed caller";
  auto found_model = models.Find("registry_zeta");
  auto found_backend = backends.Find("registry_zeta");
  ASSERT_TRUE(found_model);
  ASSERT_TRUE(found_backend);
  EXPECT_EQ(found_model->description, "literal model");
  EXPECT_EQ(found_backend->description, "literal backend");
  found_model->fixture_backends.clear();
  found_backend->supported_protocols.clear();
  EXPECT_EQ(models.Find("registry_zeta")->fixture_backends,
            (std::vector<std::string>{"registry_backend"}));
  EXPECT_EQ(backends.Find("registry_zeta")->supported_protocols,
            (std::vector<ExecutionProtocol>{ExecutionProtocol::kFixture}));
  const auto model_definitions = models.ListDefinitions();
  const auto backend_definitions = backends.ListDefinitions();
  EXPECT_TRUE(std::is_sorted(model_definitions.begin(), model_definitions.end(),
                             [](const auto& lhs, const auto& rhs) {
                               return lhs.impl_name < rhs.impl_name;
                             }));
  EXPECT_TRUE(std::is_sorted(backend_definitions.begin(),
                             backend_definitions.end(),
                             [](const auto& lhs, const auto& rhs) {
                               return lhs.backend_type < rhs.backend_type;
                             }));
}

TEST(ModelExecutionRegistryTest,
     InvalidDeclarationsNeverInstallAndLeaveStickyErrors) {
  // Each invalid registration is isolated because conflicts intentionally
  // persist.
  for (int defect = 0; defect < 9; ++defect) {
    SCOPED_TRACE(defect);
    EXPECT_EXIT(
        ([&] {
          auto definition = ModelDefinitionFor("invalid_model");
          auto creator = ModelRegistry::Creator(NullModel());
          switch (defect) {
            case 0:
              definition.impl_name.clear();
              break;
            case 1:
              definition.model_type.clear();
              break;
            case 2:
              definition.required_protocol = static_cast<ExecutionProtocol>(99);
              break;
            case 3:
              definition.concurrency = static_cast<InferenceConcurrency>(99);
              break;
            case 4:
              creator = {};
              break;
            case 5:
              definition.fixture_backends.clear();
              break;
            case 6:
              definition.required_protocol = ExecutionProtocol::kTensorGraph;
              break;
            case 7:
              definition.fixture_backends = {""};
              break;
            case 8:
              definition.fixture_backends = {"registry_backend",
                                             "registry_backend"};
              break;
          }
          auto& registry = ModelRegistry::Instance();
          Check(!registry.Register(definition, creator));
          Check(!registry.Find("invalid_model").has_value());
          Check(registry.HasConflict());
          Check(!registry.GetConflictErrors().empty());
          std::_Exit(0);
        }()),
        ::testing::ExitedWithCode(0), "");
  }
  for (int defect = 0; defect < 6; ++defect) {
    SCOPED_TRACE(defect);
    EXPECT_EXIT(([&] {
                  auto definition = BackendDefinitionFor("invalid_backend");
                  auto creator = BackendRegistry::Creator(NullBackend());
                  switch (defect) {
                    case 0:
                      definition.backend_type.clear();
                      break;
                    case 1:
                      definition.supported_protocols.clear();
                      break;
                    case 2:
                      definition.concurrency =
                          static_cast<InferenceConcurrency>(99);
                      break;
                    case 3:
                      creator = {};
                      break;
                    case 4:
                      definition.supported_protocols = {
                          static_cast<ExecutionProtocol>(99)};
                      break;
                    case 5:
                      definition.supported_protocols.push_back(
                          ExecutionProtocol::kFixture);
                      break;
                  }
                  auto& registry = BackendRegistry::Instance();
                  Check(!registry.Register(definition, creator));
                  Check(!registry.Find("invalid_backend").has_value());
                  Check(registry.HasConflict());
                  Check(!registry.GetConflictErrors().empty());
                  std::_Exit(0);
                }()),
                ::testing::ExitedWithCode(0), "");
  }
}

TEST(ModelExecutionRegistryTest,
     DuplicateKeepsTheOriginalDeclarationAndCreator) {
  EXPECT_EXIT(
      ([&] {
        auto& models = ModelRegistry::Instance();
        auto& backends = BackendRegistry::Instance();
        int model_calls = 0, backend_calls = 0;
        auto model = ModelDefinitionFor("duplicate_model");
        auto backend = BackendDefinitionFor("duplicate_backend");
        Check(models.Register(model,
                              [&](const ModelCreateContext&,
                                  std::string*) -> std::shared_ptr<IModel> {
                                ++model_calls;
                                return nullptr;
                              }));
        Check(backends.Register(backend,
                                [&]() -> std::unique_ptr<IInferenceBackend> {
                                  ++backend_calls;
                                  return nullptr;
                                }));
        model.description = "second model";
        backend.description = "second backend";
        Check(!models.Register(model, NullModel()));
        Check(!backends.Register(backend, NullBackend()));
        Check(models.Find("duplicate_model")->description == "literal model");
        Check(backends.Find("duplicate_backend")->description ==
              "literal backend");
        models.Create("duplicate_model", {});
        backends.Create("duplicate_backend");
        Check(model_calls == 1 && backend_calls == 1);
        Check(models.HasConflict() && backends.HasConflict());
        std::_Exit(0);
      }()),
      ::testing::ExitedWithCode(0), "");
}

TEST(ModelExecutionRegistryTest,
     SelectionUsesProtocolAndFixtureWhitelistAndAuditRejectsAmbiguity) {
  EXPECT_EXIT(
      ([&] {
        auto& models = ModelRegistry::Instance();
        auto& backends = BackendRegistry::Instance();
        Check(backends.Register(BackendDefinitionFor("registry_backend"),
                                NullBackend()));
        Check(backends.Register(BackendDefinitionFor("registry_unlisted"),
                                NullBackend()));
        auto model = ModelDefinitionFor("first_registry_impl");
        Check(models.Register(model, NullModel()));
        const auto compatible =
            models.FindImplementation("registry_probe", "registry_backend");
        Check(compatible.size() == 1 &&
              compatible.front().impl_name == "first_registry_impl");
        Check(models.FindImplementation("registry_probe", "registry_unlisted")
                  .empty());
        Check(models.FindImplementation("missing", "registry_backend").empty());
        Check(models.FindImplementation("registry_probe", "missing").empty());
        model.impl_name = "second_registry_impl";
        Check(models.Register(model, NullModel()));
        Check(models.FindImplementation("registry_probe", "registry_backend")
                  .size() == 2);
        std::vector<std::string> errors;
        Check(!models.Audit(backends, &errors));
        bool identified = false;
        for (const auto& error : errors)
          identified |= error.find("registry_probe") != std::string::npos &&
                        error.find("registry_backend") != std::string::npos;
        Check(identified);
        std::_Exit(0);
      }()),
      ::testing::ExitedWithCode(0), "");
}

struct CopyFailureCreator {
  std::shared_ptr<int> failure;
  explicit CopyFailureCreator(std::shared_ptr<int> value)
      : failure(std::move(value)) {}
  CopyFailureCreator(const CopyFailureCreator& other) : failure(other.failure) {
    if (*failure == 1) throw std::runtime_error("creator copy failure");
    if (*failure == 2) throw 42;
  }
  std::shared_ptr<IModel> operator()(const ModelCreateContext&,
                                     std::string*) const {
    return nullptr;
  }
  std::unique_ptr<IInferenceBackend> operator()() const { return nullptr; }
};

TEST(ModelExecutionRegistryTest, CreatorCopyFailuresLeaveRegistrationsUsable) {
  EXPECT_EXIT(([&] {
                auto failure = std::make_shared<int>(0);
                auto& models = ModelRegistry::Instance();
                auto& backends = BackendRegistry::Instance();
                Check(models.Register(ModelDefinitionFor("copy_model"),
                                      CopyFailureCreator(failure)));
                Check(backends.Register(BackendDefinitionFor("copy_backend"),
                                        CopyFailureCreator(failure)));
                for (int mode : {1, 2}) {
                  *failure = mode;
                  std::string model_error, backend_error;
                  Check(!models.Create("copy_model", {}, &model_error));
                  Check(!backends.Create("copy_backend", &backend_error));
                  const auto expected =
                      mode == 1 ? "creator copy failure" : "Unknown exception";
                  Check(model_error.find(expected) != std::string::npos);
                  Check(backend_error.find(expected) != std::string::npos);
                  Check(!models.HasConflict() && !backends.HasConflict());
                  Check(models.Find("copy_model").has_value());
                  Check(backends.Find("copy_backend").has_value());
                }
                *failure = 0;
                std::string model_error, backend_error;
                Check(!models.Create("copy_model", {}, &model_error));
                Check(!backends.Create("copy_backend", &backend_error));
                Check(model_error.empty() && backend_error.empty());
                std::_Exit(0);
              }()),
              ::testing::ExitedWithCode(0), "");
}

TEST(ModelExecutionRegistryTest,
     CreatorInvocationCanReenterAndExceptionsBecomeDiagnostics) {
  EXPECT_EXIT(([&] {
                auto& models = ModelRegistry::Instance();
                auto& backends = BackendRegistry::Instance();
                Check(models.Register(
                    ModelDefinitionFor("reenter_model"),
                    [&](const ModelCreateContext&,
                        std::string*) -> std::shared_ptr<IModel> {
                      Check(models.Find("reenter_model").has_value());
                      throw std::runtime_error("model callback exception");
                    }));
                Check(backends.Register(
                    BackendDefinitionFor("reenter_backend"),
                    [&]() -> std::unique_ptr<IInferenceBackend> {
                      Check(backends.Find("reenter_backend").has_value());
                      throw 31;
                    }));
                auto result = std::async(std::launch::async, [&] {
                  std::string model_error, backend_error;
                  Check(!models.Create("reenter_model", {}, &model_error));
                  Check(!backends.Create("reenter_backend", &backend_error));
                  Check(model_error.find("model callback exception") !=
                        std::string::npos);
                  Check(!backend_error.empty());
                });
                Check(result.wait_for(std::chrono::seconds(2)) ==
                      std::future_status::ready);
                result.get();
                std::_Exit(0);
              }()),
              ::testing::ExitedWithCode(0), "");
}

}  // namespace
}  // namespace llm_edgeflow
