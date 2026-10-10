#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "demo/common/dataset_reader.h"
#include "demo/common/demo_io_registry.h"
#include "demo/common/demo_options.h"
#include "demo/common/result_writer.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "nlohmann/json.hpp"

namespace alg_demo {

/**
 * @brief 自动解析 model_root 和 relative cfg_file_name
 * (支持跨运行路径与父级查找)
 */
inline bool ResolveModelRootAndConfig(const std::string& conf_path,
                                      std::string* out_model_root,
                                      std::string* out_cfg_rel) {
  std::string resolved = ResolvePath(conf_path);
  std::error_code ec;
  std::filesystem::path abs_conf =
      std::filesystem::weakly_canonical(resolved, ec);
  if (ec || !std::filesystem::exists(abs_conf)) {
    abs_conf = std::filesystem::absolute(resolved);
  }

  std::filesystem::path rel_p(conf_path);
  if (rel_p.is_absolute()) {
    if (out_model_root) *out_model_root = abs_conf.parent_path().string();
    if (out_cfg_rel) *out_cfg_rel = abs_conf.filename().string();
    return true;
  }

  std::vector<std::string> parts;
  for (const auto& part : rel_p) {
    if (part != ".") {
      parts.push_back(part.string());
    }
  }

  std::filesystem::path base = abs_conf;
  for (size_t i = 0; i < parts.size(); ++i) {
    base = base.parent_path();
  }

  if (out_model_root) *out_model_root = base.string();
  if (out_cfg_rel) {
    std::filesystem::path rel_combined;
    for (size_t i = 0; i < parts.size(); ++i) {
      rel_combined /= parts[i];
    }
    *out_cfg_rel = rel_combined.string();
  }
  return true;
}

inline bool ResolveConfigIo(const DemoOptions& options,
                            llm_edgeflow::operator_api::OperatorIoContract* out,
                            std::string* error) {
  std::string root;
  std::string config;
  ResolveModelRootAndConfig(options.config_path, &root, &config);
  char diagnostic[512] = {};
  const int ret = llm_edgeflow::operator_api::ResolveOperatorConfigIo(
      root.c_str(), config.c_str(), out, diagnostic, sizeof(diagnostic));
  if (ret == 0) return true;
  if (error) {
    *error = diagnostic[0] ? diagnostic
                           : "Operator config resolution failed (code " +
                                 std::to_string(ret) + ")";
  }
  return false;
}

/**
 * @brief Operator 句柄 RAII 生命周期管理器
 */
struct OperatorHandleGuard {
  llm_edgeflow::operator_api::OperatorFunc ops;
  void* handle = nullptr;

  OperatorHandleGuard(llm_edgeflow::operator_api::OperatorFunc f, void* h)
      : ops(f), handle(h) {}

  ~OperatorHandleGuard() {
    if (handle) {
      ops.Destroy(handle);
      handle = nullptr;
    }
  }

  OperatorHandleGuard(const OperatorHandleGuard&) = delete;
  OperatorHandleGuard& operator=(const OperatorHandleGuard&) = delete;
};

// Control 必须显式提供命令号和 JSON 文件。
inline int ApplyOperatorControl(
    const DemoOptions& options,
    const llm_edgeflow::operator_api::OperatorFunc& ops, void* handle) {
  using namespace llm_edgeflow::operator_api;
  if (!options.control_file && !options.control_cmd) return 0;
  if (!options.control_file || options.control_file->empty() ||
      !options.control_cmd || *options.control_cmd <= 0) {
    std::cerr << "[OperatorRunner ERROR] Control requires a positive "
                 "--control-cmd and a non-empty --control-file"
              << std::endl;
    return 3;
  }
  std::string payload;
  std::string error;
  if (!ReadTextFile(*options.control_file, &payload, &error)) {
    std::cerr << "[OperatorRunner ERROR] " << error << std::endl;
    return 3;
  }
  const auto parsed = nlohmann::json::parse(payload, nullptr, false);
  if (!parsed.is_object()) {
    std::cerr
        << "[OperatorRunner ERROR] Control file must contain a JSON object"
        << std::endl;
    return 3;
  }
  ControlJsonParam parameter{*options.control_cmd, payload.c_str()};
  if (ops.Control(handle, static_cast<int>(ControlCommand::kJson),
                  &parameter) != 0) {
    std::cerr << "[OperatorRunner ERROR] ops.Control failed: "
              << GetOperatorLastError() << std::endl;
    return 5;
  }
  return 0;
}

/**
 * @brief 统一校验配置、解析芯片与模型路径并创建 Operator 实例
 * @return 0 成功，3 参数/配置错误，5 Operator 创建失败。
 */
inline int CreateOperatorInstance(
    const DemoOptions& options, std::string_view logger_prefix,
    llm_edgeflow::operator_api::OperatorFunc* out_ops, void** out_handle) {
  using namespace llm_edgeflow::operator_api;

  if (!out_ops || !out_handle) {
    return 3;
  }
  *out_handle = nullptr;

  ComputePlatform chip_type = ComputePlatform::kUnknown;
  if (!ParseComputePlatform(options.chip, &chip_type)) {
    std::cerr << "[" << logger_prefix
              << " ERROR] Unsupported compute platform / chip: " << options.chip
              << std::endl;
    return 3;
  }

  std::string model_root;
  std::string cfg_rel;
  ResolveModelRootAndConfig(options.config_path, &model_root, &cfg_rel);

  *out_ops = Get_LLM_EDGEFLOW_OperatorTable();

  int max_batch_size = options.batch_size > 0 ? options.batch_size : 1;
  uint32_t requested_depth = options.depth_num > 0 ? options.depth_num : 25;
  if (requested_depth < static_cast<uint32_t>(max_batch_size)) {
    requested_depth = static_cast<uint32_t>(max_batch_size);
  }

  CreateParam param{};
  param.model_path = model_root.c_str();
  param.cfg_file_name = cfg_rel.c_str();
  param.device_id = options.device_id;
  param.compute_platform = chip_type;
  param.max_frame_depth = requested_depth;

  int ret = out_ops->Create(out_handle, &param);
  if (ret != 0 || !*out_handle) {
    std::string op_err = GetOperatorLastError();
    std::cerr << "[" << logger_prefix
              << " ERROR] Failed ops.Create with conf: " << options.config_path
              << " (Operator error: " << op_err << ")" << std::endl;
    if (op_err.find("max_frame_depth") != std::string::npos) {
      std::cerr << "[OperatorRunner HINT] 输出池深度超过上限；用 "
                   "alg_pipeline_tool resolve-conf <conf> --root <root> "
                   "--depth <depth> 查看 max_frame_depth_limit"
                << std::endl;
    }
    return 5;
  }

  return 0;
}

inline int RunOperatorDemo(const DemoOptions& options) {
  using namespace llm_edgeflow::operator_api;
  OperatorIoContract contract;
  std::string error;
  if (!ResolveConfigIo(options, &contract, &error)) {
    std::cerr << "[OperatorRunner ERROR] " << error << std::endl;
    return 3;
  }
  auto& registry = DemoIoRegistry::Instance();
  if (registry.HasConflict()) {
    std::cerr << "[OperatorRunner ERROR] Conflicting Demo I/O registrations"
              << std::endl;
    return 3;
  }
  std::string input_types;
  for (const auto& entry : contract.inputs) {
    if (!input_types.empty()) input_types += ",";
    input_types += entry.type_name;
  }
  const auto build = registry.FindInput(input_types);
  std::vector<ShowResultFn> displays;
  std::map<std::string, size_t> output_type_counts;
  for (const auto& entry : contract.outputs) {
    displays.push_back(registry.FindOutput(entry.type_name));
    ++output_type_counts[entry.type];
  }
  std::vector<std::string> output_keys;
  for (const auto& entry : contract.outputs)
    output_keys.push_back(output_type_counts[entry.type] > 1
                              ? entry.name + "." + entry.type
                              : "demo." + entry.type);
  if (!build ||
      std::find(displays.begin(), displays.end(), nullptr) != displays.end()) {
    std::cerr << "[OperatorRunner ERROR] Unsupported I/O carriers: "
              << input_types << std::endl;
    for (const auto& type : registry.ListInputs())
      std::cerr << "  input: " << type << '\n';
    for (const auto& type : registry.ListOutputs())
      std::cerr << "  output: " << type << '\n';
    return 3;
  }
  DemoRequestBatch requests;
  const int build_ret = build(options, contract.inputs, &requests);
  if (build_ret != 0) return build_ret;
  if (requests.requests.empty()) {
    std::cerr << "[OperatorRunner ERROR] Dataset has no requests" << std::endl;
    return 4;
  }
  OperatorFunc ops{};
  void* handle = nullptr;
  const int create_ret =
      CreateOperatorInstance(options, "OperatorRunner", &ops, &handle);
  if (create_ret != 0) return create_ret;
  OperatorHandleGuard guard(ops, handle);
  const int control_ret = ApplyOperatorControl(options, ops, handle);
  if (control_ret != 0) return control_ret;

  std::vector<DemoSampleResult> samples(requests.requests.size());
  const size_t batch_size = options.batch_size > 0 ? options.batch_size : 1;
  double duration = 0.0;
  const nlohmann::json empty_info = nlohmann::json::object();
  for (size_t offset = 0; offset < requests.requests.size();
       offset += batch_size) {
    const size_t count =
        std::min(batch_size, requests.requests.size() - offset);
    NamedIoBatch inputs(requests.requests.begin() + offset,
                        requests.requests.begin() + offset + count);
    NamedIoBatch outputs(count);
    for (auto& row : outputs) {
      for (const auto& key : output_keys) row[key] = {};
    }
    const auto start = std::chrono::steady_clock::now();
    const int ret = ops.Process(handle, inputs, outputs);
    const double elapsed = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    duration += elapsed;
    if (ret != 0) {
      std::cerr << "[OperatorRunner ERROR] ops.Process failed: "
                << GetOperatorLastError() << std::endl;
      return 5;
    }
    for (size_t i = 0; i < count; ++i) {
      auto& sample = samples[offset + i];
      sample.latency_ms = elapsed / count;
      sample.output = nlohmann::json::object();
      const auto& info = offset + i < requests.request_info.size()
                             ? requests.request_info[offset + i]
                             : empty_info;
      std::vector<nlohmann::json> parts(contract.outputs.size(),
                                        nlohmann::json::object());
      std::map<std::string, size_t> field_counts;
      for (size_t j = 0; j < contract.outputs.size(); ++j) {
        const auto& entry = contract.outputs[j];
        const auto found = outputs[i].find(output_keys[j]);
        if (found == outputs[i].end() || !found->second) {
          if (!entry.required) continue;
          std::cerr << "[OperatorRunner ERROR] Missing output: " << entry.type
                    << std::endl;
          return 5;
        }
        int32_t status = 0;
        displays[j](found->second.get(), info, &status, &parts[j]);
        if (status != 0 && sample.status == 0) sample.status = status;
        for (const auto& field : parts[j].items()) ++field_counts[field.key()];
      }
      for (size_t j = 0; j < parts.size(); ++j) {
        const auto& entry = contract.outputs[j];
        const auto prefix = output_type_counts[entry.type] > 1
                                ? entry.name + "." + entry.type
                                : entry.type;
        for (const auto& field : parts[j].items()) {
          const auto key = field_counts[field.key()] > 1
                               ? prefix + "." + field.key()
                               : field.key();
          sample.output[key] = field.value();
        }
      }
      std::cout << "[OperatorRunner] Row " << offset + i << ": "
                << sample.output.dump() << std::endl;
    }
    // 先把结果复制为 JSON，再归还本批次的所有输出租约。
    outputs.clear();
  }
  ResultWriter writer(options);
  const int write_ret = writer.WriteResults(samples, duration, &error);
  if (write_ret != 0)
    std::cerr << "[OperatorRunner ERROR] " << error << std::endl;
  return write_ret;
}

}  // namespace alg_demo
