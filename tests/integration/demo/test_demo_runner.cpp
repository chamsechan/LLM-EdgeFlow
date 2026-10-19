#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "demo/common/demo_io_registry.h"
#include "demo/common/operator_runner.h"
#include "demo/common/result_writer.h"
#include "nlohmann/json.hpp"

namespace alg_demo {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

class SharedDemoConsumerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (fs::temp_directory_path() / "demo-consumer-XXXXXX").string();
    char* created = mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    directory = created;
    dataset = directory / "input.txt";
    Write(dataset,
          "# ignored comment\n  初始化完成  \n\nordinary text\n自检完成\n");
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  }
  static void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
  }
  static Json Read(const fs::path& path) {
    std::ifstream stream(path);
    Json document;
    stream >> document;
    return document;
  }
  int Run(const std::vector<std::string>& arguments) {
    const auto log = directory / "demo.log";
    const pid_t child = fork();
    if (child == 0) {
      FILE* output = freopen(log.c_str(), "w", stdout);
      FILE* error = freopen(log.c_str(), "a", stderr);
      if (!output || !error) _exit(125);
      std::vector<std::string> strings{EDGEFLOW_DEMO_BINARY};
      strings.insert(strings.end(), arguments.begin(), arguments.end());
      std::vector<char*> argv;
      for (auto& string : strings) argv.push_back(string.data());
      argv.push_back(nullptr);
      execv(argv.front(), argv.data());
      _exit(126);
    }
    if (child < 0) return -1;
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
  }
  std::vector<std::string> Arguments() const {
    return {"--config",     "configs/pipeline_keyword_match_rules.conf",
            "--dataset",    dataset.string(),
            "--output-dir", (directory / "results").string()};
  }
  std::vector<Json> Records() const {
    std::ifstream stream(directory /
                         "results/pipeline_keyword_match_rules/results.jsonl");
    std::vector<Json> records;
    std::string line;
    while (std::getline(stream, line)) records.push_back(Json::parse(line));
    return records;
  }
  fs::path directory, dataset;
};

TEST_F(SharedDemoConsumerTest,
       BinaryProducesOneCopiedResponsePerRowAndActualSampleCounts) {
  ASSERT_EQ(Run(Arguments()), 0);
  const auto records = Records();
  ASSERT_EQ(records.size(), 3u);
  for (const auto& record : records) {
    EXPECT_EQ(record.at("status"), 0);
    EXPECT_TRUE(record.at("output").is_object());
  }
  EXPECT_EQ(records[0]["output"]["is_hit"], 1);
  EXPECT_EQ(records[1]["output"]["is_hit"], 0);
  const auto summary =
      Read(directory / "results/pipeline_keyword_match_rules/summary.json");
  EXPECT_EQ(summary["total_samples"], 3);
  EXPECT_EQ(summary["success_count"], 3);
  EXPECT_EQ(summary["failed_count"], 0);
  EXPECT_EQ(summary["dataset_path"], dataset.string());
  EXPECT_FALSE(fs::exists(
      directory / "results/pipeline_keyword_match_rules/results.jsonl.tmp"));
}

TEST_F(SharedDemoConsumerTest,
       ExplicitControlIsAppliedBeforeProcessingAndCopiedToResults) {
  const auto control = directory / "control.json";
  Write(control, R"({"categories":{"DEMO":["ordinary"]}})");
  auto arguments = Arguments();
  arguments.insert(arguments.end(),
                   {"--control-cmd", "1", "--control-file", control.string()});
  ASSERT_EQ(Run(arguments), 0);
  const auto records = Records();
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0]["output"]["is_hit"], 0);
  EXPECT_EQ(records[1]["output"]["is_hit"], 1);
}

TEST_F(SharedDemoConsumerTest,
       DatasetAndControlFailuresPublishNoResultsAndNextRunSucceeds) {
  fs::remove(dataset);
  EXPECT_EQ(Run(Arguments()), 4);
  EXPECT_FALSE(fs::exists(directory / "results"));
  Write(dataset, "初始化完成\n");
  const auto control = directory / "bad-control.json";
  Write(control, "[]");
  auto arguments = Arguments();
  arguments.insert(arguments.end(),
                   {"--control-cmd", "1", "--control-file", control.string()});
  EXPECT_EQ(Run(arguments), 3);
  EXPECT_FALSE(fs::exists(directory / "results"));
  EXPECT_EQ(Run(Arguments()), 0);
  EXPECT_EQ(Records().size(), 1u);
}

TEST_F(SharedDemoConsumerTest,
       UnsupportedCliExecutionOverrideFailsBeforeRunning) {
  auto arguments = Arguments();
  arguments.insert(arguments.end(), {"--batch-size", "8"});
  EXPECT_EQ(Run(arguments), 2);
  EXPECT_FALSE(fs::exists(directory / "results"));
}

TEST_F(SharedDemoConsumerTest,
       ResultDirectoryFailureReturnsWriterExitAndPreservesHostFile) {
  const auto blocked = directory / "blocked";
  Write(blocked, "host-owned bytes");
  auto arguments = Arguments();
  arguments.back() = blocked.string();
  EXPECT_EQ(Run(arguments), 6);
  std::ifstream stream(blocked);
  std::string retained;
  std::getline(stream, retained);
  EXPECT_EQ(retained, "host-owned bytes");
  EXPECT_FALSE(fs::exists(directory / "results"));
  EXPECT_EQ(Run(Arguments()), 0);
  EXPECT_EQ(Records().size(), 3u);
}

TEST_F(SharedDemoConsumerTest,
       WriterAppendCountsRecordedFailuresRatherThanProcessExit) {
  DemoOptions options;
  options.output_dir = (directory / "writer").string();
  options.profile = "display";
  std::string error;
  ASSERT_EQ(ResultWriter(options).WriteResults(
                {{0, 2.0, "", {{"text", "first"}}}}, 2.0, &error),
            0)
      << error;
  options.append = true;
  const Json payload = {{"unexpected", Json::array({false, "中文"})}};
  ASSERT_EQ(ResultWriter(options).WriteResults(
                {{-4, 3.0, "sample rejected", payload}}, 3.0, &error),
            0)
      << error;
  const auto summary = Read(directory / "writer/display/summary.json");
  EXPECT_EQ(summary["total_samples"], 2);
  EXPECT_EQ(summary["success_count"], 1);
  EXPECT_EQ(summary["failed_count"], 1);
  EXPECT_EQ(summary["run_samples"], 1);
  EXPECT_EQ(summary["run_failed_count"], 1);
  std::ifstream stream(directory / "writer/display/results.jsonl");
  std::string first, second;
  ASSERT_TRUE(static_cast<bool>(std::getline(stream, first)));
  ASSERT_TRUE(static_cast<bool>(std::getline(stream, second)));
  EXPECT_EQ(Json::parse(second)["output"], payload);
  EXPECT_EQ(Json::parse(second)["error"], "sample rejected");
}
}  // namespace
}  // namespace alg_demo
