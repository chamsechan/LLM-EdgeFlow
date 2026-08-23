#include "demo/common/result_writer.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace alg_demo {

ResultWriter::ResultWriter(const DemoOptions& options) : options_(options) {}

std::string ResultWriter::GetTargetOutputDir() const {
  std::filesystem::path root(options_.output_dir.empty() ? "./results"
                                                         : options_.output_dir);
  std::string sub_name =
      options_.profile.empty() ? options_.business : options_.profile;
  if (sub_name.empty()) sub_name = "default";
  return (root / sub_name).string();
}

int ResultWriter::WriteResults(const std::vector<DemoSampleResult>& results,
                               double total_duration_ms,
                               std::string* error_msg) {
  namespace fs = std::filesystem;
  std::string dir_path = GetTargetOutputDir();

  std::error_code ec;
  if (!fs::exists(dir_path, ec)) {
    if (!fs::create_directories(dir_path, ec)) {
      if (error_msg) {
        *error_msg = "Failed to create result directory '" + dir_path +
                     "': " + ec.message();
      }
      return 6;
    }
  }

  std::string profile_name =
      options_.profile.empty() ? options_.business : options_.profile;
  std::string jsonl_path = (fs::path(dir_path) / "results.jsonl").string();
  std::string summary_path = (fs::path(dir_path) / "summary.json").string();

  std::string tmp_jsonl = jsonl_path + ".tmp";
  std::string tmp_summary = summary_path + ".tmp";

  std::string target_jsonl = options_.append ? jsonl_path : tmp_jsonl;
  std::string target_summary = options_.append ? summary_path : tmp_summary;

  auto open_mode = options_.append ? (std::ios::out | std::ios::app)
                                   : (std::ios::out | std::ios::trunc);

  int success_count = 0;
  int failed_count = 0;
  double sample_latencies_sum = 0.0;

  {
    std::ofstream ofs(target_jsonl, open_mode);
    if (!ofs.is_open()) {
      if (error_msg) {
        *error_msg = "Failed to open output jsonl file: " + target_jsonl;
      }
      return 6;
    }

    for (const auto& sample : results) {
      if (sample.status == 0) {
        success_count++;
      } else {
        failed_count++;
      }
      sample_latencies_sum += sample.latency_ms;

      nlohmann::json record;
      record["schema_version"] = 1;
      record["profile"] = profile_name;
      record["business"] = options_.business;
      record["request_id"] = sample.request_id;
      record["status"] = sample.status;
      record["latency_ms"] = sample.latency_ms;
      if (sample.status != 0) {
        record["error"] = sample.error;
      }
      record["output"] = sample.output;

      ofs << record.dump() << "\n";
    }
    ofs.flush();
    if (!ofs.good()) {
      if (error_msg) *error_msg = "Write error while writing jsonl results";
      return 6;
    }
  }

  // 写入 summary.json
  {
    std::ofstream ofs_sum(target_summary, std::ios::out | std::ios::trunc);
    if (!ofs_sum.is_open()) {
      if (error_msg) {
        *error_msg = "Failed to open summary file: " + target_summary;
      }
      return 6;
    }

    nlohmann::json summary;
    summary["schema_version"] = 1;
    summary["profile"] = profile_name;
    summary["business"] = options_.business;
    summary["config_path"] = options_.config_path;
    summary["dataset_path"] = options_.dataset_path;
    summary["total_samples"] = static_cast<int>(results.size());
    summary["success_count"] = success_count;
    summary["failed_count"] = failed_count;
    summary["total_latency_ms"] =
        (total_duration_ms > 0.0) ? total_duration_ms : sample_latencies_sum;
    summary["avg_latency_ms"] =
        results.empty() ? 0.0 : (sample_latencies_sum / results.size());

    ofs_sum << std::setw(2) << summary << "\n";
    ofs_sum.flush();
    if (!ofs_sum.good()) {
      if (error_msg) *error_msg = "Write error while writing summary JSON";
      return 6;
    }
  }

  // 原子重命名替换
  if (!options_.append) {
    fs::rename(tmp_jsonl, jsonl_path, ec);
    if (ec) {
      if (error_msg) {
        *error_msg = "Failed to rename temp jsonl to target: " + ec.message();
      }
      return 6;
    }
    fs::rename(tmp_summary, summary_path, ec);
    if (ec) {
      if (error_msg) {
        *error_msg = "Failed to rename temp summary to target: " + ec.message();
      }
      return 6;
    }
  }

  std::cout << "[ResultWriter] Results saved to: " << jsonl_path << std::endl;
  std::cout << "[ResultWriter] Summary saved to: " << summary_path << std::endl;
  return 0;
}

}  // namespace alg_demo
