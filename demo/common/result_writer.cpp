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
      options_.profile.empty() ? options_.biz : options_.profile;
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
      options_.profile.empty() ? options_.biz : options_.profile;
  std::string jsonl_path = (fs::path(dir_path) / "results.jsonl").string();
  std::string summary_path = (fs::path(dir_path) / "summary.json").string();

  std::string tmp_jsonl = jsonl_path + ".tmp";
  std::string tmp_summary = summary_path + ".tmp";

  std::string target_jsonl = options_.append ? jsonl_path : tmp_jsonl;

  auto open_mode = options_.append ? (std::ios::out | std::ios::app)
                                   : (std::ios::out | std::ios::trunc);

  int run_success_count = 0;
  int run_failed_count = 0;
  double run_latencies_sum = 0.0;

  // 1. 写入样本至 results.jsonl (含错误样本记录)
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
        run_success_count++;
      } else {
        run_failed_count++;
      }
      run_latencies_sum += sample.latency_ms;

      nlohmann::json record;
      record["schema_version"] = 1;
      record["profile"] = profile_name;
      record["biz"] = options_.biz;
      record["request_id"] = sample.request_id;
      record["status"] = sample.status;
      record["latency_ms"] = sample.latency_ms;
      if (sample.status != 0) {
        record["error"] =
            sample.error.empty() ? "Sample execution failed" : sample.error;
      }
      record["output"] =
          sample.output.is_null() ? nlohmann::json::object() : sample.output;

      ofs << record.dump() << "\n";
    }
    ofs.flush();
    if (!ofs.good()) {
      if (error_msg) *error_msg = "Write error while writing jsonl results";
      return 6;
    }
  }

  // 2. 如果是非追加模式，先执行 jsonl 的原子重命名
  if (!options_.append) {
    fs::rename(tmp_jsonl, jsonl_path, ec);
    if (ec) {
      if (error_msg) {
        *error_msg = "Failed to rename temp jsonl to target: " + ec.message();
      }
      return 6;
    }
  }

  // 3. 计算统计摘要口径 (在 --append 模式下精确从完整 results.jsonl
  // 扫描累计口径)
  int cum_total_samples = 0;
  int cum_success_count = 0;
  int cum_failed_count = 0;
  double cum_latency_sum = 0.0;

  if (options_.append) {
    std::ifstream scan_ifs(jsonl_path);
    std::string line;
    while (std::getline(scan_ifs, line)) {
      if (line.empty()) continue;
      try {
        auto obj = nlohmann::json::parse(line);
        cum_total_samples++;
        int s = obj.value("status", 0);
        if (s == 0) {
          cum_success_count++;
        } else {
          cum_failed_count++;
        }
        cum_latency_sum += obj.value("latency_ms", 0.0);
      } catch (...) {
        // 忽略异常行
      }
    }
  } else {
    cum_total_samples = static_cast<int>(results.size());
    cum_success_count = run_success_count;
    cum_failed_count = run_failed_count;
    cum_latency_sum =
        (total_duration_ms > 0.0) ? total_duration_ms : run_latencies_sum;
  }

  // 4. 写入 summary.json (统一使用 .tmp 原子写入替换)
  {
    std::ofstream ofs_sum(tmp_summary, std::ios::out | std::ios::trunc);
    if (!ofs_sum.is_open()) {
      if (error_msg) {
        *error_msg = "Failed to open summary temp file: " + tmp_summary;
      }
      return 6;
    }

    nlohmann::json summary;
    summary["schema_version"] = 1;
    summary["profile"] = profile_name;
    summary["biz"] = options_.biz;
    summary["config_path"] = options_.config_path;
    summary["dataset_path"] = options_.dataset_path;
    summary["total_samples"] = cum_total_samples;
    summary["success_count"] = cum_success_count;
    summary["failed_count"] = cum_failed_count;
    summary["total_latency_ms"] = cum_latency_sum;
    summary["avg_latency_ms"] =
        cum_total_samples > 0 ? (cum_latency_sum / cum_total_samples) : 0.0;

    if (options_.append) {
      summary["run_samples"] = static_cast<int>(results.size());
      summary["run_success_count"] = run_success_count;
      summary["run_failed_count"] = run_failed_count;
      summary["run_latency_ms"] =
          (total_duration_ms > 0.0) ? total_duration_ms : run_latencies_sum;
    }

    ofs_sum << std::setw(2) << summary << "\n";
    ofs_sum.flush();
    if (!ofs_sum.good()) {
      if (error_msg) *error_msg = "Write error while writing summary JSON";
      return 6;
    }
  }

  fs::rename(tmp_summary, summary_path, ec);
  if (ec) {
    if (error_msg) {
      *error_msg = "Failed to rename temp summary to target: " + ec.message();
    }
    return 6;
  }

  std::cout << "[ResultWriter] Results saved to: " << jsonl_path << std::endl;
  std::cout << "[ResultWriter] Summary saved to: " << summary_path << std::endl;
  return 0;
}

}  // namespace alg_demo
