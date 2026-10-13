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
#include "edgeflow/operator/interface.h"

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
    directory_ = directory;
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

TEST_F(IoParametersTest,
       ReadOnlyResolutionNormalizesDepthAndReturnsNativeHostContract) {
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
  operator_api::OperatorIoContract contract;
  ASSERT_EQ(operator_api::ResolveOperatorConfigIo(
                root_.c_str(), "conf/deploy.conf", &contract),
            0);
  ASSERT_EQ(contract.inputs.size(), 1u);
  ASSERT_EQ(contract.outputs.size(), 1u);
  EXPECT_EQ(contract.inputs[0].type, "keyword_in");
  EXPECT_EQ(contract.inputs[0].name, "keyword_match");
  EXPECT_EQ(contract.inputs[0].type_name, "CompanyOperatorKeywordInput");
  EXPECT_EQ(contract.inputs[0].service_type, kMockServiceKeywordMatch);
  EXPECT_EQ(contract.outputs[0].type, "json_out");
  EXPECT_EQ(contract.outputs[0].type_name, "CompanyOperatorOutput");
  EXPECT_EQ(contract.outputs[0].service_type, kMockServiceKeywordMatch);
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
  const auto outside = directory_ / "outside";
  std::filesystem::create_directory(outside);
  std::ofstream(outside / "deploy.conf") << R"({"pipe_path":"pipeline.json"})";
  std::ofstream(outside / "pipeline.json") << KeywordDeployment().dump();
  std::filesystem::create_symlink(outside / "deploy.conf",
                                  root_ / "linked.conf");
  for (const auto& file :
       {std::string("../outside/deploy.conf"),
        (config_ / "deploy.conf").string(), std::string("C:deploy.conf"),
        std::string("\\\\server\\deploy.conf"),
        std::string("conf/missing.conf"), std::string("conf"),
        std::string("linked.conf")}) {
    SCOPED_TRACE(file);
    ResolvedOperatorConfig rejected;
    EXPECT_EQ(Resolve(file, 2, &rejected), error::kDeploymentError);
    EXPECT_EQ(rejected.io_plan, nullptr);
  }
  std::filesystem::create_symlink(outside / "pipeline.json",
                                  config_ / "linked.json");
  for (const auto& pipe :
       {std::string("../pipeline.json"), std::string("linked.json"),
        (outside / "pipeline.json").string(), std::string("missing.json")}) {
    SCOPED_TRACE(pipe);
    WriteConfig(pipe);
    DeploymentDiagnostic diagnostic;
    ResolvedOperatorConfig rejected;
    EXPECT_EQ(Resolve("conf/deploy.conf", 2, &rejected, &diagnostic),
              error::kDeploymentError);
    EXPECT_EQ(rejected.io_plan, nullptr);
    EXPECT_EQ(diagnostic.path, "/pipe_path");
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
       PathContainmentRejectsSiblingDirectoriesSharingRootPrefixes) {
  const auto root_extra = directory_ / "root_extra";
  std::filesystem::create_directory(root_extra);
  std::ofstream(root_extra / "deploy.conf")
      << Json{{"pipe_path", "pipeline.json"}}.dump();
  std::ofstream(root_extra / "pipeline.json") << KeywordDeployment().dump();
  std::filesystem::create_symlink(root_extra / "deploy.conf",
                                  root_ / "linked.conf");

  DeploymentDiagnostic diagnostic;
  ResolvedOperatorConfig rejected_config;
  EXPECT_EQ(Resolve("linked.conf", 2, &rejected_config, &diagnostic),
            error::kDeploymentError);
  EXPECT_EQ(rejected_config.io_plan, nullptr);
  EXPECT_EQ(diagnostic.code, error::kDeploymentError);
  EXPECT_EQ(diagnostic.path, "/");
  EXPECT_NE(error_.find("symlink escapes model_path"), std::string::npos);

  const auto conf_extra = root_ / "conf_extra";
  std::filesystem::create_directory(conf_extra);
  std::ofstream(conf_extra / "pipeline.json") << KeywordDeployment().dump();
  WriteConfig((conf_extra / "pipeline.json").string());

  ResolvedOperatorConfig rejected_pipeline;
  EXPECT_EQ(Resolve("conf/deploy.conf", 2, &rejected_pipeline, &diagnostic),
            error::kDeploymentError);
  EXPECT_EQ(rejected_pipeline.io_plan, nullptr);
  EXPECT_EQ(diagnostic.code, error::kDeploymentError);
  EXPECT_EQ(diagnostic.path, "/pipe_path");
  EXPECT_NE(error_.find("pipe_path escapes config directory"),
            std::string::npos);
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
    const char* label;
    Json document;
    int code;
    const char* path;
  };
  std::vector<InvalidSelection> cases;
  auto unknown = KeywordDeployment();
  unknown["io"]["input"][0]["name"] = "not_keyword_match";
  cases.push_back(
      {"unknown pair", unknown, error::kUnknownConverter, "/io/input/0"});
  auto duplicate = KeywordDeployment();
  duplicate["io"]["input"].push_back(duplicate["io"]["input"][0]);
  cases.push_back({"duplicate selection", duplicate, error::kDuplicateIoEntry,
                   "/io/input/1"});
  auto port_collision = KeywordDeployment();
  port_collision["io"]["input"].push_back(
      {{"type", "entity_in"}, {"name", "entity_extract"}});
  cases.push_back({"duplicate producer", port_collision,
                   error::kDuplicatePortProducer, "/io/input/1"});
  auto unknown_egress = KeywordDeployment();
  unknown_egress["io"]["output"][0]["inputs"] = {{"wrong", "rules.matches"}};
  cases.push_back({"unknown egress", unknown_egress, error::kUnknownField,
                   "/io/output/0/inputs/wrong"});
  auto duplicate_output = KeywordDeployment();
  duplicate_output["io"]["output"].push_back(
      duplicate_output["io"]["output"][0]);
  cases.push_back({"duplicate output", duplicate_output,
                   error::kDuplicateIoEntry, "/io/output/1"});
  for (const auto& invalid : cases) {
    SCOPED_TRACE(invalid.label);
    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(PrepareDeploymentDocument(invalid.document, {}, &prepared,
                                           &diagnostic));
    EXPECT_TRUE(prepared.inputs.empty());
    EXPECT_TRUE(prepared.outputs.empty());
    EXPECT_EQ(diagnostic.code, invalid.code);
    EXPECT_EQ(diagnostic.path, invalid.path);
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
  source["io"]["output"].push_back(
      {{"type", "json_out"},
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
  // An individual 64 KiB output at requested depth 1024 also exceeds 64 MiB
  // once the native outer structure and string header are counted.
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(KeywordDeployment(65536),
                                                    config_.string(), &plan,
                                                    &error_, &diagnostic, 1024),
            error::kInvalidOutputAllocation);
  EXPECT_EQ(diagnostic.path, "/io/output/0");
}

}  // namespace
}  // namespace llm_edgeflow
