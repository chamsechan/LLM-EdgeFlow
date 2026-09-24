#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/node_registry.h"
#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"
#include "edgeflow/log.h"
#include "edgeflow/operator/interface.h"
#include "engine/backend_registry.h"
#include "nlohmann/json.hpp"
#include "nodes/node_base.h"
#include "tests/support/control_test_utils.h"

using namespace alg_demo;
using namespace llm_edgeflow::operator_api;

namespace llm_edgeflow::test {
namespace {

class TestDemoStatusNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "TestDemoStatusNode";
  TestDemoStatusNode()
      : NodeBase(kNodeType), input_("text"), output_("matches") {}

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
      result.match_result_json = "{}";
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
  // Keep the artifact inside the deployment root without a symlink escape.
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
      node["config"].erase("fallback_json");
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
  std::string resolution_error;
  ASSERT_TRUE(ResolveConfigBiz(&opts, &resolution_error)) << resolution_error;
  const auto* desc = DemoRegistry::Instance().Find(opts.biz);
  ASSERT_NE(desc, nullptr);
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  const int result = desc->run(opts);
  EXPECT_EQ(ops.DeInit(), 0);
  ASSERT_EQ(result, 0);

  std::ifstream results(temporary.path / "output" / opts.biz / "results.jsonl");
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
  EXPECT_TRUE(opts.biz.empty());
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
  for (const char* flag : {"--biz", "-b", "--business", "--unknown-option"}) {
    SCOPED_TRACE(flag);
    const char* argv[] = {"alg_demo", flag, "entity_extract_v1"};
    DemoOptions opts;
    std::string err;
    EXPECT_EQ(ParseCommandLine(3, const_cast<char**>(argv), &opts, &err), 2);
    EXPECT_NE(err.find("Unknown CLI option"), std::string::npos);
  }
}

TEST(DemoRunnerTest, ConfigAloneResolvesRegisteredRunner) {
  const char* argv[] = {"alg_demo", "--config",
                        "configs/pipeline_keyword_match_rules.conf"};
  DemoOptions options;
  std::string error;
  ASSERT_EQ(ParseCommandLine(3, const_cast<char**>(argv), &options, &error), 0);
  EXPECT_TRUE(options.biz.empty());
  ASSERT_TRUE(ResolveConfigBiz(&options, &error)) << error;
  EXPECT_EQ(options.biz, "keyword_match_v1");
  ASSERT_NE(DemoRegistry::Instance().Find(options.biz), nullptr);
}

// CLI errors retain exit code 2.
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

// 3. Profile loading and remaining CLI overrides.
TEST(DemoRunnerTest, ProfileLoadAndMerge) {
  DemoOptions cli_opts;
  cli_opts.profile = "entity_extract_mock";

  DemoOptions merged;
  std::string err;
  int ret = LoadAndMergeProfiles("demo/profiles.json", cli_opts, &merged, &err);
  EXPECT_EQ(ret, 0) << "Error: " << err;

  EXPECT_TRUE(merged.biz.empty());
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
        {{"schema_version", 2}, {"profiles", {{"execution", value}}}});
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

TEST(DemoRunnerTest, ConfigOverrideDeterminesResolvedBiz) {
  DemoOptions cli;
  cli.profile = "entity_extract_mock";
  cli.biz = "stale_resolved_value";
  cli.config_path = "configs/pipeline_keyword_match_rules.conf";
  cli.has_config_path = true;
  DemoOptions merged;
  std::string error;
  ASSERT_EQ(LoadAndMergeProfiles("demo/profiles.json", cli, &merged, &error), 0)
      << error;
  EXPECT_TRUE(merged.biz.empty());
  EXPECT_EQ(merged.config_path, cli.config_path);
  ASSERT_TRUE(ResolveConfigBiz(&merged, &error)) << error;
  EXPECT_EQ(merged.biz, "keyword_match_v1");
  ASSERT_NE(DemoRegistry::Instance().Find(merged.biz), nullptr);
}

TEST(DemoRunnerTest, ProfileRejectsUnknownFieldsAndInvalidShapes) {
  KiteDemoDirectory temporary;
  const auto path = temporary.path / "profiles.json";
  const std::vector<std::pair<nlohmann::json, std::string>> cases = {
      {{{"config", "configs/pipeline_keyword_match_rules.conf"},
        {"dataset", "data/corpus_keyword_match.txt"},
        {"biz", "keyword_match_v1"}},
       "Unknown Profile field: 'biz'"},
      {{{"config", "configs/pipeline_keyword_match_rules.conf"},
        {"dataset", "data/corpus_keyword_match.txt"},
        {"datset", "data/corpus_keyword_match.txt"}},
       "Unknown Profile field: 'datset'"},
      {nlohmann::json::array(), "must be an object"},
      {nullptr, "must be an object"}};
  for (const auto& [profile, diagnostic] : cases) {
    SCOPED_TRACE(profile.dump());
    std::ofstream(path) << nlohmann::json{{"schema_version", 2},
                                          {"profiles", {{"invalid", profile}}}};
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

  // Case 1: 缺少 schema_version
  {
    std::ofstream ofs(temp_invalid_json);
    ofs << "{\"profiles\": {}}";
  }
  DemoOptions cli_opts;
  cli_opts.profile = "foo";
  DemoOptions merged;
  std::string err;
  EXPECT_EQ(LoadAndMergeProfiles(temp_invalid_json, cli_opts, &merged, &err),
            3);

  // Case 2: 非法 suite 字符串
  {
    std::ofstream ofs(temp_invalid_json);
    ofs << R"({
      "schema_version": 2,
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
      "schema_version": 2,
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
      "schema_version": 2,
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

// 4. 测试 DemoRegistry 注册与冲突检测
TEST(DemoRunnerTest, RegistryLookupAndConflictDetection) {
  auto& reg = DemoRegistry::Instance();
  struct RestoreRegistry {
    std::vector<DemoDescriptor> saved;
    ~RestoreRegistry() {
      auto& registry = DemoRegistry::Instance();
      registry.ResetForTesting();
      for (const auto& descriptor : saved)
        EXPECT_TRUE(registry.Register(descriptor));
    }
  } restore{reg.ListDescriptors()};

  // Demo dispatch uses the Pipeline business identity directly.
  for (const char* biz :
       {"entity_extract_v1", "keyword_match_v1", "smart_doc_qa_v1",
        "dialogue_compliance_audit_v1", "multimodal_ocr_invoice_qa",
        "speech_audio_asr_intent_slot", "dense_cross_rerank_scoring",
        "translate_v1"}) {
    SCOPED_TRACE(biz);
    const auto* descriptor = reg.Find(biz);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->biz_name, biz);
    EXPECT_NE(descriptor->run, nullptr);
  }
  for (const char* alias :
       {"entity_extract", "keyword_match", "doc_qa", "dialogue_audit",
        "ocr_doc_qa", "audio_asr", "cross_rerank", "translate"}) {
    EXPECT_EQ(reg.Find(alias), nullptr) << alias;
  }

  EXPECT_FALSE(reg.Register({"entity_extract_v1", "Duplicate",
                             [](const DemoOptions&) { return 0; }}));
  EXPECT_TRUE(reg.HasConflict());
  EXPECT_FALSE(
      reg.Register({"", "Empty", [](const DemoOptions&) { return 0; }}));
  EXPECT_FALSE(reg.Register({"dummy_new", "NullFunc", nullptr}));
  EXPECT_EQ(reg.Find("dummy_new"), nullptr);

  reg.ResetForTesting();
  EXPECT_EQ(reg.Find("entity_extract_v1"), nullptr);
  EXPECT_TRUE(reg.Register(
      {"new_domain_v1", "Domain", [](const DemoOptions&) { return 0; }}));
  const auto* added = reg.Find("new_domain_v1");
  ASSERT_NE(added, nullptr);
  EXPECT_EQ(added->biz_name, "new_domain_v1");
  EXPECT_EQ(reg.Find("entity_extract_v1"), nullptr);
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
    options.batch_size = 2;  // Exercise multi-request custom-node execution.
    std::string resolution_error;
    ASSERT_TRUE(ResolveConfigBiz(&options, &resolution_error))
        << resolution_error;
    const auto* descriptor = DemoRegistry::Instance().Find(options.biz);
    ASSERT_NE(descriptor, nullptr);
    ASSERT_EQ(descriptor->run(options), 0);
    std::ifstream results(temporary.path / profile / "results.jsonl");
    ASSERT_TRUE(results.good());
    std::string line;
    size_t index = 0;
    while (std::getline(results, line)) {
      const auto sample = nlohmann::json::parse(line);
      EXPECT_EQ(sample["status"], 0);
      const auto& output = sample["output"];
      if (options.biz == "entity_extract_v1") {
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
    EXPECT_EQ(index, options.biz == "entity_extract_v1" ? 1U : 2U);
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
  DemoOptions opts;
  opts.profile = "test_profile_unit";
  opts.biz = "unit_test";
  opts.output_dir = "./results/test_out_unit";
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
      EXPECT_EQ(obj["schema_version"], 1);
      EXPECT_EQ(obj["profile"], "test_profile_unit");
      EXPECT_EQ(obj["biz"], "unit_test");
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
    EXPECT_EQ(summary_obj["schema_version"], 1);
    EXPECT_EQ(summary_obj["biz"], "unit_test");
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

// Native resolution supplies the business identity and clears stale results.
TEST(DemoRunnerTest, ConfigBizResolution) {
  std::string error;
  for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
           {"demo/fixtures/mock/pipeline_entity_extract.conf",
            "entity_extract_v1"},
           {"demo/fixtures/mock/pipeline_doc_qa.conf", "smart_doc_qa_v1"},
           {"demo/fixtures/mock/pipeline_doc_qa_rerank.conf",
            "smart_doc_qa_v1"},
           {"configs/pipeline_keyword_match_rules.conf", "keyword_match_v1"}}) {
    DemoOptions options;
    options.config_path = entry.first;
    options.biz = "stale_resolved_value";
    ASSERT_TRUE(ResolveConfigBiz(&options, &error)) << error;
    EXPECT_EQ(options.biz, entry.second);
  }

  KiteDemoDirectory temporary;
  const auto bad_conf = temporary.path / "invalid.conf";
  std::ofstream(bad_conf) << R"({"pipe_path":123})";
  DemoOptions options;
  options.config_path = bad_conf.string();
  options.biz = "stale_resolved_value";
  EXPECT_FALSE(ResolveConfigBiz(&options, &error));
  EXPECT_TRUE(options.biz.empty());
  EXPECT_NE(error.find("pipe_path"), std::string::npos);
  EXPECT_FALSE(ResolveConfigBiz(nullptr, &error));

  char err_buf[256]{};
  std::string biz;
  EXPECT_EQ(ResolveOperatorConfigBiz(
                ".", "demo/fixtures/mock/pipeline_entity_extract.conf", &biz,
                err_buf, sizeof(err_buf)),
            0);
  EXPECT_EQ(biz, "entity_extract_v1");
  EXPECT_EQ(ResolveOperatorConfigBiz(".", "non_existent_conf_file.conf", &biz,
                                     err_buf, sizeof(err_buf)),
            -2);
  EXPECT_TRUE(biz.empty());
}

// P1-2: 测试显式指定不存在或非法的 Control 文件 Fail-Closed
TEST(DemoRunnerTest, FailClosedOnMissingOrInvalidControlFile) {
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);

  KiteDemoDirectory temporary;
  for (const char* profile : {"keyword_match_rules", "ocr_doc_qa_mock"}) {
    SCOPED_TRACE(profile);
    DemoOptions opts;
    opts.profile = profile;
    std::string err;
    ASSERT_EQ(LoadAndMergeProfiles("demo/profiles.json", opts, &opts, &err), 0)
        << err;
    std::string resolution_error;
    ASSERT_TRUE(ResolveConfigBiz(&opts, &resolution_error)) << resolution_error;
    const auto* desc = DemoRegistry::Instance().Find(opts.biz);
    ASSERT_NE(desc, nullptr);

    opts.control_file = (temporary.path / "missing.json").string();
    EXPECT_EQ(desc->run(opts), 3);
    opts.control_file = "";
    EXPECT_EQ(desc->run(opts), 3);

    const auto payload_path = temporary.path / "control.json";
    opts.control_file = payload_path.string();
    for (const char* payload : {"NOT_VALID_JSON{{{", "[]"}) {
      std::ofstream(payload_path) << payload;
      EXPECT_EQ(desc->run(opts), 3);
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
  std::string resolution_error;
  ASSERT_TRUE(ResolveConfigBiz(&options, &resolution_error))
      << resolution_error;
  const auto* demo = DemoRegistry::Instance().Find(options.biz);
  ASSERT_NE(demo, nullptr);
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  ASSERT_EQ(demo->run(options), 0);
  std::ifstream results(temporary.path /
                        "results/keyword_match_v1/results.jsonl");
  ASSERT_TRUE(results.good());
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(results, line)));
  const auto record = nlohmann::json::parse(line);
  EXPECT_EQ(record["status"], 0);
  EXPECT_EQ(record["output"]["is_hit"], true);
  EXPECT_NE(record.dump().find("PREFIX_APPLIED"), std::string::npos);
  options.control_file.reset();
  EXPECT_EQ(demo->run(options),
            3);  // Never substitute a default rules payload.
  options.control_file = (temporary.path / "control.json").string();
  options.control_cmd = 19999;
  EXPECT_EQ(demo->run(options), 5);
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
  std::string resolution_error;
  ASSERT_TRUE(ResolveConfigBiz(&options, &resolution_error))
      << resolution_error;
  const auto* demo = DemoRegistry::Instance().Find(options.biz);
  ASSERT_NE(demo, nullptr);
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  ASSERT_EQ(demo->run(options), 0);

  std::ifstream results(temporary.path / "keyword_match_v1/results.jsonl");
  ASSERT_TRUE(results.good());
  std::string line;
  for (int index = 0; index < 2; ++index) {
    ASSERT_TRUE(static_cast<bool>(std::getline(results, line)));
    const auto sample = nlohmann::json::parse(line);
    EXPECT_EQ(sample["request_id"], 20001 + index);
    EXPECT_EQ(sample["status"], index == 0 ? 0 : -42);
  }
  EXPECT_FALSE(static_cast<bool>(std::getline(results, line)));
  std::ifstream summary_file(temporary.path / "keyword_match_v1/summary.json");
  ASSERT_TRUE(summary_file.good());
  const auto summary = nlohmann::json::parse(summary_file);
  EXPECT_EQ(summary["total_samples"], 2);
  EXPECT_EQ(summary["success_count"], 1);
  EXPECT_EQ(summary["failed_count"], 1);
  EXPECT_EQ(ops.DeInit(), 0);
}

TEST(DemoRunnerTest, ExampleControlIsExplicitAndFileControlTakesPrecedence) {
  KiteDemoDirectory temporary;
  const auto dataset = temporary.path / "input.txt";
  std::ofstream(dataset) << "初始化自检\nVIP专员\n";
  DemoOptions options;
  options.config_path = "configs/pipeline_keyword_match_rules.conf";
  options.dataset_path = dataset.string();
  options.output_dir = temporary.path.string();
  std::string resolution_error;
  ASSERT_TRUE(ResolveConfigBiz(&options, &resolution_error))
      << resolution_error;
  const auto* demo = DemoRegistry::Instance().Find(options.biz);
  ASSERT_NE(demo, nullptr);
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);
  auto run_and_read = [&]() {
    EXPECT_EQ(demo->run(options), 0);
    std::ifstream results(temporary.path / "keyword_match_v1/results.jsonl");
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

  options.example_control = true;
  const auto example = run_and_read();
  ASSERT_EQ(example.size(), 2U);
  EXPECT_EQ(example[0]["output"]["is_hit"], false);
  EXPECT_EQ(example[1]["output"]["is_hit"], true);
  EXPECT_NE(example[1].dump().find("VIP_SERVICE"), std::string::npos);

  const auto control_path = temporary.path / "control.json";
  std::ofstream(control_path) << R"({"categories":{"FILE_RULE":["自检"]}})";
  options.control_file = control_path.string();
  const auto explicit_file = run_and_read();
  ASSERT_EQ(explicit_file.size(), 2U);
  EXPECT_EQ(explicit_file[0]["output"]["is_hit"], true);
  EXPECT_NE(explicit_file[0].dump().find("FILE_RULE"), std::string::npos);
  EXPECT_EQ(explicit_file[1]["output"]["is_hit"], false);
  EXPECT_EQ(ops.DeInit(), 0);
}

TEST(DemoRunnerTest, ExampleControlCliAndRejectsRemovedFlag) {
  std::string error;
  {
    DemoOptions options;
    const char* args[] = {"alg_demo", "--example-control"};
    ASSERT_EQ(ParseCommandLine(2, const_cast<char**>(args), &options, &error),
              0)
        << error;
    EXPECT_TRUE(options.example_control);
  }
  {
    DemoOptions options;
    const char* args[] = {"alg_demo"};
    ASSERT_EQ(ParseCommandLine(1, const_cast<char**>(args), &options, &error),
              0)
        << error;
    EXPECT_FALSE(options.example_control);
  }
  {
    DemoOptions options;
    const char* args[] = {"alg_demo", "--no-default-control"};
    EXPECT_EQ(ParseCommandLine(2, const_cast<char**>(args), &options, &error),
              2);
    EXPECT_NE(error.find("Unknown CLI option"), std::string::npos);
  }
}

TEST(DemoRunnerTest, OcrDemoAppliesExplicitControlBeforeProcessing) {
  KiteDemoDirectory temporary;
  DemoOptions cli;
  cli.profile = "ocr_doc_qa_mock";
  cli.output_dir = temporary.path.string();
  DemoOptions options;
  std::string error;
  ASSERT_EQ(LoadAndMergeProfiles("demo/profiles.json", cli, &options, &error),
            0)
      << error;
  std::string resolution_error;
  ASSERT_TRUE(ResolveConfigBiz(&options, &resolution_error))
      << resolution_error;
  const auto* demo = DemoRegistry::Instance().Find(options.biz);
  ASSERT_NE(demo, nullptr);
  auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);

  auto read_sample = [&]() {
    std::ifstream results(temporary.path / "ocr_doc_qa_mock/results.jsonl");
    std::string line;
    std::getline(results, line);
    return nlohmann::json::parse(line);
  };
  ASSERT_EQ(demo->run(options), 0);
  const auto original = read_sample();
  EXPECT_EQ(original["request_id"], 60001);
  EXPECT_EQ(original["output"]["extracted_invoice"]["invoice_code"],
            "011002200111");

  const auto control_path = temporary.path / "control.json";
  std::ofstream(control_path) << R"({"template":"提取实体：{{primary}}"})";
  options.control_file = control_path.string();
  // OCR defaults to its registered prompt update command when no ID is given.
  ASSERT_EQ(demo->run(options), 0);
  const auto updated = read_sample();
  EXPECT_EQ(updated["status"], 0);
  EXPECT_EQ(updated["request_id"], 60001);
  EXPECT_TRUE(updated["output"]["extracted_invoice"]["nouns"].is_array());
  EXPECT_FALSE(updated["output"]["extracted_invoice"].contains("invoice_code"));

  options.control_cmd = 19999;
  EXPECT_EQ(demo->run(options), 5);
  options.control_cmd = 0;
  EXPECT_EQ(demo->run(options), 3);
  options.control_cmd = 2;
  options.control_file.reset();
  EXPECT_EQ(demo->run(options), 3);
  EXPECT_EQ(ops.DeInit(), 0);
}

TEST(DemoRunnerTest, ControlCommandCliAndProfilePrecedence) {
  KiteDemoDirectory temporary;
  auto write_profile = [&](const nlohmann::json& cmd) {
    const nlohmann::json document = {
        {"schema_version", 2},
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

  const auto* desc = DemoRegistry::Instance().Find("keyword_match_v1");
  ASSERT_NE(desc, nullptr);

  DemoOptions opts;
  opts.profile = "keyword_match_rules";
  std::string err;
  int ret = LoadAndMergeProfiles("demo/profiles.json", opts, &opts, &err);
  ASSERT_EQ(ret, 0);
  ASSERT_TRUE(ResolveConfigBiz(&opts, &err)) << err;

  // 指定 batch_size = 1 (数据集有 2 条样本，必须分 2 批执行)
  opts.batch_size = 1;
  opts.output_dir = "./results/test_chunking_out";

  EXPECT_EQ(desc->run(opts), 0);

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

// 8. 测试全业务 Demo Case 执行 (集成测试)
TEST(DemoRunnerTest, EndToEndAllMockSmokeBusinesses) {
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);

  std::vector<std::string> smoke_profiles = {
      "entity_extract_mock", "keyword_match_rules", "doc_qa_mock",
      "dialogue_audit_mock", "ocr_doc_qa_mock",     "audio_asr_mock"};

  nlohmann::json profiles;
  std::string err;
  ASSERT_EQ(
      LoadAndValidateProfilesDocument("demo/profiles.json", &profiles, &err), 0)
      << err;

  for (const auto& prof_name : smoke_profiles) {
    DemoOptions cli_opt;
    cli_opt.profile = prof_name;
    cli_opt.output_dir = "./results/test_ci_out";

    DemoOptions merged_opt;
    int ret = MergeProfileOptions(profiles, cli_opt, &merged_opt, &err);
    ASSERT_EQ(ret, 0) << "Profile merge failed for " << prof_name << ": "
                      << err;
    std::string resolution_error;
    ASSERT_TRUE(ResolveConfigBiz(&merged_opt, &resolution_error))
        << resolution_error;
    const auto* desc = DemoRegistry::Instance().Find(merged_opt.biz);
    ASSERT_NE(desc, nullptr) << "Business not registered: " << merged_opt.biz;

    int run_ret = desc->run(merged_opt);
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
  EXPECT_TRUE(merged.biz.empty());
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
    std::string resolution_error;
    ASSERT_TRUE(ResolveConfigBiz(&options, &resolution_error))
        << resolution_error;
    const auto* descriptor = DemoRegistry::Instance().Find(options.biz);
    EXPECT_NE(descriptor, nullptr);
    if (!descriptor) continue;
    EXPECT_EQ(descriptor->run(options), 0) << name;
  }
  EXPECT_EQ(ops.DeInit(), 0);
}
