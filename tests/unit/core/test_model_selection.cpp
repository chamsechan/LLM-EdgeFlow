#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "adapter/shared_algorithm_runtime.h"
#include "core/pipeline_validator.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"
#include "tests/support/pipeline_test_utils.h"

namespace llm_edgeflow {
namespace {

nlohmann::json SelectedModel(const char* type, const char* name,
                             const char* backend) {
  return {{"type", type},
          {"name", name},
          {"file", "missing.fixture"},
          {"backend", {{"type", backend}}}};
}
nlohmann::json EmbeddingNode(const char* name, const char* model) {
  return {{"id", name},
          {"node_type", "TextEmbeddingNode"},
          {"config", {{"bind_model", model}}},
          {"inputs", {{"text", "source"}}},
          {"outputs", {{"embedding", std::string(name) + "_embedding"}}}};
}

const ValidationDiagnostic* FindCode(const ValidationReport& report,
                                     DiagnosticCode code) {
  for (const auto& diagnostic : report.diagnostics)
    if (diagnostic.code == code) return &diagnostic;
  return nullptr;
}
}  // namespace

TEST(ModelBackendDecouplingTest, ProductionSelectionsMatchCategoryAndBackend) {
  const struct {
    const char* type;
    const char* backend;
    const char* impl;
  } cases[] = {{"embedding", "onnxruntime", "bge_embedding"},
               {"embedding", "kite_llm", "generated_text_embedding"},
               {"rerank", "onnxruntime", "bge_reranker"},
               {"llm", "llama_cpp", "qwen_causal_lm"},
               {"llm", "kite_llm", "qwen_causal_lm"},
               {"ocr", "kite_llm", "vision_document"},
               {"asr", "whisper_cpp", "whisper_asr"}};
  size_t checked = 0;
  for (const auto& candidate : cases) {
    if (!BackendRegistry::Instance().Has(candidate.backend)) continue;
    SCOPED_TRACE(candidate.backend);
    const auto selected = ModelRegistry::Instance().FindImplementation(
        candidate.type, candidate.backend);
    ASSERT_EQ(selected.size(), 1U);
    EXPECT_EQ(selected.front().impl_name, candidate.impl);
    ++checked;
  }
  EXPECT_GT(checked, 0U);
  std::vector<std::string> errors;
  EXPECT_TRUE(
      ModelRegistry::Instance().Audit(BackendRegistry::Instance(), &errors))
      << (errors.empty() ? "" : errors.front());
}

TEST(ModelBackendDecouplingTest, ModelBackendSuggestionsStayWithinCategory) {
  for (const auto& backend : {"not_a_backend", "test_causal_lm_backend"}) {
    auto document = nlohmann::json{
        {"models", {SelectedModel("embedding", "encoder", backend)}},
        {"pipeline", {EmbeddingNode("embed", "encoder")}}};
    const auto result = PipelineValidator::ValidateAndPlan(
        document, MakeTestBoundary({{"source", "TextBatch"}}));
    const auto code = std::string(backend) == "not_a_backend"
                          ? DiagnosticCode::kUnknownBackend
                          : DiagnosticCode::kBackendProtocolMismatch;
    const auto* diagnostic = FindCode(result.report, code);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->path, "/models/0/backend/type");
    EXPECT_FALSE(diagnostic->suggestions.empty());
    for (const auto& suggestion : diagnostic->suggestions)
      EXPECT_FALSE(ModelRegistry::Instance()
                       .FindImplementation("embedding", suggestion)
                       .empty())
          << suggestion;
    EXPECT_EQ(FindCode(result.report, DiagnosticCode::kModelTypeMismatch),
              nullptr);
  }
}

TEST(ModelBackendDecouplingTest,
     UnusedModelsAndSharedCategoryBindingsAreExplicit) {
  auto document = nlohmann::json{
      {"models",
       {SelectedModel("embedding", "first", "test_tensor_backend"),
        SelectedModel("embedding", "second", "test_tensor_backend")}},
      {"pipeline",
       {EmbeddingNode("embed_first", "first"),
        EmbeddingNode("embed_second", "second")}}};
  const auto boundary = MakeTestBoundary({{"source", "TextBatch"}});
  auto result = PipelineValidator::ValidateAndPlan(document, boundary);
  ASSERT_TRUE(result.report.ok) << result.report.diagnostics.front().message;
  ASSERT_EQ(result.models.size(), 2U);
  EXPECT_EQ(result.models[0].model_name, "first");
  EXPECT_EQ(result.models[1].model_name, "second");
  EXPECT_EQ(result.models[0].impl_name, "test_biz_embedding");
  EXPECT_EQ(result.node_plans.at("embed_first").model_bindings[0].model_name,
            "first");
  EXPECT_EQ(result.node_plans.at("embed_second").model_bindings[0].model_name,
            "second");
  document["pipeline"].erase(1);
  result = PipelineValidator::ValidateAndPlan(document, boundary);
  const auto* unused = FindCode(result.report, DiagnosticCode::kUnusedModel);
  ASSERT_NE(unused, nullptr);
  EXPECT_EQ(unused->path, "/models/1/name");
  document["pipeline"][0]["node_type"] = "unregistered_node";
  result = PipelineValidator::ValidateAndPlan(document, boundary);
  EXPECT_NE(FindCode(result.report, DiagnosticCode::kUnknownNodeType), nullptr);
  EXPECT_EQ(FindCode(result.report, DiagnosticCode::kUnusedModel), nullptr);
}

TEST(ModelBackendDecouplingTest,
     FixtureBackendsApplyConfiguredFixedBatchPolicy) {
  for (const auto* name : {"test_tensor_backend", "test_causal_lm_backend"}) {
    SCOPED_TRACE(name);
    const auto definition = BackendRegistry::Instance().Find(name);
    ASSERT_TRUE(definition);
    std::string error;
    auto backend = BackendRegistry::Instance().Create(name, &error);
    ASSERT_NE(backend, nullptr) << error;
    for (int size : {3, 0}) {
      BackendLoadSpec spec(ExecutionProtocol::kFixture);
      spec.model_file = "missing.fixture";
      ASSERT_TRUE(definition->params.Parse({{"fixed_batch_size", size}},
                                           &spec.params, &error))
          << error;
      auto session = backend->Load(spec, &error);
      ASSERT_NE(session, nullptr) << error;
      EXPECT_EQ(session->Protocol(), ExecutionProtocol::kFixture);
      const auto policy = session->GetBatchPolicy();
      EXPECT_EQ(policy.fixed_batch_size, static_cast<size_t>(size));
      EXPECT_EQ(policy.max_batch_size,
                size                                         ? 3U
                : std::string(name) == "test_tensor_backend" ? 16U
                                                             : 1U);
    }
  }
}

TEST(ModelBackendDecouplingTest,
     FixtureOwnershipAndConflictsAreAuditedInInitAndCore) {
  EXPECT_EXIT(
      ([] {
        auto& backends = BackendRegistry::Instance();
        auto& models = ModelRegistry::Instance();
        bool passed = true;
        for (const auto* name :
             {"selection_first_backend", "selection_second_backend",
              "selection_third_backend"}) {
          BackendDefinition backend;
          backend.backend_type = name;
          backend.supported_protocols = {ExecutionProtocol::kFixture};
          passed &= backends.Register(backend, [] { return nullptr; });
        }
        ModelDefinition first;
        first.impl_name = "selection_first_model";
        first.model_type = "selection_fixture";
        first.required_protocol = ExecutionProtocol::kFixture;
        first.fixture_backends = {"selection_first_backend",
                                  "selection_second_backend"};
        passed &=
            models.Register(first, [](const auto&, auto*) { return nullptr; });
        auto second = first;
        second.impl_name = "selection_second_model";
        second.fixture_backends = {"selection_third_backend"};
        passed &=
            models.Register(second, [](const auto&, auto*) { return nullptr; });
        for (const auto* name :
             {"selection_first_backend", "selection_second_backend"}) {
          const auto selection =
              models.FindImplementation("selection_fixture", name);
          passed &= selection.size() == 1 &&
                    selection[0].impl_name == first.impl_name;
        }
        passed &= models
                      .FindImplementation("selection_fixture",
                                          "selection_third_backend")
                      .size() == 1;
        std::vector<std::string> errors;
        passed &= models.Audit(backends, &errors);
        // 一个新增生产协议实现与现有实现冲突，不能依赖 GlobalInit 才检测。
        if (backends.Has("onnxruntime")) {
          const auto existing = models.Find("bge_embedding");
          passed &= existing.has_value();
          if (existing) {
            auto duplicate = *existing;
            duplicate.impl_name = "conflicting_bge_implementation";
            passed &= models.Register(
                duplicate, [](const auto&, auto*) { return nullptr; });
            passed &= !models.Audit(backends, &errors);
            std::string init_error;
            passed &= SharedAlgorithmRuntime::GlobalInit(&init_error) != 0;
            passed &= init_error.find("bge_embedding") != std::string::npos &&
                      init_error.find(duplicate.impl_name) != std::string::npos;
            const auto result = PipelineValidator::ValidateAndPlan(
                {{"models",
                  {SelectedModel("embedding", "encoder", "onnxruntime")}},
                 {"pipeline", {EmbeddingNode("embed", "encoder")}}},
                MakeTestBoundary({{"source", "TextBatch"}}));
            const auto* conflict =
                FindCode(result.report, DiagnosticCode::kRegistryConflict);
            passed &=
                conflict && conflict->path == "/models/0/backend/type" &&
                conflict->message.find(duplicate.impl_name) !=
                    std::string::npos &&
                conflict->message.find("bge_embedding") != std::string::npos;
          }
        }
        std::exit(passed ? 0 : 1);
      }()),
      ::testing::ExitedWithCode(0), ".*");
}
}  // namespace llm_edgeflow
