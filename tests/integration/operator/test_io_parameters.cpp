#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "adapter/deployment_io_config.h"
#include "adapter/deployment_preparation.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/operator/operator_config_resolver.h"
#include "adapter/runtime_types.h"
#include "adapter/shared_algorithm_runtime.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {
namespace {
using Json = nlohmann::json;

Json KeywordDeployment(uint32_t capacity = 8191) {
  return {
      {"io",
       {{"input",
         Json::array({{{"type", "keyword_in"}, {"name", "keyword_match"}}})},
        {"output",
         Json::array({{{"type", "json_out"},
                       {"name", "keyword_match"},
                       {"params", {{"result_json_max_bytes", capacity}}},
                       {"inputs", {{"matches", "rules.matches"}}}}})}}},
      {"pipeline",
       Json::array({{{"type", "text_rule_match"},
                     {"name", "rules"},
                     {"inputs", {{"text", "input.sentence_text"}}},
                     {"params", {{"categories", {{"HIT", {"hello"}}}}}}}})}};
}

class IoParametersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char directory[] = "/tmp/edgeflow-deployment-XXXXXX";
    ASSERT_NE(mkdtemp(directory), nullptr);
    directory_ = std::filesystem::canonical(directory);
    root_ = directory_ / "root";
    config_ = root_ / "conf";
    std::filesystem::create_directories(config_ / "nested");
    WritePipeline(KeywordDeployment());
    WriteConfig("pipeline.json");
  }
  void TearDown() override {
    std::error_code error;
    if (!directory_.empty()) std::filesystem::remove_all(directory_, error);
    EXPECT_FALSE(error) << error.message();
  }
  void WritePipeline(const Json& document) {
    std::ofstream(config_ / "pipeline.json") << document.dump(2);
  }
  void WriteConfig(const std::string& path) {
    std::ofstream(config_ / "deploy.conf") << Json{{"pipe_path", path}}.dump();
  }
  int Resolve(const std::string& file, uint32_t depth,
              ResolvedOperatorConfig* out,
              DeploymentDiagnostic* diagnostic = nullptr) {
    return OperatorConfigResolver::Resolve(root_.c_str(), file.c_str(), out,
                                           &error_, depth, diagnostic);
  }
  std::filesystem::path directory_, root_, config_;
  std::string error_;
};

TEST_F(IoParametersTest, ReadOnlyResolutionNormalizesDepthAndOutputCapacity) {
  struct Depth {
    uint32_t requested, pool, process;
  };
  for (const auto depth : {Depth{0, 25, 25}, Depth{64, 64, 64},
                           Depth{65, 65, 64}, Depth{1024, 1024, 64}}) {
    SCOPED_TRACE(depth.requested);
    ResolvedOperatorConfig result;
    ASSERT_EQ(Resolve("conf/deploy.conf", depth.requested, &result), 0)
        << error_;
    EXPECT_EQ(result.effective_frame_depth, depth.pool);
    EXPECT_EQ(result.effective_process_batch_limit, depth.process);
    ASSERT_NE(result.io_plan, nullptr);
    EXPECT_EQ(result.io_plan->output_pool_depth, depth.pool);
    EXPECT_EQ(result.pipeline_path, config_ / "pipeline.json");
    ASSERT_EQ(result.io_plan->outputs.size(), 1u);
    EXPECT_EQ(result.io_plan->outputs[0].pool_spec.GetCapacity("result_json"),
              8191u);
  }
  ResolvedOperatorConfig invalid;
  EXPECT_EQ(Resolve("conf/deploy.conf", 1025, &invalid),
            error::kInvalidOutputAllocation);
  EXPECT_EQ(invalid.io_plan, nullptr);
  EXPECT_NE(error_.find("exceeds hard limit"), std::string::npos);
}

TEST_F(IoParametersTest, ResolutionReturnsSpecificCodeWithoutDiagnosticOutput) {
  std::ofstream(config_ / "deploy.conf") << "{invalid";
  ResolvedOperatorConfig result;
  DeploymentDiagnostic diagnostic;
  EXPECT_EQ(Resolve("conf/deploy.conf", 2, &result, &diagnostic),
            error::kJsonParse);
  EXPECT_EQ(diagnostic.code, error::kJsonParse);
  EXPECT_EQ(Resolve("conf/deploy.conf", 2, &result), error::kJsonParse);

  std::unique_ptr<ValidatedIoPlan> plan;
  EXPECT_EQ(IoPlanResolver::ResolveFromFile((config_ / "deploy.conf").string(),
                                            &plan, &error_),
            error::kJsonParse);
  EXPECT_EQ(IoPlanResolver::ResolveFromFile((config_ / "missing.conf").string(),
                                            &plan, &error_),
            error::kConfigFileOpen);
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(
                KeywordDeployment(65537), config_.string(), &plan, &error_),
            error::kConfigFieldRange);
}

TEST_F(IoParametersTest,
       ConfigAndPipelinePathsPermitContainedNormalizationButRejectEscape) {
  ResolvedOperatorConfig result;
  ASSERT_EQ(Resolve("conf/nested/../deploy.conf", 2, &result), 0) << error_;
  EXPECT_EQ(result.conf_path, config_ / "deploy.conf");
  WriteConfig("nested/../pipeline.json");
  ASSERT_EQ(Resolve("conf/deploy.conf", 2, &result), 0) << error_;
  EXPECT_EQ(result.pipeline_path, config_ / "pipeline.json");
  WriteConfig((config_ / "pipeline.json").string());
  ASSERT_EQ(Resolve("conf/deploy.conf", 2, &result), 0) << error_;
  const auto outside = directory_ / "root_extra";
  std::filesystem::create_directory(outside);
  std::ofstream(outside / "deploy.conf") << R"({"pipe_path":"pipeline.json"})";
  std::ofstream(outside / "pipeline.json") << KeywordDeployment().dump();
  std::filesystem::create_symlink(outside / "deploy.conf",
                                  root_ / "linked.conf");
  for (const auto& file :
       {std::string("../root_extra/deploy.conf"),
        (config_ / "deploy.conf").string(), std::string("C:deploy.conf"),
        std::string("\\\\server\\deploy.conf"),
        std::string("conf/missing.conf"), std::string("conf"),
        std::string("linked.conf")}) {
    SCOPED_TRACE(file);
    ResolvedOperatorConfig rejected;
    DeploymentDiagnostic diagnostic;
    EXPECT_EQ(Resolve(file, 2, &rejected, &diagnostic),
              error::kDeploymentError);
    EXPECT_EQ(rejected.io_plan, nullptr);
    if (file == "linked.conf") {
      EXPECT_EQ(diagnostic.code, error::kDeploymentError);
      EXPECT_EQ(diagnostic.path, "/");
      EXPECT_NE(error_.find("symlink escapes model_path"), std::string::npos);
    }
  }
  std::filesystem::create_symlink(outside / "pipeline.json",
                                  config_ / "linked.json");
  const auto conf_extra = root_ / "conf_extra";
  std::filesystem::create_directory(conf_extra);
  std::ofstream(conf_extra / "pipeline.json") << KeywordDeployment().dump();
  for (const auto& pipe :
       {std::string("../pipeline.json"), std::string("linked.json"),
        (outside / "pipeline.json").string(),
        (conf_extra / "pipeline.json").string(), std::string("missing.json")}) {
    SCOPED_TRACE(pipe);
    WriteConfig(pipe);
    DeploymentDiagnostic diagnostic;
    ResolvedOperatorConfig rejected;
    EXPECT_EQ(Resolve("conf/deploy.conf", 2, &rejected, &diagnostic),
              error::kDeploymentError);
    EXPECT_EQ(rejected.io_plan, nullptr);
    EXPECT_EQ(diagnostic.path, "/pipe_path");
    if (pipe == (conf_extra / "pipeline.json").string()) {
      EXPECT_EQ(diagnostic.code, error::kDeploymentError);
      EXPECT_NE(error_.find("pipe_path escapes config directory"),
                std::string::npos);
    }
  }
}

TEST_F(IoParametersTest,
       ConfigAndPipelineSymlinksWithinTheirRootsResolveToTargets) {
  std::filesystem::create_symlink(config_ / "deploy.conf",
                                  root_ / "linked.conf");
  std::filesystem::create_symlink(config_ / "pipeline.json",
                                  config_ / "linked.json");
  WriteConfig("linked.json");

  ResolvedOperatorConfig result;
  DeploymentDiagnostic diagnostic;
  ASSERT_EQ(Resolve("linked.conf", 2, &result, &diagnostic), 0) << error_;
  EXPECT_EQ(result.conf_path, config_ / "deploy.conf");
  EXPECT_EQ(result.pipeline_path, config_ / "pipeline.json");
  EXPECT_EQ(diagnostic.code, error::kOk);
}

TEST_F(IoParametersTest,
       DeploymentConfAcceptsOnlyPipePathAndDoesNotSearchForMissingPipelines) {
  DeploymentIoConfig config;
  DeploymentDiagnostic diagnostic;
  std::string error;
  EXPECT_FALSE(DeploymentIoConfig::Parse(
      {{"pipe_path", "pipeline.json"}, {"models", Json::array()}},
      config_.string(), &config, &error, &diagnostic));
  EXPECT_EQ(diagnostic.path, "/models");
  EXPECT_FALSE(DeploymentIoConfig::Parse({{"pipe_path", 7}}, config_.string(),
                                         &config, &error, &diagnostic));
  EXPECT_EQ(diagnostic.path, "/pipe_path");
  std::ofstream(root_ / "elsewhere.json") << KeywordDeployment().dump();
  EXPECT_FALSE(DeploymentIoConfig::Parse({{"pipe_path", "elsewhere.json"}},
                                         config_.string(), &config, &error,
                                         &diagnostic));
  EXPECT_NE(error.find("does not exist"), std::string::npos);
}

TEST_F(IoParametersTest,
       ExactSelectionsAndUniqueIngressPortsFailWithOwnedDiagnosticPaths) {
  struct InvalidSelection {
    const char* path;
    Json value;
    int code;
    const char* diagnostic_path = nullptr;
  };
  const InvalidSelection cases[] = {
      {"/io/input/0/name", "not_keyword_match", error::kUnknownConverter,
       "/io/input/0"},
      {"/io/input",
       Json::array({{{"type", "entity_in"}, {"name", "entity_extract"}},
                    {{"type", "entity_in"}, {"name", "translate"}}}),
       error::kDuplicateIoEntry, "/io/input/1"},
      {"/io/input/-",
       {{"type", "entity_in"}, {"name", "entity_extract"}},
       error::kDuplicatePortProducer,
       "/io/input/1"},
      {"/io/output/0/inputs",
       {{"wrong", "rules.matches"}},
       error::kUnknownField,
       "/io/output/0/inputs/wrong"},
      {"/io/output/-",
       {{"type", "json_out"},
        {"name", "translate"},
        {"inputs", {{"translation", "rules.matches"}}}},
       error::kDuplicateIoEntry,
       "/io/output/1"},
      {"/io/a~1b~0c", true, error::kDeploymentError},
      {"/io/", true, error::kDeploymentError},
      {"/io/output/0/a~1b~0c", true, error::kDeploymentError},
      {"/io/output/0/", true, error::kDeploymentError}};
  for (const auto& invalid : cases) {
    SCOPED_TRACE(invalid.path);
    auto document = KeywordDeployment();
    document[Json::json_pointer(invalid.path)] = invalid.value;
    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(
        PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
    EXPECT_TRUE(prepared.inputs.empty());
    EXPECT_TRUE(prepared.outputs.empty());
    EXPECT_EQ(diagnostic.code, invalid.code);
    EXPECT_EQ(diagnostic.path,
              invalid.diagnostic_path ? invalid.diagnostic_path : invalid.path);
  }
}

TEST_F(IoParametersTest,
       UnknownConverterTypesReportExactSelectionWithoutGuesses) {
  for (const auto* direction : {"input", "output"}) {
    SCOPED_TRACE(direction);
    auto document = KeywordDeployment();
    const std::string type =
        std::string(direction) == "input" ? "keyword_i" : "json_ou";
    document["io"][direction][0]["type"] = type;
    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(
        PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
    EXPECT_EQ(diagnostic.code, error::kUnknownConverter);
    EXPECT_EQ(diagnostic.path, std::string("/io/") + direction + "/0");
    EXPECT_EQ(diagnostic.message,
              "Unknown converter: " + type + "/keyword_match");
    EXPECT_TRUE(prepared.inputs.empty());
    EXPECT_TRUE(prepared.outputs.empty());
  }
}

TEST_F(
    IoParametersTest,
    OutputCapacityInheritsNativeHardLimitAndPreparedDepthCannotChangeAtCreate) {
  std::unique_ptr<ValidatedIoPlan> plan;
  DeploymentDiagnostic diagnostic;
  ASSERT_EQ(IoPlanResolver::ResolveFromPipelineJson(KeywordDeployment(65536),
                                                    config_.string(), &plan,
                                                    &error_, &diagnostic, 2),
            0)
      << error_;
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(plan->outputs[0].pool_spec.GetCapacity("result_json"), 65536u);
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(KeywordDeployment(65537),
                                                    config_.string(), &plan,
                                                    &error_, &diagnostic, 2),
            error::kConfigFieldRange);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diagnostic.code, error::kConfigFieldRange);
  EXPECT_EQ(diagnostic.path, "/io/output/0/params/result_json_max_bytes");
  ASSERT_EQ(IoPlanResolver::ResolveFromPipelineJson(KeywordDeployment(),
                                                    config_.string(), &plan,
                                                    &error_, &diagnostic, 2),
            0)
      << error_;
  RuntimeCreateOptions options;
  options.output_pool_depth = 3;
  std::unique_ptr<SharedAlgorithmRuntime> runtime;
  EXPECT_NE(SharedAlgorithmRuntime::CreateFromIoPlan(std::move(plan), options,
                                                     &runtime, &error_),
            0);
  EXPECT_EQ(runtime, nullptr);
  EXPECT_NE(error_.find("depth"), std::string::npos);
}

TEST_F(IoParametersTest,
       RequestedDepthAndAggregateOutputSlotsConsumeOneHandleBudget) {
  std::unique_ptr<ValidatedIoPlan> plan;
  DeploymentDiagnostic diagnostic;
  auto source = KeywordDeployment(40000);
  ASSERT_EQ(IoPlanResolver::ResolveFromPipelineJson(
                source, config_.string(), &plan, &error_, &diagnostic, 1024),
            0)
      << error_;
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(plan->output_pool_depth, 1024u);
  // 每方向 type 唯一，聚合预算用测试注入的第二种输出 type 覆盖。
  test_support::RegistryTestAccess::ScopedValueTypeState value_types;
  auto& converters = IoConverterRegistry::Instance();
  const auto restore = converters.AllOutputConverters();
  const auto inputs = converters.AllInputConverters();
  struct RestoreConverters {
    const std::vector<InputConverterDefinition>& inputs;
    const std::vector<OutputConverterDefinition>& outputs;
    ~RestoreConverters() {
      test_support::RegistryTestAccess::ResetConverters();
      for (const auto& def : inputs)
        IoConverterRegistry::Instance().RegisterInputConverter(def);
      for (const auto& def : outputs)
        IoConverterRegistry::Instance().RegisterOutputConverter(def);
    }
  } restore_converters{inputs, restore};
  auto aux_binding =
      OperatorValueTypeRegistry::Instance().CopyBindingBySuffix("json_out");
  ASSERT_TRUE(aux_binding);
  aux_binding->canonical_suffix = "aux_json";
  test_support::RegistryTestAccess::SetValueBinding(*aux_binding);
  auto aux = *converters.FindOutputConverter("json_out", "translate");
  aux.type = "aux_json";
  ASSERT_TRUE(converters.RegisterOutputConverter(aux));
  source["io"]["output"].push_back(
      {{"type", "aux_json"},
       {"name", "translate"},
       {"params", {{"result_json_max_bytes", 40000}}},
       {"inputs", {{"translation", "render.text"}}}});
  source["pipeline"].push_back(
      {{"type", "text_template"},
       {"name", "render"},
       {"inputs", {{"primary", "input.sentence_text"}}}});
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(
                source, config_.string(), &plan, &error_, &diagnostic, 1024),
            error::kInvalidOutputAllocation);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diagnostic.code, error::kInvalidOutputAllocation);
  EXPECT_EQ(diagnostic.path, "/io/output");
  EXPECT_NE(error_.find("per-handle payload budget"), std::string::npos);
  ASSERT_EQ(IoPlanResolver::ResolveFromPipelineJson(
                source, config_.string(), &plan, &error_, &diagnostic, 64),
            0)
      << error_;
  EXPECT_EQ(plan->outputs.size(), 2u);
  // 单个 64 KiB 输出在请求深度 1024 下，计入原生外层结构和字符串头后
  // 同样超过 64 MiB。
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(KeywordDeployment(65536),
                                                    config_.string(), &plan,
                                                    &error_, &diagnostic, 1024),
            error::kInvalidOutputAllocation);
  EXPECT_EQ(diagnostic.path, "/io/output/0");
}

}  // namespace
}  // namespace llm_edgeflow
