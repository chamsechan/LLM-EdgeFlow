#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "company_alg_log.h"
#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/demo_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"
#include "nlohmann/json.hpp"
#include "operator/operator_interface.h"

using namespace alg_demo;
using namespace llm_edgeflow::operator_api;

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

}  // namespace

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
                        "--biz",
                        "entity_extract",
                        "--config",
                        "demo/fixtures/mock/pipeline_entity_extract.conf",
                        "--dataset",
                        "data/corpus_entity_extract.txt",
                        "--output-dir",
                        "./results/test_out",
                        "--batch-size",
                        "4",
                        "--device-id",
                        "1",
                        "--chip",
                        "cpu_generic",
                        "--depth",
                        "2",
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
  EXPECT_EQ(opts.biz, "entity_extract");
  EXPECT_EQ(opts.config_path,
            "demo/fixtures/mock/pipeline_entity_extract.conf");
  EXPECT_EQ(opts.dataset_path, "data/corpus_entity_extract.txt");
  EXPECT_EQ(opts.output_dir, "./results/test_out");
  EXPECT_EQ(opts.batch_size, 4);
  EXPECT_EQ(opts.device_id, 1);
  EXPECT_EQ(opts.chip, "cpu_generic");
  EXPECT_EQ(opts.depth_num, 2u);
  EXPECT_EQ(opts.suite, "smoke");
  EXPECT_TRUE(opts.has_batch_size);
  EXPECT_TRUE(opts.has_device_id);
  EXPECT_TRUE(opts.has_chip);
  EXPECT_TRUE(opts.has_suite);
  EXPECT_TRUE(opts.append);
  EXPECT_TRUE(opts.allow_fallback_sample);
}

TEST(DemoRunnerTest, CommandLineBizName) {
  const char* argv[] = {"alg_demo", "--biz", "entity_extract"};
  int argc = 3;

  DemoOptions opts;
  std::string err;
  int ret = ParseCommandLine(argc, const_cast<char**>(argv), &opts, &err);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(opts.biz, "entity_extract");
  EXPECT_TRUE(opts.has_biz);
}

TEST(DemoRunnerTest, RejectsLegacyBusinessFlag) {
  const char* argv[] = {"alg_demo", "--business", "entity_extract"};
  DemoOptions opts;
  std::string err;
  EXPECT_EQ(ParseCommandLine(3, const_cast<char**>(argv), &opts, &err), 2);
  EXPECT_NE(err.find("Unknown CLI option"), std::string::npos);
}

// P2-1: 测试 CLI 参数严格解析 (尾随字符拦截与错误退出码 2)
TEST(DemoRunnerTest, CommandLineParsingErrors) {
  DemoOptions opts;
  std::string err;

  // 未知参数
  const char* argv1[] = {"alg_demo", "--unknown-flag"};
  EXPECT_EQ(ParseCommandLine(2, const_cast<char**>(argv1), &opts, &err), 2);

  // 参数缺少值
  const char* argv2[] = {"alg_demo", "--profile"};
  EXPECT_EQ(ParseCommandLine(2, const_cast<char**>(argv2), &opts, &err), 2);

  // 非法 batch_size (负数)
  const char* argv3[] = {"alg_demo", "--batch-size", "-1"};
  EXPECT_EQ(ParseCommandLine(3, const_cast<char**>(argv3), &opts, &err), 2);

  // 非法 batch_size (超大数值溢出拦截)
  const char* argv3_overflow[] = {"alg_demo", "--batch-size", "4294967297"};
  EXPECT_EQ(
      ParseCommandLine(3, const_cast<char**>(argv3_overflow), &opts, &err), 2);

  // P2-1: 非法 batch_size (含尾随非法字符 "1abc")
  const char* argv3_trailing[] = {"alg_demo", "--batch-size", "1abc"};
  EXPECT_EQ(
      ParseCommandLine(3, const_cast<char**>(argv3_trailing), &opts, &err), 2);

  // P2-1: 非法 device-id (含尾随字符 "0xyz")
  const char* argv_dev_trailing[] = {"alg_demo", "--device-id", "0xyz"};
  EXPECT_EQ(
      ParseCommandLine(3, const_cast<char**>(argv_dev_trailing), &opts, &err),
      2);

  // P2-1: 非法 depth (含尾随字符 "2foo")
  const char* argv_depth_trailing[] = {"alg_demo", "--depth", "2foo"};
  EXPECT_EQ(
      ParseCommandLine(3, const_cast<char**>(argv_depth_trailing), &opts, &err),
      2);

  // 非法 chip
  const char* argv4[] = {"alg_demo", "--chip", "unsupported_dsp"};
  EXPECT_EQ(ParseCommandLine(3, const_cast<char**>(argv4), &opts, &err), 2);

  // 非法 suite
  const char* argv5[] = {"alg_demo", "--suite", "invalid_suite"};
  EXPECT_EQ(ParseCommandLine(3, const_cast<char**>(argv5), &opts, &err), 2);
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

  EXPECT_TRUE(ParseComputePlatform("nvidia_gpu", &type));
  EXPECT_EQ(type, ComputePlatform::kCuda);

  EXPECT_TRUE(ParseComputePlatform("cpu_generic", &type));
  EXPECT_EQ(type, ComputePlatform::kCpu);

  EXPECT_FALSE(ParseComputePlatform("invalid_hardware", &type));
  EXPECT_EQ(type, ComputePlatform::kUnknown);
}

// 3. 测试 Profile 加载、合并与 P1-1 CLI 显式默认值覆盖
TEST(DemoRunnerTest, ProfileLoadAndMerge) {
  DemoOptions cli_opts;
  cli_opts.profile = "entity_extract_mock";
  cli_opts.batch_size = 8;
  cli_opts.has_batch_size = true;

  DemoOptions merged;
  std::string err;
  int ret = LoadAndMergeProfiles("demo/profiles.json", cli_opts, &merged, &err);
  EXPECT_EQ(ret, 0) << "Error: " << err;

  EXPECT_EQ(merged.biz, "entity_extract");
  EXPECT_EQ(merged.config_path,
            "demo/fixtures/mock/pipeline_entity_extract.conf");
  EXPECT_EQ(merged.dataset_path, "data/corpus_entity_extract.txt");
  EXPECT_EQ(merged.chip, "cpu");
  EXPECT_EQ(merged.batch_size, 8);  // CLI 覆盖 Profile 的默认 1
}

// P1-1: 验证 CLI 显式传入默认值 (例如 --batch-size 1) 可以可靠覆盖 Profile 中非
// 1 的 batch_size
TEST(DemoRunnerTest, CliOverridesProfileEvenWithExplicitDefault) {
  const char* argv[] = {"alg_demo", "--profile", "cross_rerank_onnx",
                        "--batch-size", "1"};
  int argc = 5;

  DemoOptions cli_opts;
  std::string err;
  int ret = ParseCommandLine(argc, const_cast<char**>(argv), &cli_opts, &err);
  ASSERT_EQ(ret, 0);
  EXPECT_TRUE(cli_opts.has_batch_size);
  EXPECT_EQ(cli_opts.batch_size, 1);

  DemoOptions merged;
  ret = LoadAndMergeProfiles("demo/profiles.json", cli_opts, &merged, &err);
  ASSERT_EQ(ret, 0) << "Error: " << err;

  // 即使 CLI 显式给出默认值 1，也必须保留该显式覆盖语义。
  EXPECT_EQ(merged.batch_size, 1);
}

TEST(DemoRunnerTest, ProfileBizMismatchRejection) {
  DemoOptions cli_opts;
  cli_opts.profile = "entity_extract_mock";
  cli_opts.biz = "doc_qa";  // 冲突的业务名
  cli_opts.has_biz = true;

  DemoOptions merged;
  std::string err;
  int ret = LoadAndMergeProfiles("demo/profiles.json", cli_opts, &merged, &err);
  EXPECT_EQ(ret, 3);
  EXPECT_NE(err.find("Biz conflict"), std::string::npos);
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
          "biz": "entity_extract",
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
          "biz": "keyword_match",
          "config": "configs/pipeline_keyword_match.conf",
          "dataset": "data/corpus_keyword_match.txt",
          "suite": 123
        }
      }
    })";
  }
  EXPECT_EQ(LoadAndMergeProfiles(temp_invalid_json, cli_opts, &merged, &err),
            3);
  EXPECT_NE(err.find("suite"), std::string::npos);

  // 验证 GetProfilesForSuite 对该非法文件同样返回 3 且不崩溃
  std::vector<std::string> prof_list;
  EXPECT_EQ(GetProfilesForSuite(temp_invalid_json, "smoke", &prof_list, &err),
            3);

  // Case 4: batch_size 数值超界溢出 (4294967297) 防御拦截
  {
    std::ofstream ofs(temp_invalid_json);
    ofs << R"({
      "schema_version": 2,
      "profiles": {
        "overflow_prof": {
          "biz": "entity_extract",
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

  // 验证 7 大业务均已静态注册
  EXPECT_NE(reg.Find("entity_extract"), nullptr);
  EXPECT_NE(reg.Find("keyword_match"), nullptr);
  EXPECT_NE(reg.Find("doc_qa"), nullptr);
  EXPECT_NE(reg.Find("dialogue_audit"), nullptr);
  EXPECT_NE(reg.Find("ocr_doc_qa"), nullptr);
  EXPECT_NE(reg.Find("audio_asr"), nullptr);
  EXPECT_NE(reg.Find("cross_rerank"), nullptr);

  // 尝试重复注册已存在的业务名 -> 应该失败
  bool ok = reg.Register(
      {"entity_extract", "Duplicate", [](const DemoOptions&) { return 0; }});
  EXPECT_FALSE(ok);
  EXPECT_TRUE(reg.HasConflict());

  // 尝试注册非法空业务名 -> 应该失败
  ok = reg.Register({"", "Empty", [](const DemoOptions&) { return 0; }});
  EXPECT_FALSE(ok);

  // 尝试注册空函数 -> 应该失败
  ok = reg.Register({"dummy_new", "NullFunc", nullptr});
  EXPECT_FALSE(ok);
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

// 7. 测试 Config 与 Biz 匹配校验、P1-2 pipe_path 类型异常与 P1-3 Operator
// 公开预检 API
TEST(DemoRunnerTest, ConfigBizMatchValidation) {
  std::string err;

  // 正确匹配
  EXPECT_TRUE(
      ValidateConfigBizMatch("demo/fixtures/mock/pipeline_entity_extract.conf",
                             "entity_extract", &err));
  EXPECT_TRUE(ValidateConfigBizMatch("demo/fixtures/mock/pipeline_doc_qa.conf",
                                     "doc_qa", &err));
  EXPECT_TRUE(ValidateConfigBizMatch(
      "demo/fixtures/mock/pipeline_doc_qa_rerank.conf", "doc_qa", &err));
  EXPECT_TRUE(ValidateConfigBizMatch("configs/pipeline_keyword_match.conf",
                                     "keyword_match", &err));

  // 错误匹配 -> 快速失败
  EXPECT_FALSE(ValidateConfigBizMatch("demo/fixtures/mock/pipeline_doc_qa.conf",
                                      "entity_extract", &err));
  EXPECT_NE(err.find("Biz mismatch"), std::string::npos);

  // P1-3: cross_rerank 绝不应该匹配 doc_qa_rerank (即使名字里有 rerank)
  EXPECT_FALSE(ValidateConfigBizMatch(
      "demo/fixtures/mock/pipeline_doc_qa_rerank.conf", "cross_rerank", &err));
  EXPECT_NE(err.find("Biz mismatch"), std::string::npos);

  // P1-2 探针测试: .conf 中 pipe_path 为数字 123 (必须返回 false，不抛异常崩溃)
  std::string bad_conf_path = "./results/bad_pipe_path.conf";
  {
    std::ofstream ofs(bad_conf_path);
    ofs << R"({"data": {"pipe_path": 123}})";
  }
  EXPECT_FALSE(ValidateConfigBizMatch(bad_conf_path, "keyword_match", &err));
  EXPECT_NE(err.find("pipe_path"), std::string::npos);
  std::filesystem::remove(bad_conf_path);

  // 测试公开 API ValidateOperatorConfigBinding
  char err_buf[256] = {0};
  std::string root_dir = ".";
  if (std::filesystem::exists("../configs")) {
    root_dir = "..";
  }
  EXPECT_EQ(
      ValidateOperatorConfigBinding(
          root_dir.c_str(), "demo/fixtures/mock/pipeline_entity_extract.conf",
          static_cast<int32_t>(ALG_BIZ_TYPE_ENTITY_EXTRACT), err_buf,
          sizeof(err_buf)),
      0);
  EXPECT_EQ(
      ValidateOperatorConfigBinding(
          root_dir.c_str(), "demo/fixtures/mock/pipeline_entity_extract.conf",
          static_cast<int32_t>(ALG_BIZ_TYPE_DOC_QA), err_buf, sizeof(err_buf)),
      -3);
  EXPECT_EQ(
      ValidateOperatorConfigBinding(
          root_dir.c_str(), "non_existent_conf_file.conf",
          static_cast<int32_t>(ALG_BIZ_TYPE_DOC_QA), err_buf, sizeof(err_buf)),
      -2);
}

// P1-2: 测试显式指定不存在或非法的 Control 文件 Fail-Closed
TEST(DemoRunnerTest, FailClosedOnMissingOrInvalidControlFile) {
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);

  const auto* desc = DemoRegistry::Instance().Find("keyword_match");
  ASSERT_NE(desc, nullptr);

  DemoOptions opts;
  opts.profile = "keyword_match_mock";
  std::string err;
  int ret = LoadAndMergeProfiles("demo/profiles.json", opts, &opts, &err);
  ASSERT_EQ(ret, 0);

  // 显式指定不存在的 control 文件 -> 必须返回 3 报错退出
  opts.control_file = "/private/tmp/definitely_missing_control_file_123.json";
  opts.has_control_file = true;
  EXPECT_EQ(desc->run(opts), 3);

  // 显式指定非法 JSON 的 control 文件 -> 必须返回 3 报错退出
  std::string bad_json_file = "./results/bad_control.json";
  {
    std::ofstream ofs(bad_json_file);
    ofs << "NOT_VALID_JSON{{{";
  }
  opts.control_file = bad_json_file;
  EXPECT_EQ(desc->run(opts), 3);
  std::filesystem::remove(bad_json_file);

  ops.Deinit();
}

// P1-1: 测试多样本按 batch_size 进行分块调度 (Chunking)
TEST(DemoRunnerTest, OperatorBatchChunking) {
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);

  const auto* desc = DemoRegistry::Instance().Find("keyword_match");
  ASSERT_NE(desc, nullptr);

  DemoOptions opts;
  opts.profile = "keyword_match_mock";
  std::string err;
  int ret = LoadAndMergeProfiles("demo/profiles.json", opts, &opts, &err);
  ASSERT_EQ(ret, 0);

  // 指定 batch_size = 1 (数据集有 2 条样本，必须分 2 批执行)
  opts.batch_size = 1;
  opts.has_batch_size = true;
  opts.output_dir = "./results/test_chunking_out";

  EXPECT_EQ(desc->run(opts), 0);

  // 验证结果文件中有 2 条记录
  std::string jsonl_path =
      opts.output_dir + "/keyword_match_mock/results.jsonl";
  std::ifstream ifs(jsonl_path);
  std::string line;
  int sample_count = 0;
  while (std::getline(ifs, line)) {
    if (!line.empty()) sample_count++;
  }
  EXPECT_EQ(sample_count, 2);

  ops.Deinit();
}

// 8. 测试全业务 Demo Case 执行 (集成测试)
TEST(DemoRunnerTest, EndToEndAllMockSmokeBusinesses) {
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(ops.Init(), 0);

  std::vector<std::string> smoke_profiles = {
      "entity_extract_mock", "keyword_match_mock", "doc_qa_mock",
      "dialogue_audit_mock", "ocr_doc_qa_mock",    "audio_asr_mock"};

  for (const auto& prof_name : smoke_profiles) {
    DemoOptions cli_opt;
    cli_opt.profile = prof_name;
    cli_opt.output_dir = "./results/test_ci_out";

    DemoOptions merged_opt;
    std::string err;
    int ret =
        LoadAndMergeProfiles("demo/profiles.json", cli_opt, &merged_opt, &err);
    ASSERT_EQ(ret, 0) << "Profile merge failed for " << prof_name << ": "
                      << err;

    const auto* desc = DemoRegistry::Instance().Find(merged_opt.biz);
    ASSERT_NE(desc, nullptr) << "Business not registered: " << merged_opt.biz;

    int run_ret = desc->run(merged_opt);
    EXPECT_EQ(run_ret, 0) << "Execution failed for profile: " << prof_name;
  }

  ops.Deinit();
}
