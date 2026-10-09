#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapter/io_binding_registry.h"
#include "adapter/io_converter_registry.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"
#include "demo/common/demo_options.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"
#include "dev_support/node_authoring/legacy_node_base.h"
#include "edgeflow/log.h"
#include "edgeflow/operator/interface.h"
#include "engine/backend_registry.h"
#include "nlohmann/json.hpp"
#include "nodes/node_base.h"
#include "tests/support/control_test_utils.h"
#include "tests/support/registry_test_access.h"

using namespace alg_demo;
using namespace llm_edgeflow::operator_api;

namespace llm_edgeflow::test {
namespace {

class TestDemoStatusNode final : public LegacyNodeBase {
 public:
  inline static constexpr char kNodeType[] = "TestDemoStatusNode";
  TestDemoStatusNode()
      : LegacyNodeBase(kNodeType), input_("text"), output_("matches") {}

 protected:
  bool InitNode(const NodeInitContext& init, const nlohmann::json&,
                SessionContext&) override {
    BindPort(init, input_);
    BindPort(init, output_);
    return true;
  }
  int ProcessNode(AlgContext& context) override {
    const auto* input = input_.Require(context, -9001);
    if (!input) return -9001;
    RuleMatchBatch results;
    for (const auto& item : *input) {
      RuleMatchItem result;
      result.status_code = item.data == "fail" ? -42 : 0;
      results.emplace_back(item.req_id, item.sub_id, std::move(result));
    }
    output_.Set(context, std::move(results));
    return 0;
  }

 private:
  BoundInput<TextBatch> input_;
  BoundOutput<RuleMatchBatch> output_;
};

NodeDefinition DemoStatusDefinition() {
  NodeDefinition definition;
  definition.node_type = TestDemoStatusNode::kNodeType;
  definition.category = "test";
  definition.description = "Mixed per-sample status fixture for Demo output";
  definition.inputs = {
      RequiredInputPort("text", BlackboardKey<TextBatch>{"text", "TextBatch"},
                        "1:1", "preserve", "request")};
  definition.outputs = {OutputPort(
      "matches", BlackboardKey<RuleMatchBatch>{"matches", "RuleMatchBatch"},
      "1:1", "preserve", "request")};
  return definition;
}

REGISTER_NODE_WITH_DEFINITION(TestDemoStatusNode, DemoStatusDefinition());

// 用两个真实 KeywordOutput 槽验证 runner 合并，复用生产编码与展示。
int EncodeDemoMultiOutput(AlgContext* context,
                          const OutputEncodeOptions& options,
                          ExternalOutputBatchView* destination,
                          size_t* written_count, AdapterStatus* status) {
  if (!destination) return -1;
  const auto* converter =
      IoConverterRegistry::Instance().FindOutputConverter("keyword.result");
  if (!converter) return -1;
  for (const char* slot : {"primary", "secondary"}) {
    ExternalOutputBatchView view;
    view.count = destination->count;
    view.leased_slots["keyword_out"] = destination->leased_slots.at(slot);
    view.slot_types["keyword_out"] = destination->slot_types.at(slot);
    view.pool_specs["keyword_out"] = destination->pool_specs.at(slot);
    const int ret =
        converter->encode_fn(context, options, &view, written_count, status);
    if (ret != 0) return ret;
    if (std::string(slot) == "secondary") {
      for (size_t i = 0; i < *written_count; ++i) {
        auto* output =
            view.GetSlot<CompanyOperatorKeywordOutput>("keyword_out", i);
        output->status_code = output->is_hit ? 0 : -17;
        output->is_hit = !output->is_hit;
      }
    }
  }
  return 0;
}

bool RegisterDemoIoFixtures() {
  const auto production_biz = PipelineCatalog::FindBiz("keyword_match");
  const auto* production_output =
      IoConverterRegistry::Instance().FindOutputConverter("keyword.result");
  const auto* production_input =
      IoConverterRegistry::Instance().FindInputConverter("keyword.plain");
  if (!production_biz || !production_output || !production_input) return false;
  if (!IoBindingRegistry::Instance().FindBinding("test_demo_multi_output")) {
    auto biz = *production_biz;
    biz.biz_name = "test_demo_multi_output";
    if (!PipelineCatalog::RegisterBizDefinition(biz)) return false;
    auto output = *production_output;
    output.converter_id = "test_demo_multi_output";
    output.external_slots = {production_output->external_slots[0],
                             production_output->external_slots[0]};
    output.external_slots[0].slot_name = "primary";
    output.external_slots[0].key_suffix = "primary";
    output.external_slots[1].slot_name = "secondary";
    output.external_slots[1].key_suffix = "secondary";
    output.encode_fn = &EncodeDemoMultiOutput;
    if (!IoConverterRegistry::Instance().RegisterOutputConverter(output))
      return false;
    if (!IoBindingRegistry::Instance().RegisterBinding(
            {biz.biz_name, "keyword.plain", output.converter_id}))
      return false;
  }
  if (!IoBindingRegistry::Instance().FindBinding(
          "test_demo_unsupported_inputs")) {
    auto biz = *production_biz;
    biz.biz_name = "test_demo_unsupported_inputs";
    if (!PipelineCatalog::RegisterBizDefinition(biz)) return false;
    auto input = *production_input;
    input.converter_id = "test_demo_unsupported_inputs";
    input.external_slots = {production_input->external_slots[0],
                            production_input->external_slots[0]};
    input.external_slots[1].slot_name = "extra";
    input.external_slots[1].key_suffix = "extra";
    if (!IoConverterRegistry::Instance().RegisterInputConverter(input))
      return false;
    if (!IoBindingRegistry::Instance().RegisterBinding(
            {biz.biz_name, input.converter_id, "keyword.result"}))
      return false;
  }
  return true;
}

class ScopedDemoIoFixtures {
 public:
  ScopedDemoIoFixtures()
      : bizs_(PipelineCatalog::Bizs()),
        inputs_(IoConverterRegistry::Instance().AllInputConverters()),
        outputs_(IoConverterRegistry::Instance().AllOutputConverters()),
        bindings_(IoBindingRegistry::Instance().AllBindings()) {}
  ~ScopedDemoIoFixtures() {
    IoBindingRegistry::Instance().ClearForTesting();
    IoConverterRegistry::Instance().ClearForTesting();
    test_support::RegistryTestAccess::ResetBizs();
    EXPECT_TRUE(PipelineCatalog::RegisterBizDefinitions(bizs_));
    for (const auto& input : inputs_)
      EXPECT_TRUE(
          IoConverterRegistry::Instance().RegisterInputConverter(input));
    for (const auto& output : outputs_)
      EXPECT_TRUE(
          IoConverterRegistry::Instance().RegisterOutputConverter(output));
    for (const auto& binding : bindings_)
      EXPECT_TRUE(IoBindingRegistry::Instance().RegisterBinding(binding));
  }

 private:
  std::vector<BizDefinition> bizs_;
  std::vector<InputConverterDefinition> inputs_;
  std::vector<OutputConverterDefinition> outputs_;
  std::vector<IoBindingDefinition> bindings_;
};

}  // namespace
}  // namespace llm_edgeflow::test

namespace {

class EnvironmentGuard {
 public:
  explicit EnvironmentGuard(const char* name) : name_(name) {
    const char* value = std::getenv(name);
    if (value) {
      had_value_ = true;
      value_ = value;
    }
  }

  ~EnvironmentGuard() {
    if (had_value_) {
      setenv(name_, value_.c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

 private:
  const char* name_;
  bool had_value_ = false;
  std::string value_;
};

class DemoLogLevelGuard {
 public:
  DemoLogLevelGuard()
      : saved_level_(AlgBase_getLogLevelByName(COMPANY_ALG_LOG_NAME)) {}
  ~DemoLogLevelGuard() {
    (void)AlgBase_setLogLevelByName(COMPANY_ALG_LOG_NAME, saved_level_);
  }

 private:
  int saved_level_;
};

class KiteDemoDirectory final {
 public:
  KiteDemoDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("edgeflow-kite-demo-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(path)) {
      throw std::runtime_error("Cannot create Kite demo test directory");
    }
  }
  ~KiteDemoDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

}  // namespace

TEST(DemoRunnerTest, RealKiteEntityExtractionThroughOperator) {
  if (!llm_edgeflow::BackendRegistry::Instance().Find("kite_llm")) {
    GTEST_SKIP() << "kiteLLM SDK support is disabled";
  }
  const char* model_path = std::getenv("LLM_EDGEFLOW_TEST_KITELLM_MODEL");
  if (!model_path || !*model_path) {
    GTEST_SKIP()
        << "Set LLM_EDGEFLOW_TEST_KITELLM_MODEL for the real demo gate";
  }
  ASSERT_TRUE(std::filesystem::is_regular_file(model_path));
  KiteDemoDirectory temporary;
  // 产物保留在部署根目录内，不能通过符号链接逃逸。
  std::error_code ec;
  std::filesystem::create_hard_link(std::filesystem::absolute(model_path),
                                    temporary.path / "model.gguf", ec);
  if (ec) {
    ASSERT_TRUE(
        std::filesystem::copy_file(model_path, temporary.path / "model.gguf"));
  }

  std::ifstream pipeline_input("configs/pipeline_entity_extract_cpu.json");
  ASSERT_TRUE(pipeline_input.good());
  auto pipeline = nlohmann::json::parse(pipeline_input);
  auto& model = pipeline["models"][0];
  model["backend"] = "kite_llm";
  model["model_path"] = "model.gguf";
  model["backend_config"] = {{"run_config_file", "run.json"}};
  for (auto& node : pipeline["pipeline"]) {
    if (node["node_type"] == "LlmGenerateNode") {
      node["config"]["temperature"] = 0.0;
    }
    if (node["node_type"] == "StructuredJsonParseNode") {
      node["config"].erase("fallback");
      node["config"]["failure_policy"] = "fail";
    }
  }

  std::ofstream(temporary.path / "pipeline.json") << pipeline.dump(2);
  std::ofstream(temporary.path / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}}.dump(2);
  std::ofstream(temporary.path / "run.json")
      << R"({"schema_version":1,"model":{"context_size":256,"threads":2,"threads_batch":2,"gpu_layers":0},"logging":{"level":"error"}})";
  std::ofstream(temporary.path / "input.txt") << "张三在北京工作。\n";

  DemoOptions opts;
  opts.config_path = (temporary.path / "pipeline.conf").string();
  opts.dataset_path = (temporary.path / "input.txt").string();
  opts.output_dir = (temporary.path / "output").string();
  opts.chip = "cpu";
  opts.device_id = 0;
  opts.batch_size = 1;
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  const int result = RunOperatorDemo(opts);
  EXPECT_EQ(ops.DeInit(), 0);
  ASSERT_EQ(result, 0);

  std::ifstream results(temporary.path / "output/pipeline/results.jsonl");
  ASSERT_TRUE(results.good());
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(results, line)));
  const auto sample = nlohmann::json::parse(line);
  EXPECT_EQ(sample["request_id"], 30001);
  EXPECT_EQ(sample["status"], 0);
  ASSERT_TRUE(sample["output"].contains("entities"));
  EXPECT_FALSE(sample["output"]["entities"].empty());
}

TEST(DemoRunnerTest, LogLevelEnvironmentConfiguration) {
  EnvironmentGuard environment_guard("LLMEDGEFLOW_LEVEL");
  DemoLogLevelGuard log_level_guard;

  ASSERT_EQ(AlgBase_setLogLevelByName(COMPANY_ALG_LOG_NAME,
                                      E_ALG_BASE_LOG_LEVEL_WARNING),
            0);
  unsetenv("LLMEDGEFLOW_LEVEL");
  ConfigureLogLevelFromEnvironment();
  EXPECT_EQ(AlgBase_getLogLevelByName(COMPANY_ALG_LOG_NAME),
            E_ALG_BASE_LOG_LEVEL_WARNING);

  setenv("LLMEDGEFLOW_LEVEL", "4", 1);
  ConfigureLogLevelFromEnvironment();
  EXPECT_EQ(AlgBase_getLogLevelByName(COMPANY_ALG_LOG_NAME),
            E_ALG_BASE_LOG_LEVEL_DEBUG);

  for (const char* invalid : {"", "3x", " 3", "+3", "6", "-1"}) {
    ASSERT_EQ(AlgBase_setLogLevelByName(COMPANY_ALG_LOG_NAME,
                                        E_ALG_BASE_LOG_LEVEL_WARNING),
              0);
    setenv("LLMEDGEFLOW_LEVEL", invalid, 1);
    ConfigureLogLevelFromEnvironment();
    EXPECT_EQ(AlgBase_getLogLevelByName(COMPANY_ALG_LOG_NAME),
              E_ALG_BASE_LOG_LEVEL_WARNING)
        << "invalid value: " << invalid;
  }
}

// 1. 测试 CLI 命令行解析
TEST(DemoRunnerTest, CommandLineParsingSuccess) {
  const char* argv[] = {"alg_demo",
                        "--profile",
                        "entity_extract_mock",
                        "--config",
                        "demo/fixtures/mock/pipeline_entity_extract.conf",
                        "--dataset",
                        "data/corpus_entity_extract.txt",
                        "--output-dir",
                        "./results/test_out",
                        "--suite",
                        "smoke",
                        "--append",
                        "--allow-fallback-sample"};
  int argc = sizeof(argv) / sizeof(argv[0]);

  DemoOptions opts;
  std::string err;
  int ret = ParseCommandLine(argc, const_cast<char**>(argv), &opts, &err);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(opts.profile, "entity_extract_mock");
  EXPECT_EQ(opts.config_path,
            "demo/fixtures/mock/pipeline_entity_extract.conf");
  EXPECT_EQ(opts.dataset_path, "data/corpus_entity_extract.txt");
  EXPECT_EQ(opts.output_dir, "./results/test_out");
  EXPECT_EQ(opts.batch_size, 1);
  EXPECT_EQ(opts.device_id, 0);
  EXPECT_EQ(opts.chip, "cpu");
  EXPECT_EQ(opts.depth_num, 1u);
  EXPECT_EQ(opts.suite, "smoke");
  EXPECT_TRUE(opts.has_suite);
  EXPECT_TRUE(opts.append);
  EXPECT_TRUE(opts.allow_fallback_sample);
}

TEST(DemoRunnerTest, UnknownCommandLineFlagsAreRejected) {
  for (const char* flag : {"--unknown-option"}) {
    SCOPED_TRACE(flag);
    const char* argv[] = {"alg_demo", flag, "value"};
    DemoOptions opts;
    std::string err;
    EXPECT_EQ(ParseCommandLine(3, const_cast<char**>(argv), &opts, &err), 2);
    EXPECT_NE(err.find("Unknown CLI option"), std::string::npos);
  }
}

TEST(DemoRunnerTest, ConfigAloneResolvesRegisteredCarriers) {
  const char* argv[] = {"alg_demo", "--config",
                        "configs/pipeline_keyword_match_rules.conf"};
  DemoOptions options;
  std::string error;
  ASSERT_EQ(ParseCommandLine(3, const_cast<char**>(argv), &options, &error), 0);
  OperatorIoContract contract;
  ASSERT_TRUE(ResolveConfigIo(options, &contract, &error)) << error;
  ASSERT_EQ(contract.inputs.size(), 1U);
  ASSERT_EQ(contract.outputs.size(), 1U);
  EXPECT_EQ(contract.inputs[0].type, "keyword_in");
  EXPECT_EQ(contract.outputs[0].type, "keyword_out");
  EXPECT_NE(DemoIoRegistry::Instance().FindInput(contract.inputs[0].type_name),
            nullptr);
  EXPECT_NE(
      DemoIoRegistry::Instance().FindOutput(contract.outputs[0].type_name),
      nullptr);
}

// CLI 错误保持退出码 2。
TEST(DemoRunnerTest, CommandLineParsingErrors) {
  DemoOptions opts;
  std::string err;

  // 未知参数
  const char* argv1[] = {"alg_demo", "--unknown-flag"};
  EXPECT_EQ(ParseCommandLine(2, const_cast<char**>(argv1), &opts, &err), 2);

  // 参数缺少值
  const char* argv2[] = {"alg_demo", "--profile"};
  EXPECT_EQ(ParseCommandLine(2, const_cast<char**>(argv2), &opts, &err), 2);

  // 非法 suite
  const char* argv5[] = {"alg_demo", "--suite", "invalid_suite"};
  EXPECT_EQ(ParseCommandLine(3, const_cast<char**>(argv5), &opts, &err), 2);
}

TEST(DemoRunnerTest, RejectsProfileOnlyExecutionFlags) {
  for (const char* flag :
       {"--batch-size", "--device-id", "--chip", "--depth"}) {
    SCOPED_TRACE(flag);
    const char* argv[] = {"alg_demo", flag, "1"};
    DemoOptions options;
    std::string error;
    EXPECT_EQ(ParseCommandLine(3, const_cast<char**>(argv), &options, &error),
              2);
    EXPECT_EQ(error, "Unknown CLI option: '" + std::string(flag) + "'");
  }
}

// 2. 测试芯片白名单解析
TEST(DemoRunnerTest, ComputePlatformWhitelistValidation) {
  EXPECT_EQ(DemoOptions{}.chip, "cpu");

  ComputePlatform type;

  EXPECT_TRUE(ParseComputePlatform("ax650", &type));
  EXPECT_EQ(type, ComputePlatform::kAx650);

  EXPECT_TRUE(ParseComputePlatform("AX650", &type));
  EXPECT_EQ(type, ComputePlatform::kAx650);

  EXPECT_TRUE(ParseComputePlatform("ascend310p", &type));
  EXPECT_EQ(type, ComputePlatform::kAscend310P);

  EXPECT_TRUE(ParseComputePlatform("ascend910b", &type));
  EXPECT_EQ(type, ComputePlatform::kAscend910B);

  EXPECT_TRUE(ParseComputePlatform("rk3588", &type));
  EXPECT_EQ(type, ComputePlatform::kRk3588);

  EXPECT_TRUE(ParseComputePlatform("cuda", &type));
  EXPECT_EQ(type, ComputePlatform::kCuda);

  EXPECT_TRUE(ParseComputePlatform("cpu", &type));
  EXPECT_EQ(type, ComputePlatform::kCpu);

  EXPECT_FALSE(ParseComputePlatform("cpu_generic", &type));
  EXPECT_FALSE(ParseComputePlatform("invalid_hardware", &type));
  EXPECT_EQ(type, ComputePlatform::kUnknown);
}

// 3. Profile 加载及其余 CLI 覆盖。
TEST(DemoRunnerTest, ProfileLoadAndMerge) {
  DemoOptions cli_opts;
  cli_opts.profile = "entity_extract_mock";

  DemoOptions merged;
  std::string err;
  int ret = LoadAndMergeProfiles("demo/profiles.json", cli_opts, &merged, &err);
  EXPECT_EQ(ret, 0) << "Error: " << err;

  EXPECT_EQ(merged.config_path,
            "demo/fixtures/mock/pipeline_entity_extract.conf");
  EXPECT_EQ(merged.dataset_path, "data/corpus_entity_extract.txt");
  EXPECT_EQ(merged.chip, "cpu");
  EXPECT_EQ(merged.batch_size, 1);
}

TEST(DemoRunnerTest, ProfileOwnsExecutionSettings) {
  KiteDemoDirectory temporary;
  const std::string path = (temporary.path / "profiles.json").string();
  const nlohmann::json profile = {
      {"config", "configs/pipeline_keyword_match_rules.conf"},
      {"dataset", "data/corpus_keyword_match.txt"},
      {"batch_size", 4},
      {"device_id", 2},
      {"chip", "cuda"},
      {"depth", 8}};
  auto write_profile = [&](const nlohmann::json& value) {
    std::ofstream(path) << nlohmann::json(
        {{"profiles", {{"execution", value}}}});
  };
  write_profile(profile);
  DemoOptions cli, merged;
  cli.profile = "execution";
  std::string error;
  ASSERT_EQ(LoadAndMergeProfiles(path, cli, &merged, &error), 0) << error;
  EXPECT_EQ(merged.batch_size, 4);
  EXPECT_EQ(merged.device_id, 2);
  EXPECT_EQ(merged.chip, "cuda");
  EXPECT_EQ(merged.depth_num, 8u);

  auto defaults = profile;
  for (const char* field : {"batch_size", "device_id", "chip", "depth"})
    defaults.erase(field);
  write_profile(defaults);
  ASSERT_EQ(LoadAndMergeProfiles(path, cli, &merged, &error), 0) << error;
  EXPECT_EQ(merged.batch_size, 1);
  EXPECT_EQ(merged.device_id, 0);
  EXPECT_EQ(merged.chip, "cpu");
  EXPECT_EQ(merged.depth_num, 1u);

  for (const auto& invalid :
       std::vector<std::pair<std::string, nlohmann::json>>{{"batch_size", 0},
                                                           {"batch_size", "2"},
                                                           {"device_id", -1},
                                                           {"device_id", "0"},
                                                           {"chip", "invalid"},
                                                           {"chip", 1},
                                                           {"depth", 0},
                                                           {"depth", "2"}}) {
    auto bad = profile;
    bad[invalid.first] = invalid.second;
    write_profile(bad);
    EXPECT_EQ(LoadAndMergeProfiles(path, cli, &merged, &error), 3);
    EXPECT_NE(error.find(invalid.first), std::string::npos) << error;
  }
}

TEST(DemoRunnerTest, ConfigOverrideDeterminesResolvedCarriers) {
  DemoOptions cli;
  cli.profile = "entity_extract_mock";
  cli.config_path = "configs/pipeline_keyword_match_rules.conf";
  cli.has_config_path = true;
  DemoOptions merged;
  std::string error;
  ASSERT_EQ(LoadAndMergeProfiles("demo/profiles.json", cli, &merged, &error), 0)
      << error;
  EXPECT_EQ(merged.config_path, cli.config_path);
  OperatorIoContract contract;
  ASSERT_TRUE(ResolveConfigIo(merged, &contract, &error)) << error;
  ASSERT_EQ(contract.inputs.size(), 1U);
  ASSERT_EQ(contract.outputs.size(), 1U);
  EXPECT_EQ(contract.inputs[0].type_name, "CompanyOperatorKeywordInput");
  EXPECT_EQ(contract.outputs[0].type_name, "CompanyOperatorKeywordOutput");
}

TEST(DemoRunnerTest, ProfileRejectsUnknownFieldsAndInvalidShapes) {
  KiteDemoDirectory temporary;
  const auto path = temporary.path / "profiles.json";
  const std::vector<std::pair<nlohmann::json, std::string>> cases = {
      {{{"config", "configs/pipeline_keyword_match_rules.conf"},
        {"dataset", "data/corpus_keyword_match.txt"},
        {"datset", "data/corpus_keyword_match.txt"}},
       "Unknown Profile field: 'datset'"},
      {nlohmann::json::array(), "must be an object"},
      {nullptr, "must be an object"}};
  for (const auto& [profile, diagnostic] : cases) {
    SCOPED_TRACE(profile.dump());
    std::ofstream(path) << nlohmann::json{{"profiles", {{"invalid", profile}}}};
    nlohmann::json profiles;
    std::string error;
    EXPECT_EQ(LoadAndValidateProfilesDocument(path.string(), &profiles, &error),
              3);
    EXPECT_NE(error.find(diagnostic), std::string::npos) << error;
  }
}

TEST(DemoRunnerTest, ProfileNotFound) {
  DemoOptions cli_opts;
  cli_opts.profile = "non_existent_profile_xyz";

  DemoOptions merged;
  std::string err;
  int ret = LoadAndMergeProfiles("demo/profiles.json", cli_opts, &merged, &err);
  EXPECT_EQ(ret, 3);
  EXPECT_NE(err.find("not found"), std::string::npos);
}

// P1-1 & P2-1: 测试 Profile Schema 严格校验、非法 suite 类型防御 (123)
// 与数值溢出防御
TEST(DemoRunnerTest, ProfileSchemaStrictValidation) {
  std::string temp_invalid_json = "./results/invalid_profile.json";
  std::filesystem::create_directories("./results");

  // Case 1: 缺少 profiles 映射
  {
    std::ofstream ofs(temp_invalid_json);
    ofs << "{}";
  }
  DemoOptions cli_opts;
  cli_opts.profile = "foo";
  DemoOptions merged;
  std::string err;
  EXPECT_EQ(LoadAndMergeProfiles(temp_invalid_json, cli_opts, &merged, &err),
            3);
  EXPECT_NE(err.find("profiles"), std::string::npos);

  // Case 2: 非法 suite 字符串
  {
    std::ofstream ofs(temp_invalid_json);
    ofs << R"({
      "profiles": {
        "bad_prof": {
          "config": "demo/fixtures/mock/pipeline_entity_extract.conf",
          "dataset": "data/corpus_entity_extract.txt",
          "suite": "invalid_suite"
        }
      }
    })";
  }
  EXPECT_EQ(LoadAndMergeProfiles(temp_invalid_json, cli_opts, &merged, &err),
            3);
  EXPECT_NE(err.find("bad_prof"), std::string::npos);

  // Case 3 (P1-1 探针): suite 字段为数字 123 -> 必须返回 3 而非抛出异常崩溃
  // (exit 134)
  {
    std::ofstream ofs(temp_invalid_json);
    ofs << R"({
      "profiles": {
        "bad_suite_type": {
          "config": "configs/pipeline_keyword_match_rules.conf",
          "dataset": "data/corpus_keyword_match.txt",
          "suite": 123
        }
      }
    })";
  }
  EXPECT_EQ(LoadAndMergeProfiles(temp_invalid_json, cli_opts, &merged, &err),
            3);
  EXPECT_NE(err.find("suite"), std::string::npos);

  // Case 4: batch_size 数值超界溢出 (4294967297) 防御拦截
  {
    std::ofstream ofs(temp_invalid_json);
    ofs << R"({
      "profiles": {
        "overflow_prof": {
          "config": "demo/fixtures/mock/pipeline_entity_extract.conf",
          "dataset": "data/corpus_entity_extract.txt",
          "batch_size": 4294967297
        }
      }
    })";
  }
  EXPECT_EQ(LoadAndMergeProfiles(temp_invalid_json, cli_opts, &merged, &err),
            3);
  EXPECT_NE(err.find("batch_size"), std::string::npos);

  std::filesystem::remove(temp_invalid_json);
}

// 登记冲突用局部注册表验证，不改变进程中的生产登记。
TEST(DemoRunnerTest, RegistryLookupAndConflictDetection) {
  const auto& production = DemoIoRegistry::Instance();
  EXPECT_FALSE(production.HasConflict());
  for (const char* type :
       {"CompanyOperatorKeywordInput", "CompanyOperatorEntityInput",
        "CompanyOperatorDocInput", "CompanyOperatorAuditInput",
        "CompanyOperatorRerankInput", "CompanyOperatorAudioInput",
        "CompanyFrame,CompanyString"}) {
    SCOPED_TRACE(type);
    EXPECT_NE(production.FindInput(type), nullptr);
  }
  for (const char* type :
       {"CompanyOperatorKeywordOutput", "CompanyOperatorEntityOutput",
        "CompanyOperatorDocOutput", "CompanyOperatorAuditOutput",
        "CompanyOperatorRerankOutput", "CompanyOperatorAudioOutput",
        "CompanyOdOutput"}) {
    SCOPED_TRACE(type);
    EXPECT_NE(production.FindOutput(type), nullptr);
  }
  EXPECT_EQ(production.FindInput("CompanyString,CompanyFrame"), nullptr);

  const BuildRequestsFn build = [](const DemoOptions&,
                                   const std::vector<OperatorIoEntry>&,
                                   DemoRequestBatch*) { return 0; };
  const ShowResultFn show = [](const void*, const nlohmann::json&, uint64_t*,
                               int32_t*, nlohmann::json*) {};
  DemoIoRegistry registry;
  EXPECT_TRUE(registry.RegisterInput("Z", build));
  EXPECT_TRUE(registry.RegisterInput("A,B", build));
  EXPECT_TRUE(registry.RegisterOutput("Z", show));
  EXPECT_TRUE(registry.RegisterOutput("A", show));
  EXPECT_FALSE(registry.HasConflict());
  EXPECT_EQ(registry.FindInput("A,B"), build);
  EXPECT_EQ(registry.FindInput("B,A"), nullptr);
  EXPECT_EQ(registry.FindOutput("A"), show);
  EXPECT_EQ(registry.FindOutput("unknown"), nullptr);
  EXPECT_EQ(registry.ListInputs(), (std::vector<std::string>{"A,B", "Z"}));
  EXPECT_EQ(registry.ListOutputs(), (std::vector<std::string>{"A", "Z"}));
  EXPECT_FALSE(registry.RegisterInput("A,B", build));
  EXPECT_FALSE(registry.RegisterOutput("A", show));
  EXPECT_FALSE(registry.RegisterInput("", build));
  EXPECT_FALSE(registry.RegisterOutput("", show));
  EXPECT_FALSE(registry.RegisterInput("null", nullptr));
  EXPECT_FALSE(registry.RegisterOutput("null", nullptr));
  EXPECT_EQ(registry.FindInput("null"), nullptr);
  EXPECT_EQ(registry.FindOutput("null"), nullptr);
  EXPECT_TRUE(registry.HasConflict());
  EXPECT_FALSE(production.HasConflict());
}

TEST(DemoRunnerTest, UnsupportedInputCombinationListsRegisteredCarriers) {
  llm_edgeflow::test::ScopedDemoIoFixtures fixtures;
  ASSERT_TRUE(llm_edgeflow::test::RegisterDemoIoFixtures());
  KiteDemoDirectory temporary;
  std::ifstream pipeline_file("configs/pipeline_keyword_match_rules.json");
  ASSERT_TRUE(pipeline_file.good());
  auto pipeline = nlohmann::json::parse(pipeline_file);
  pipeline["deployment"]["io"]["io_binding"] = "test_demo_unsupported_inputs";
  std::ofstream(temporary.path / "pipeline.json") << pipeline;
  std::ofstream(temporary.path / "pipeline.conf")
      << R"({"pipe_path":"pipeline.json"})";
  DemoOptions options;
  options.config_path = (temporary.path / "pipeline.conf").string();
  options.dataset_path = "data/corpus_keyword_match.txt";
  options.output_dir = temporary.path.string();
  OperatorIoContract contract;
  std::string error;
  ASSERT_TRUE(ResolveConfigIo(options, &contract, &error)) << error;
  ASSERT_EQ(contract.inputs.size(), 2U);
  testing::internal::CaptureStderr();
  const int ret = RunOperatorDemo(options);
  const auto diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_EQ(ret, 3);
  EXPECT_NE(diagnostic.find(
                "CompanyOperatorKeywordInput,CompanyOperatorKeywordInput"),
            std::string::npos);
  EXPECT_NE(diagnostic.find("input: CompanyFrame,CompanyString"),
            std::string::npos);
  EXPECT_NE(diagnostic.find("output: CompanyOperatorKeywordOutput"),
            std::string::npos);
  EXPECT_FALSE(
      std::filesystem::exists(temporary.path / "pipeline/results.jsonl"));
}

TEST(DemoRunnerTest, MultiOutputCollisionsPreserveValuesAndReleaseEachBatch) {
  llm_edgeflow::test::ScopedDemoIoFixtures fixtures;
  ASSERT_TRUE(llm_edgeflow::test::RegisterDemoIoFixtures());
  KiteDemoDirectory temporary;
  std::ifstream pipeline_file("configs/pipeline_keyword_match_rules.json");
  ASSERT_TRUE(pipeline_file.good());
  auto pipeline = nlohmann::json::parse(pipeline_file);
  pipeline["deployment"]["io"] = {
      {"io_binding", "test_demo_multi_output"},
      {"out_mem",
       {{"primary", {{"capacities", {{"match_result_json", 2047}}}}},
        {"secondary", {{"capacities", {{"match_result_json", 2047}}}}}}}};
  std::ofstream(temporary.path / "pipeline.json") << pipeline;
  std::ofstream(temporary.path / "pipeline.conf")
      << R"({"pipe_path":"pipeline.json"})";
  std::ofstream(temporary.path / "input.txt")
      << "初始化自检\n普通文本\n初始化自检\n";
  DemoOptions options;
  options.config_path = (temporary.path / "pipeline.conf").string();
  options.dataset_path = (temporary.path / "input.txt").string();
  options.output_dir = temporary.path.string();
  options.batch_size = 1;
  options.depth_num = 1;  // 三批必须复用每个槽的同一个池块。
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  ASSERT_EQ(RunOperatorDemo(options), 0);
  EXPECT_EQ(ops.DeInit(), 0);

  std::ifstream results(temporary.path / "pipeline/results.jsonl");
  ASSERT_TRUE(results.good());
  std::string line;
  for (int index = 0; index < 3; ++index) {
    ASSERT_TRUE(static_cast<bool>(std::getline(results, line)));
    const auto sample = nlohmann::json::parse(line);
    EXPECT_EQ(sample["request_id"], 20001 + index);
    EXPECT_EQ(sample["status"], index == 1 ? -17 : 0);
    const auto& output = sample["output"];
    EXPECT_EQ(output.size(), 4U);
    EXPECT_FALSE(output.contains("is_hit"));
    EXPECT_FALSE(output.contains("match_result"));
    EXPECT_EQ(output["primary.is_hit"], index != 1);
    EXPECT_EQ(output["secondary.is_hit"], index == 1);
    EXPECT_TRUE(output["primary.match_result"].is_object());
    EXPECT_EQ(output["primary.match_result"], output["secondary.match_result"]);
    if (index != 1) {
      EXPECT_NE(output["primary.match_result"].dump().find("SYSTEM_INIT"),
                std::string::npos);
    }
  }
  EXPECT_FALSE(static_cast<bool>(std::getline(results, line)));
  std::ifstream summary_file(temporary.path / "pipeline/summary.json");
  ASSERT_TRUE(summary_file.good());
  const auto summary = nlohmann::json::parse(summary_file);
  EXPECT_EQ(summary["total_samples"], 3);
  EXPECT_EQ(summary["success_count"], 2);
  EXPECT_EQ(summary["failed_count"], 1);
}

TEST(DemoRunnerTest, BuiltCarriersSurviveMoveAndDisplayActualOperatorOutputs) {
  KiteDemoDirectory temporary;
  std::ifstream rerank_source("configs/pipeline_cross_rerank_cpu.json");
  ASSERT_TRUE(rerank_source.good());
  auto rerank_pipeline = nlohmann::json::parse(rerank_source);
  auto& model = rerank_pipeline["models"][0];
  model["model_type"] = "test_biz_rerank";
  model["backend"] = "test_tensor_backend";
  model["model_path"] = "rerank.fixture";
  model["model_config"] = nlohmann::json::object();
  model["backend_config"] = nlohmann::json::object();
  std::ofstream(temporary.path / "rerank.json") << rerank_pipeline;
  std::ofstream(temporary.path / "rerank.conf")
      << R"({"pipe_path":"rerank.json"})";
  struct Case {
    std::string config;
    const char* dataset;
    uint64_t request_id;
    size_t count;
    const char* output_field;
    const char* suite = "smoke";
  };
  const std::vector<Case> cases = {
      {"configs/pipeline_keyword_match_rules.conf",
       "data/corpus_keyword_match.txt", 20001, 2, "is_hit"},
      {"demo/fixtures/mock/pipeline_entity_extract.conf",
       "data/corpus_entity_extract.txt", 30001, 1, "entities"},
      {"demo/fixtures/mock/pipeline_doc_qa.conf", "data/corpus_doc_qa.txt",
       10001, 2, "answer_text"},
      {"demo/fixtures/mock/pipeline_dialogue_audit.conf",
       "data/corpus_dialogue_audit.txt", 40001, 2, "audit_verdict"},
      {"demo/fixtures/mock/pipeline_ocr_invoice_qa.conf",
       "data/corpus_ocr_invoice_qa.txt", 60001, 1, "extracted_invoice"},
      {"demo/fixtures/mock/pipeline_audio_asr_intent.conf",
       "data/corpus_audio_asr_intent.txt", 70001, 1, "transcribed_text"},
      {"demo/fixtures/mock/pipeline_audio_asr_intent.conf",
       "data/corpus_audio_asr_intent_whisper.jsonl", 70001, 1,
       "transcribed_text", "real"},
      {(temporary.path / "rerank.conf").string(),
       "data/corpus_cross_rerank.txt", 80001, 1, "ranked_results"}};
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.config);
    DemoOptions options;
    options.config_path = test_case.config;
    options.dataset_path = test_case.dataset;
    options.suite = test_case.suite;
    OperatorIoContract contract;
    std::string error;
    ASSERT_TRUE(ResolveConfigIo(options, &contract, &error)) << error;
    std::string types;
    for (const auto& entry : contract.inputs) {
      if (!types.empty()) types += ',';
      types += entry.type_name;
    }
    const auto build = DemoIoRegistry::Instance().FindInput(types);
    ASSERT_NE(build, nullptr);
    DemoRequestBatch batch;
    std::weak_ptr<void> lifetime;
    {
      DemoRequestBatch built;
      ASSERT_EQ(build(options, contract.inputs, &built), 0);
      ASSERT_NE(built.storage, nullptr);
      lifetime = built.storage;
      batch = std::move(built);
    }
    EXPECT_FALSE(lifetime.expired());
    ASSERT_EQ(batch.requests.size(), test_case.count);
    for (const auto& row : batch.requests) {
      ASSERT_EQ(row.size(), contract.inputs.size());
      for (const auto& entry : contract.inputs)
        ASSERT_NE(row.at("demo." + entry.type), nullptr);
    }
    if (contract.inputs.size() == 2) {
      EXPECT_EQ(contract.inputs[0].type_name, "CompanyFrame");
      EXPECT_EQ(contract.inputs[1].type_name, "CompanyString");
    }
    OperatorFunc instance_ops{};
    void* handle = nullptr;
    ASSERT_EQ(CreateOperatorInstance(options, "DemoStorageTest", &instance_ops,
                                     &handle),
              0);
    std::vector<nlohmann::json> copied_outputs;
    {
      OperatorHandleGuard guard(instance_ops, handle);
      for (size_t i = 0; i < batch.requests.size(); ++i) {
        NamedIoBatch inputs{batch.requests[i]}, outputs(1);
        ASSERT_EQ(contract.outputs.size(), 1U);
        const auto& entry = contract.outputs[0];
        outputs[0]["demo." + entry.type] = nullptr;
        ASSERT_EQ(instance_ops.Process(handle, inputs, outputs), 0)
            << GetOperatorLastError();
        const auto show =
            DemoIoRegistry::Instance().FindOutput(entry.type_name);
        ASSERT_NE(show, nullptr);
        uint64_t request_id = 0;
        int32_t status = 0;
        nlohmann::json displayed = nlohmann::json::object();
        const auto info = i < batch.request_info.size()
                              ? batch.request_info[i]
                              : nlohmann::json::object();
        const auto& output = outputs[0].at("demo." + entry.type);
        ASSERT_NE(output, nullptr);
        show(output.get(), info, &request_id, &status, &displayed);
        EXPECT_EQ(request_id, test_case.request_id + i);
        EXPECT_EQ(status, 0);
        EXPECT_TRUE(displayed.contains(test_case.output_field));
        if (entry.type_name == "CompanyOperatorRerankOutput") {
          EXPECT_EQ(displayed["query"], "怎么办理7天无理由退款？");
          ASSERT_EQ(displayed["ranked_results"].size(), 3U);
          EXPECT_EQ(displayed["ranked_results"][0]["passage_index"], 1);
          EXPECT_EQ(displayed["ranked_results"][0]["passage_text"],
                    "条款B: 售后退款支持7天无理由，原路退回付款账户。");
          nlohmann::json without_info = nlohmann::json::object();
          show(output.get(), nlohmann::json::object(), &request_id, &status,
               &without_info);
          EXPECT_EQ(request_id, 80001U);
          EXPECT_EQ(status, 0);
          EXPECT_FALSE(without_info.contains("query"));
          ASSERT_EQ(without_info["ranked_results"].size(), 3U);
          for (const auto& ranked : without_info["ranked_results"])
            EXPECT_FALSE(ranked.contains("passage_text"));
          EXPECT_EQ(without_info["ranked_results"][0]["passage_index"], 1);
        } else if (entry.type_name == "CompanyOperatorAuditOutput") {
          EXPECT_EQ(displayed["channel"], info["channel"]);
          nlohmann::json without_info = nlohmann::json::object();
          show(output.get(), nlohmann::json::object(), &request_id, &status,
               &without_info);
          EXPECT_FALSE(without_info.contains("channel"));
          EXPECT_EQ(without_info["audit_verdict"], displayed["audit_verdict"]);
        }
        copied_outputs.push_back(std::move(displayed));
      }
    }
    batch = DemoRequestBatch{};
    EXPECT_TRUE(lifetime.expired());
    for (const auto& displayed : copied_outputs)
      EXPECT_TRUE(displayed.contains(test_case.output_field));
    if (std::string(test_case.output_field) == "ranked_results") {
      EXPECT_EQ(copied_outputs[0]["ranked_results"][0]["passage_text"],
                "条款B: 售后退款支持7天无理由，原路退回付款账户。");
    }
  }
  EXPECT_EQ(ops.DeInit(), 0);
}

TEST(DemoRunnerTest, EntityAndTranslateShareCarrierBuildersAndDisplays) {
  DemoOptions entity;
  entity.config_path = "demo/fixtures/mock/pipeline_entity_extract.conf";
  OperatorIoContract entity_contract, translate_contract;
  std::string error;
  ASSERT_TRUE(ResolveConfigIo(entity, &entity_contract, &error)) << error;
  KiteDemoDirectory temporary;
  const std::string envelope =
      R"({"query":"Hello world","endpoint":"translate","extra":{"keep":true}})";
  std::ofstream(temporary.path / "input.jsonl") << envelope << '\n';
  DemoOptions translate;
  translate.config_path = "demo/fixtures/mock/pipeline_translate.conf";
  translate.dataset_path = (temporary.path / "input.jsonl").string();
  translate.output_dir = temporary.path.string();
  ASSERT_TRUE(ResolveConfigIo(translate, &translate_contract, &error)) << error;
  ASSERT_EQ(entity_contract.inputs.size(), 1U);
  ASSERT_EQ(translate_contract.inputs.size(), 1U);
  ASSERT_EQ(entity_contract.outputs.size(), 1U);
  ASSERT_EQ(translate_contract.outputs.size(), 1U);
  const auto& registry = DemoIoRegistry::Instance();
  const auto build = registry.FindInput(translate_contract.inputs[0].type_name);
  ASSERT_NE(build, nullptr);
  EXPECT_EQ(build, registry.FindInput(entity_contract.inputs[0].type_name));
  EXPECT_EQ(registry.FindOutput(translate_contract.outputs[0].type_name),
            registry.FindOutput(entity_contract.outputs[0].type_name));
  DemoRequestBatch batch;
  ASSERT_EQ(build(translate, translate_contract.inputs, &batch), 0);
  ASSERT_EQ(batch.requests.size(), 1U);
  const auto* carrier = static_cast<const CompanyOperatorEntityInput*>(
      batch.requests[0].at("demo.entity_in").get());
  ASSERT_NE(carrier, nullptr);
  ASSERT_NE(carrier->sentence_text, nullptr);
  EXPECT_EQ(
      std::string(carrier->sentence_text->data, carrier->sentence_text->length),
      envelope);
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  ASSERT_EQ(RunOperatorDemo(translate), 0);
  EXPECT_EQ(ops.DeInit(), 0);
  std::ifstream results(temporary.path / "pipeline_translate/results.jsonl");
  ASSERT_TRUE(results.good());
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(results, line)));
  const auto sample = nlohmann::json::parse(line);
  EXPECT_EQ(sample["request_id"], 30001);
  EXPECT_EQ(sample["status"], 0);
  EXPECT_TRUE(sample["output"]["entities"]["translated"].is_string());
}

TEST(DemoRunnerTest, AudioDatasetRequirementsUseSuiteClassification) {
  DemoOptions options;
  options.config_path = "demo/fixtures/mock/pipeline_audio_asr_intent.conf";
  options.dataset_path = "data/corpus_audio_asr_intent.txt";
  OperatorIoContract contract;
  std::string error;
  ASSERT_TRUE(ResolveConfigIo(options, &contract, &error)) << error;
  ASSERT_EQ(contract.inputs.size(), 1U);
  const auto build =
      DemoIoRegistry::Instance().FindInput(contract.inputs[0].type_name);
  ASSERT_NE(build, nullptr);
  DemoRequestBatch batch;
  options.suite = "smoke";
  options.profile = "audio_whisper_named_profile";
  ASSERT_EQ(build(options, contract.inputs, &batch), 0);
  ASSERT_EQ(batch.requests.size(), 1U);
  options.suite = "real";
  options.profile = "audio_named_profile";
  EXPECT_EQ(build(options, contract.inputs, &batch), 4);
  options.dataset_path.clear();
  EXPECT_EQ(build(options, contract.inputs, &batch), 4);
  options.allow_fallback_sample = true;
  ASSERT_EQ(build(options, contract.inputs, &batch), 0);
  ASSERT_EQ(batch.requests.size(), 1U);
  options.allow_fallback_sample = false;
  options.dataset_path = "data/corpus_audio_asr_intent_whisper.jsonl";
  ASSERT_EQ(build(options, contract.inputs, &batch), 0);
  ASSERT_NE(batch.storage, nullptr);
  ASSERT_EQ(batch.requests.size(), 1U);
  const auto* carrier = static_cast<const CompanyOperatorAudioInput*>(
      batch.requests[0].at("demo.audio_in").get());
  ASSERT_NE(carrier, nullptr);
  EXPECT_EQ(carrier->request_id, 70001U);
  EXPECT_EQ(carrier->sample_rate, 16000);
  EXPECT_EQ(
      carrier->pcm_length,
      std::filesystem::file_size("data/audio/nav_001.f32") / sizeof(float));
  ASSERT_NE(carrier->pcm_buffer, nullptr);
}

TEST(DemoRunnerTest, AllSuitePreservesRealAudioProfileDatasetRequirements) {
  KiteDemoDirectory temporary;
  const auto profiles_path = temporary.path / "profiles.json";
  const nlohmann::json profiles = {
      {"profiles",
       {{"real_audio",
         {{"config", std::filesystem::absolute(
                         "demo/fixtures/mock/pipeline_audio_asr_intent.conf")
                         .string()},
          {"dataset",
           std::filesystem::absolute("data/corpus_audio_asr_intent.txt")
               .string()},
          {"suite", "real"}}}}}};
  std::ofstream(profiles_path) << profiles;
  const auto output_dir = temporary.path / "results";
  ASSERT_TRUE(std::filesystem::is_regular_file(EDGEFLOW_DEMO_BINARY));
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    execl(EDGEFLOW_DEMO_BINARY, "alg_demo", "--profiles-file",
          profiles_path.c_str(), "--suite", "all", "--output-dir",
          output_dir.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 4);
  EXPECT_FALSE(
      std::filesystem::exists(output_dir / "real_audio/results.jsonl"));
}

TEST(DemoRunnerTest,
     CustomNodeProfilesRunThroughOperatorAndPackExpectedResults) {
  KiteDemoDirectory temporary;
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  for (const char* profile :
       {"entity_extract_custom_mock", "doc_qa_custom_mock"}) {
    SCOPED_TRACE(profile);
    DemoOptions cli;
    cli.profile = profile;
    cli.output_dir = temporary.path.string();
    DemoOptions options;
    std::string error;
    ASSERT_EQ(LoadAndMergeProfiles("demo/profiles.json", cli, &options, &error),
              0)
        << error;
    options.batch_size = 2;  // 覆盖多请求的自定义 Node 执行。
    ASSERT_EQ(RunOperatorDemo(options), 0);
    std::ifstream results(temporary.path / profile / "results.jsonl");
    ASSERT_TRUE(results.good());
    std::string line;
    size_t index = 0;
    while (std::getline(results, line)) {
      const auto sample = nlohmann::json::parse(line);
      EXPECT_EQ(sample["status"], 0);
      const auto& output = sample["output"];
      if (std::string(profile) == "entity_extract_custom_mock") {
        EXPECT_EQ(sample["request_id"], 30001 + index);
        ASSERT_TRUE(output.contains("entities"));
        EXPECT_EQ(output["entities"]["nouns"],
                  nlohmann::json({"张三", "清华大学", "北京", "人工智能",
                                  "算法工程师", "NPU", "芯片", "深度学习",
                                  "大模型", "项目", "公司"}));
      } else {
        EXPECT_EQ(sample["request_id"], 10001 + index);
        EXPECT_EQ(output["intent_name"],
                  index == 0 ? "TECH_ARCHITECTURE" : "AFTER_SALES_REFUND");
        EXPECT_EQ(output["answer_text"],
                  index == 0 ? "【LLM总结】文档核心为现代软件工程化设计，包含松"
                               "耦合、状态隔离与跨平台编译。"
                             : "【LLM意图分析】检测到售后退款诉求。建议操作：7"
                               "天无理由退货审核流程。");
        EXPECT_GT(output["chunk_count"].get<int>(), 0);
      }
      ++index;
    }
    EXPECT_EQ(index,
              std::string(profile) == "entity_extract_custom_mock" ? 1U : 2U);
    std::ifstream summary_file(temporary.path / profile / "summary.json");
    const auto summary = nlohmann::json::parse(summary_file);
    EXPECT_EQ(summary["failed_count"], 0);
    EXPECT_EQ(summary["success_count"], index);
  }
  EXPECT_EQ(ops.DeInit(), 0);
}

// 5. 测试 DatasetReader 数据读取
TEST(DemoRunnerTest, DatasetReaderFunctions) {
  std::vector<std::string> lines;
  std::string err;

  // 正常文件读取
  EXPECT_TRUE(
      ReadLinesFromFile("data/corpus_entity_extract.txt", &lines, &err));
  EXPECT_FALSE(lines.empty());

  // 不存在文件读取 -> 应该失败
  lines.clear();
  EXPECT_FALSE(ReadLinesFromFile("data/non_existent_file.txt", &lines, &err));
  EXPECT_FALSE(err.empty());

  // Tag sections 解析
  std::unordered_map<std::string, std::vector<std::string>> sections;
  EXPECT_TRUE(ParseTagSections("data/corpus_doc_qa.txt", &sections, &err));
  EXPECT_TRUE(sections.find("DOC") != sections.end());
  EXPECT_TRUE(sections.find("QUERY") != sections.end());
}

// 6. 测试 ResultWriter 结果落盘、错误样本记录与 --append 模式累计摘要口径
TEST(DemoRunnerTest, ResultWriterAtomicOutputAndCumulativeAppend) {
  KiteDemoDirectory temporary;
  DemoOptions opts;
  opts.profile = "test_profile_unit";
  opts.output_dir = temporary.path.string();
  opts.append = false;

  ResultWriter writer(opts);

  std::vector<DemoSampleResult> samples;
  DemoSampleResult s1;
  s1.request_id = 9001;
  s1.status = 0;
  s1.latency_ms = 2.5;
  s1.output["data"] = "test_value_1";
  samples.push_back(s1);

  // 错误样本测试
  DemoSampleResult s2;
  s2.request_id = 9002;
  s2.status = 5;
  s2.latency_ms = 3.5;
  s2.error = "Mock inference error for sample";
  samples.push_back(s2);

  std::string err;
  int ret = writer.WriteResults(samples, 6.0, &err);
  EXPECT_EQ(ret, 0) << "Error: " << err;

  // 验证结果文件存在
  std::string target_dir = writer.GetTargetOutputDir();
  std::string jsonl_path = target_dir + "/results.jsonl";
  std::string summary_path = target_dir + "/summary.json";

  EXPECT_TRUE(std::filesystem::exists(jsonl_path));
  EXPECT_TRUE(std::filesystem::exists(summary_path));

  // 验证 JSONL 文件行数与内容 (含错误样本)
  std::ifstream j_ifs(jsonl_path);
  std::string line;
  int count = 0;
  while (std::getline(j_ifs, line)) {
    if (!line.empty()) {
      count++;
      auto obj = nlohmann::json::parse(line);
      EXPECT_EQ(obj["profile"], "test_profile_unit");
      EXPECT_FALSE(obj.contains("biz"));
      if (obj["request_id"] == 9002) {
        EXPECT_EQ(obj["status"], 5);
        EXPECT_EQ(obj["error"], "Mock inference error for sample");
      }
    }
  }
  EXPECT_EQ(count, 2);

  // 验证 summary.json
  {
    std::ifstream s_ifs(summary_path);
    nlohmann::json summary_obj;
    s_ifs >> summary_obj;
    EXPECT_FALSE(summary_obj.contains("biz"));
    EXPECT_EQ(summary_obj["total_samples"], 2);
    EXPECT_EQ(summary_obj["success_count"], 1);
    EXPECT_EQ(summary_obj["failed_count"], 1);
  }

  // 测试 --append 模式下的追加写入与累计口径校验
  opts.append = true;
  ResultWriter append_writer(opts);

  std::vector<DemoSampleResult> append_samples;
  DemoSampleResult s3;
  s3.request_id = 9003;
  s3.status = 0;
  s3.latency_ms = 4.0;
  s3.output["data"] = "test_value_3";
  append_samples.push_back(s3);

  ret = append_writer.WriteResults(append_samples, 4.0, &err);
  EXPECT_EQ(ret, 0) << "Append write failed: " << err;

  // 验证 summary.json 准确记录了 3 条样本的累计统计
  {
    std::ifstream s_ifs2(summary_path);
    nlohmann::json summary_obj2;
    s_ifs2 >> summary_obj2;
    EXPECT_EQ(summary_obj2["total_samples"], 3);
    EXPECT_EQ(summary_obj2["success_count"], 2);
    EXPECT_EQ(summary_obj2["failed_count"], 1);
    EXPECT_EQ(summary_obj2["run_samples"], 1);
    EXPECT_EQ(summary_obj2["run_success_count"], 1);
    EXPECT_EQ(summary_obj2["run_failed_count"], 0);
  }
}

TEST(DemoRunnerTest,
     ResultWriterUsesProfileThenConfigStemThenDefaultDirectory) {
  KiteDemoDirectory temporary;
  DemoOptions options;
  options.config_path = "nested/selected_pipeline.conf";
  options.output_dir = temporary.path.string();
  options.profile = "chosen_profile";
  EXPECT_EQ(ResultWriter(options).GetTargetOutputDir(),
            (temporary.path / "chosen_profile").string());
  options.profile.clear();
  ResultWriter writer(options);
  EXPECT_EQ(writer.GetTargetOutputDir(),
            (temporary.path / "selected_pipeline").string());
  DemoSampleResult sample;
  sample.request_id = 19;
  sample.output = {{"answer", "retained"}};
  std::string error;
  ASSERT_EQ(writer.WriteResults({sample}, 0.0, &error), 0) << error;
  std::ifstream results(temporary.path / "selected_pipeline/results.jsonl");
  ASSERT_TRUE(results.good());
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(results, line)));
  const auto record = nlohmann::json::parse(line);
  EXPECT_EQ(record["profile"], "selected_pipeline");
  EXPECT_EQ(record["output"], sample.output);
  std::ifstream summary_file(temporary.path / "selected_pipeline/summary.json");
  ASSERT_TRUE(summary_file.good());
  EXPECT_EQ(nlohmann::json::parse(summary_file)["profile"],
            "selected_pipeline");
  options.config_path.clear();
  EXPECT_EQ(ResultWriter(options).GetTargetOutputDir(),
            (temporary.path / "default").string());
}

// SDK 预检返回载体信息，失败时清除两侧过期结果。
TEST(DemoRunnerTest, ConfigIoResolutionAndFailureClearsContract) {
  struct Case {
    const char* config;
    const char* name;
    const char* input;
    const char* output;
  };
  std::string error;
  OperatorIoContract contract;
  for (const auto& entry :
       {Case{"demo/fixtures/mock/pipeline_entity_extract.conf",
             "entity_extract", "CompanyOperatorEntityInput",
             "CompanyOperatorEntityOutput"},
        Case{"demo/fixtures/mock/pipeline_doc_qa.conf", "doc_qa",
             "CompanyOperatorDocInput", "CompanyOperatorDocOutput"},
        Case{"demo/fixtures/mock/pipeline_doc_qa_rerank.conf", "doc_qa",
             "CompanyOperatorDocInput", "CompanyOperatorDocOutput"},
        Case{"configs/pipeline_keyword_match_rules.conf", "keyword_match",
             "CompanyOperatorKeywordInput", "CompanyOperatorKeywordOutput"}}) {
    DemoOptions options;
    options.config_path = entry.config;
    ASSERT_TRUE(ResolveConfigIo(options, &contract, &error)) << error;
    ASSERT_EQ(contract.inputs.size(), 1U);
    ASSERT_EQ(contract.outputs.size(), 1U);
    EXPECT_EQ(contract.inputs[0].name, entry.name);
    EXPECT_EQ(contract.inputs[0].type_name, entry.input);
    EXPECT_EQ(contract.outputs[0].name, entry.name);
    EXPECT_EQ(contract.outputs[0].type_name, entry.output);
    EXPECT_TRUE(contract.inputs[0].required);
    EXPECT_TRUE(contract.outputs[0].required);
    EXPECT_FALSE(contract.inputs[0].service_type.has_value());
    EXPECT_FALSE(contract.outputs[0].service_type.has_value());
  }

  KiteDemoDirectory temporary;
  const auto bad_conf = temporary.path / "invalid.conf";
  std::ofstream(bad_conf) << R"({"pipe_path":123})";
  DemoOptions options;
  options.config_path = bad_conf.string();
  EXPECT_FALSE(ResolveConfigIo(options, &contract, &error));
  EXPECT_TRUE(contract.inputs.empty());
  EXPECT_TRUE(contract.outputs.empty());
  EXPECT_NE(error.find("pipe_path"), std::string::npos);
  EXPECT_FALSE(ResolveConfigIo(options, nullptr, &error));

  char err_buf[256]{};
  ASSERT_EQ(ResolveOperatorConfigIo(
                ".", "demo/fixtures/mock/pipeline_entity_extract.conf",
                &contract, err_buf, sizeof(err_buf)),
            0);
  ASSERT_EQ(contract.inputs.size(), 1U);
  ASSERT_EQ(contract.outputs.size(), 1U);
  EXPECT_EQ(ResolveOperatorConfigIo(".", "non_existent_conf_file.conf",
                                    &contract, err_buf, sizeof(err_buf)),
            -2);
  EXPECT_TRUE(contract.inputs.empty());
  EXPECT_TRUE(contract.outputs.empty());
}

// P1-2: 测试显式指定不存在或非法的 Control 文件 Fail-Closed
TEST(DemoRunnerTest, FailClosedOnMissingOrInvalidControlFile) {
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);

  KiteDemoDirectory temporary;
  for (const char* profile : {"keyword_match_rules", "ocr_invoice_qa_mock"}) {
    SCOPED_TRACE(profile);
    DemoOptions opts;
    opts.profile = profile;
    std::string err;
    ASSERT_EQ(LoadAndMergeProfiles("demo/profiles.json", opts, &opts, &err), 0)
        << err;
    opts.control_cmd = std::string(profile) == "keyword_match_rules" ? 1 : 2;

    opts.control_file = (temporary.path / "missing.json").string();
    EXPECT_EQ(RunOperatorDemo(opts), 3);
    opts.control_file = "";
    EXPECT_EQ(RunOperatorDemo(opts), 3);

    const auto payload_path = temporary.path / "control.json";
    opts.control_file = payload_path.string();
    for (const char* payload : {"NOT_VALID_JSON{{{", "[]"}) {
      std::ofstream(payload_path) << payload;
      EXPECT_EQ(RunOperatorDemo(opts), 3);
    }
  }

  ops.DeInit();
}

TEST(DemoRunnerTest, GenericControlCommandChangesCustomNodeOutput) {
  KiteDemoDirectory temporary;
  llm_edgeflow::test::WriteControlTestPipeline(temporary.path);
  std::ofstream(temporary.path / "input.txt") << "sample\n";
  std::ofstream(temporary.path / "control.json") << R"({"prefix":"VIP:"})";
  DemoOptions options;
  options.config_path = (temporary.path / "pipeline.conf").string();
  options.dataset_path = (temporary.path / "input.txt").string();
  options.output_dir = (temporary.path / "results").string();
  options.control_cmd = 2000000041;
  options.control_file = (temporary.path / "control.json").string();
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  ASSERT_EQ(RunOperatorDemo(options), 0);
  std::ifstream results(temporary.path / "results/pipeline/results.jsonl");
  ASSERT_TRUE(results.good());
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(results, line)));
  const auto record = nlohmann::json::parse(line);
  EXPECT_EQ(record["status"], 0);
  EXPECT_EQ(record["output"]["is_hit"], true);
  EXPECT_NE(record.dump().find("PREFIX_APPLIED"), std::string::npos);
  options.control_file.reset();
  EXPECT_EQ(RunOperatorDemo(options),
            3);  // 绝不替换为默认规则 payload。
  options.control_file = (temporary.path / "control.json").string();
  options.control_cmd = 19999;
  EXPECT_EQ(RunOperatorDemo(options), 5);
  EXPECT_EQ(ops.DeInit(), 0);
}

TEST(DemoRunnerTest, PreservesMixedSampleStatusesAndFailureCounts) {
  KiteDemoDirectory temporary;
  std::ifstream pipeline_file("configs/pipeline_keyword_match_rules.json");
  ASSERT_TRUE(pipeline_file.good());
  auto pipeline = nlohmann::json::parse(pipeline_file);
  pipeline["pipeline"][0]["node_type"] = "TestDemoStatusNode";
  pipeline["pipeline"][0].erase("config");
  const auto pipeline_path = temporary.path / "pipeline.json";
  std::ofstream(pipeline_path) << pipeline.dump();
  std::ifstream conf_file("configs/pipeline_keyword_match_rules.conf");
  ASSERT_TRUE(conf_file.good());
  auto conf = nlohmann::json::parse(conf_file);
  conf["pipe_path"] = "pipeline.json";
  std::ofstream(temporary.path / "pipeline.conf") << conf.dump();
  std::ofstream(temporary.path / "input.txt") << "success\nfail\n";

  DemoOptions options;
  options.config_path = (temporary.path / "pipeline.conf").string();
  options.dataset_path = (temporary.path / "input.txt").string();
  options.output_dir = temporary.path.string();
  options.batch_size = 2;
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  ASSERT_EQ(RunOperatorDemo(options), 0);

  std::ifstream results(temporary.path / "pipeline/results.jsonl");
  ASSERT_TRUE(results.good());
  std::string line;
  for (int index = 0; index < 2; ++index) {
    ASSERT_TRUE(static_cast<bool>(std::getline(results, line)));
    const auto sample = nlohmann::json::parse(line);
    EXPECT_EQ(sample["request_id"], 20001 + index);
    EXPECT_EQ(sample["status"], index == 0 ? 0 : -42);
  }
  EXPECT_FALSE(static_cast<bool>(std::getline(results, line)));
  std::ifstream summary_file(temporary.path / "pipeline/summary.json");
  ASSERT_TRUE(summary_file.good());
  const auto summary = nlohmann::json::parse(summary_file);
  EXPECT_EQ(summary["total_samples"], 2);
  EXPECT_EQ(summary["success_count"], 1);
  EXPECT_EQ(summary["failed_count"], 1);
  EXPECT_EQ(ops.DeInit(), 0);
}

TEST(DemoRunnerTest, KeywordControlProfileAndCliFileOverride) {
  KiteDemoDirectory temporary;
  const auto dataset = temporary.path / "input.txt";
  std::ofstream(dataset) << "初始化自检\nVIP专员\n";
  DemoOptions options;
  options.config_path = "configs/pipeline_keyword_match_rules.conf";
  options.dataset_path = dataset.string();
  options.output_dir = temporary.path.string();
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  auto run_and_read = [&]() {
    EXPECT_EQ(RunOperatorDemo(options), 0);
    std::ifstream results(
        std::filesystem::path(ResultWriter(options).GetTargetOutputDir()) /
        "results.jsonl");
    std::vector<nlohmann::json> samples;
    std::string line;
    while (std::getline(results, line))
      samples.push_back(nlohmann::json::parse(line));
    return samples;
  };

  const auto configured = run_and_read();
  ASSERT_EQ(configured.size(), 2U);
  EXPECT_EQ(configured[0]["output"]["is_hit"], true);
  EXPECT_NE(configured[0].dump().find("SYSTEM_INIT"), std::string::npos);
  EXPECT_EQ(configured[1]["output"]["is_hit"], false);

  DemoOptions cli;
  cli.profile = "keyword_match_control";
  cli.dataset_path = dataset.string();
  cli.has_dataset_path = true;
  cli.output_dir = temporary.path.string();
  std::string error;
  ASSERT_EQ(LoadAndMergeProfiles("demo/profiles.json", cli, &options, &error),
            0)
      << error;
  EXPECT_EQ(options.suite, "real");
  EXPECT_EQ(options.control_cmd, 1);
  EXPECT_EQ(options.control_file, "data/keyword_match_control.json");
  const auto example = run_and_read();
  ASSERT_EQ(example.size(), 2U);
  EXPECT_EQ(example[0]["output"]["is_hit"], false);
  EXPECT_EQ(example[1]["output"]["is_hit"], true);
  EXPECT_NE(example[1].dump().find("VIP_SERVICE"), std::string::npos);

  const auto control_path = temporary.path / "control.json";
  std::ofstream(control_path) << R"({"categories":{"FILE_RULE":["自检"]}})";
  cli.control_file = control_path.string();
  cli.has_control_file = true;
  ASSERT_EQ(LoadAndMergeProfiles("demo/profiles.json", cli, &options, &error),
            0)
      << error;
  EXPECT_EQ(options.control_file, control_path.string());
  const auto explicit_file = run_and_read();
  ASSERT_EQ(explicit_file.size(), 2U);
  EXPECT_EQ(explicit_file[0]["output"]["is_hit"], true);
  EXPECT_NE(explicit_file[0].dump().find("FILE_RULE"), std::string::npos);
  EXPECT_EQ(explicit_file[1]["output"]["is_hit"], false);
  EXPECT_EQ(ops.DeInit(), 0);
}

TEST(DemoRunnerTest, OcrDemoAppliesExplicitControlBeforeProcessing) {
  KiteDemoDirectory temporary;
  DemoOptions cli;
  cli.profile = "ocr_invoice_qa_mock";
  cli.output_dir = temporary.path.string();
  DemoOptions options;
  std::string error;
  ASSERT_EQ(LoadAndMergeProfiles("demo/profiles.json", cli, &options, &error),
            0)
      << error;
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);

  auto read_sample = [&]() {
    std::ifstream results(temporary.path / "ocr_invoice_qa_mock/results.jsonl");
    std::string line;
    std::getline(results, line);
    return nlohmann::json::parse(line);
  };
  ASSERT_EQ(RunOperatorDemo(options), 0);
  const auto original = read_sample();
  EXPECT_EQ(original["request_id"], 60001);
  EXPECT_EQ(original["output"]["extracted_invoice"]["invoice_code"],
            "011002200111");

  const auto control_path = temporary.path / "control.json";
  std::ofstream(control_path) << R"({"template":"提取实体：{{primary}}"})";
  options.control_file = control_path.string();
  EXPECT_EQ(RunOperatorDemo(options), 3);
  options.control_cmd = 2;
  ASSERT_EQ(RunOperatorDemo(options), 0);
  const auto updated = read_sample();
  EXPECT_EQ(updated["status"], 0);
  EXPECT_EQ(updated["request_id"], 60001);
  EXPECT_TRUE(updated["output"]["extracted_invoice"]["nouns"].is_array());
  EXPECT_FALSE(updated["output"]["extracted_invoice"].contains("invoice_code"));

  options.control_cmd = 19999;
  EXPECT_EQ(RunOperatorDemo(options), 5);
  options.control_cmd = 0;
  EXPECT_EQ(RunOperatorDemo(options), 3);
  options.control_cmd = 2;
  options.control_file.reset();
  EXPECT_EQ(RunOperatorDemo(options), 3);
  EXPECT_EQ(ops.DeInit(), 0);
}

TEST(DemoRunnerTest, ControlCommandCliAndProfilePrecedence) {
  KiteDemoDirectory temporary;
  auto write_profile = [&](const nlohmann::json& cmd) {
    const nlohmann::json document = {
        {"profiles",
         {{"control",
           {{"config", "configs/pipeline_keyword_match_rules.conf"},
            {"dataset", "data/corpus_keyword_match.txt"},
            {"control_cmd", cmd},
            {"control_file", "payload.json"}}}}}};
    std::ofstream(temporary.path / "profiles.json") << document.dump();
  };
  const std::string path = (temporary.path / "profiles.json").string();
  write_profile(2000000041);
  DemoOptions cli, merged;
  cli.profile = "control";
  std::string error;
  ASSERT_EQ(LoadAndMergeProfiles(path, cli, &merged, &error), 0) << error;
  EXPECT_EQ(merged.control_cmd, 2000000041);
  EXPECT_EQ(merged.control_file, "payload.json");
  const char* args[] = {"alg_demo", "--profile", "control", "--control-cmd",
                        "2000000042"};
  ASSERT_EQ(ParseCommandLine(5, const_cast<char**>(args), &cli, &error), 0);
  ASSERT_EQ(LoadAndMergeProfiles(path, cli, &merged, &error), 0) << error;
  EXPECT_EQ(merged.control_cmd, 2000000042);
  for (const char* value : {"0", "-1", "2000000041abc", "2147483648"}) {
    const char* invalid[] = {"alg_demo", "--control-cmd", value};
    DemoOptions options;
    EXPECT_EQ(
        ParseCommandLine(3, const_cast<char**>(invalid), &options, &error), 2);
  }
  for (const auto& invalid :
       {nlohmann::json(0), nlohmann::json("2000000041"), nlohmann::json(1.5),
        nlohmann::json(2147483648ULL)}) {
    write_profile(invalid);
    nlohmann::json output;
    EXPECT_EQ(LoadAndValidateProfilesDocument(path, &output, &error), 3);
  }
}

// P1-1: 测试多样本按 batch_size 进行分块调度 (Chunking)
TEST(DemoRunnerTest, OperatorBatchChunking) {
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);

  DemoOptions opts;
  opts.profile = "keyword_match_rules";
  std::string err;
  int ret = LoadAndMergeProfiles("demo/profiles.json", opts, &opts, &err);
  ASSERT_EQ(ret, 0);

  // 指定 batch_size = 1 (数据集有 2 条样本，必须分 2 批执行)
  opts.batch_size = 1;
  opts.output_dir = "./results/test_chunking_out";

  EXPECT_EQ(RunOperatorDemo(opts), 0);

  // 验证结果文件中有 2 条记录
  std::string jsonl_path =
      opts.output_dir + "/keyword_match_rules/results.jsonl";
  std::ifstream ifs(jsonl_path);
  std::string line;
  int sample_count = 0;
  while (std::getline(ifs, line)) {
    if (!line.empty()) sample_count++;
  }
  EXPECT_EQ(sample_count, 2);

  ops.DeInit();
}

TEST(DemoRunnerTest, OversizedProcessBatchFailsWithoutWritingSuccessResults) {
  KiteDemoDirectory temporary;
  std::ofstream dataset(temporary.path / "input.txt");
  for (int i = 0; i < 65; ++i) dataset << "初始化自检\n";
  dataset.close();
  DemoOptions options;
  options.config_path = "configs/pipeline_keyword_match_rules.conf";
  options.dataset_path = (temporary.path / "input.txt").string();
  options.output_dir = temporary.path.string();
  options.batch_size = 65;
  options.depth_num = 65;
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  EXPECT_EQ(RunOperatorDemo(options), 5);
  EXPECT_EQ(ops.DeInit(), 0);
  EXPECT_FALSE(std::filesystem::exists(
      temporary.path / "pipeline_keyword_match_rules/results.jsonl"));
  EXPECT_FALSE(std::filesystem::exists(
      temporary.path / "pipeline_keyword_match_rules/summary.json"));
}

// 8. 测试全业务 Demo Case 执行 (集成测试)
TEST(DemoRunnerTest, EndToEndAllMockSmokeBusinesses) {
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);

  std::vector<std::string> smoke_profiles = {
      "audio_asr_intent_mock", "dialogue_audit_mock",
      "doc_qa_custom_mock",    "doc_qa_mock",
      "doc_qa_rerank_mock",    "entity_extract_custom_mock",
      "entity_extract_mock",   "keyword_match_rules",
      "ocr_invoice_qa_mock"};

  nlohmann::json profiles;
  std::string err;
  ASSERT_EQ(
      LoadAndValidateProfilesDocument("demo/profiles.json", &profiles, &err), 0)
      << err;
  EXPECT_EQ(SelectProfilesForSuite(profiles, "smoke"), smoke_profiles);

  for (const auto& prof_name : smoke_profiles) {
    DemoOptions cli_opt;
    cli_opt.profile = prof_name;
    cli_opt.output_dir = "./results/test_ci_out";

    DemoOptions merged_opt;
    int ret = MergeProfileOptions(profiles, cli_opt, &merged_opt, &err);
    ASSERT_EQ(ret, 0) << "Profile merge failed for " << prof_name << ": "
                      << err;

    int run_ret = RunOperatorDemo(merged_opt);
    EXPECT_EQ(run_ret, 0) << "Execution failed for profile: " << prof_name;
  }

  ops.DeInit();
}

TEST(DemoRunnerTest, DeploymentProfilesFileSelection) {
  const char* args[] = {"alg_demo", "--profiles-file",
                        "demo/profiles_kite.json", "--profile",
                        "entity_extract_kite"};
  DemoOptions options;
  std::string error;
  ASSERT_EQ(ParseCommandLine(5, const_cast<char**>(args), &options, &error), 0);
  EXPECT_EQ(options.profiles_file, "demo/profiles_kite.json");
  DemoOptions merged;
  ASSERT_EQ(
      LoadAndMergeProfiles(options.profiles_file, options, &merged, &error), 0)
      << error;
  EXPECT_EQ(merged.config_path, "configs/pipeline_entity_extract_kite.conf");
  nlohmann::json profiles;
  ASSERT_EQ(
      LoadAndValidateProfilesDocument(options.profiles_file, &profiles, &error),
      0);
  EXPECT_EQ(SelectProfilesForSuite(profiles, "real").size(), 8U);
  const char* missing[] = {"alg_demo", "--profiles-file"};
  EXPECT_EQ(ParseCommandLine(2, const_cast<char**>(missing), &options, &error),
            2);
}

TEST(DemoRunnerTest, RealKiteDeploymentProfiles) {
  const char* enabled = std::getenv("LLM_EDGEFLOW_TEST_KITELLM_DEMOS");
  if (!enabled || std::string(enabled) != "1")
    GTEST_SKIP()
        << "Set LLM_EDGEFLOW_TEST_KITELLM_DEMOS=1 after fetching --kite models";
  ASSERT_TRUE(llm_edgeflow::BackendRegistry::Instance().Find("kite_llm"));
  nlohmann::json profiles;
  std::string error;
  ASSERT_EQ(LoadAndValidateProfilesDocument("demo/profiles_kite.json",
                                            &profiles, &error),
            0)
      << error;
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  for (const auto& name : SelectProfilesForSuite(profiles, "real")) {
    DemoOptions cli;
    cli.profile = name;
    cli.output_dir = "results/kite-deployment-tests";
    DemoOptions options;
    const int merged = MergeProfileOptions(profiles, cli, &options, &error);
    EXPECT_EQ(merged, 0) << error;
    if (merged) continue;
    EXPECT_EQ(RunOperatorDemo(options), 0) << name;
  }
  EXPECT_EQ(ops.DeInit(), 0);
}
