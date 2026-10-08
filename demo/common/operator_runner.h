#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
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

/**
 * @brief 由 SDK 预检最终配置，取得 Demo 分派所需的 I/O 契约
 * (只读，不加载模型)。
 */
inline bool ResolveDemoContract(
    const std::string& config_path,
    llm_edgeflow::operator_api::OperatorIoContract* contract,
    std::string* error_msg) {
  std::string model_root;
  std::string cfg_rel;
  ResolveModelRootAndConfig(config_path, &model_root, &cfg_rel);

  char err_buf[512] = {0};
  int ret = llm_edgeflow::operator_api::ResolveOperatorConfigIo(
      model_root.c_str(), cfg_rel.c_str(), contract, err_buf, sizeof(err_buf));
  if (ret != 0) {
    if (error_msg) {
      *error_msg = (err_buf[0] != '\0')
                       ? std::string(err_buf)
                       : "Operator config resolution failed (code " +
                             std::to_string(ret) + ")";
    }
    return false;
  }
  return true;
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

/**
 * @brief 读取并校验显式 Control：control_file 与 control_cmd 必须同时给出。
 * 没有 Control 时 out_payload 为空；Demo 不提供任何内置默认命令。
 * @return 0 成功，3 参数/文件错误。
 */
inline int LoadControlPayload(const DemoOptions& options,
                              std::string* out_payload) {
  out_payload->clear();
  if (!options.control_file.has_value() && !options.control_cmd.has_value()) {
    return 0;
  }
  if (!options.control_file.has_value() || options.control_file->empty() ||
      !options.control_cmd.has_value() || *options.control_cmd <= 0) {
    std::cerr << "[OperatorRunner ERROR] --control-file and --control-cmd must "
                 "be given together: a non-empty file path and a positive "
                 "command ID"
              << std::endl;
    return 3;
  }

  std::string err;
  if (!ReadTextFile(*options.control_file, out_payload, &err)) {
    std::cerr << "[OperatorRunner ERROR] Failed to read explicit control file '"
              << *options.control_file << "': " << err << std::endl;
    return 3;
  }
  try {
    auto parsed_check = nlohmann::json::parse(*out_payload);
    if (!parsed_check.is_object()) {
      std::cerr << "[OperatorRunner ERROR] Control file content must be a "
                   "JSON object"
                << std::endl;
      return 3;
    }
  } catch (const std::exception& e) {
    std::cerr << "[OperatorRunner ERROR] Invalid JSON syntax in control file: "
              << e.what() << std::endl;
    return 3;
  }
  return 0;
}

/**
 * @brief 向已创建的句柄下发显式 Control；payload 为空时不做任何事。
 * @return 0 成功，5 Operator Control 失败。
 */
inline int ApplyOperatorControl(
    const DemoOptions& options,
    const llm_edgeflow::operator_api::OperatorFunc& ops, void* raw_handle,
    const std::string& control_payload) {
  using namespace llm_edgeflow::operator_api;
  if (control_payload.empty()) return 0;

  std::cout << "[OperatorRunner] Invoking ops.Control to dynamically push "
               "parameters..."
            << std::endl;
  ControlJsonParam ctrl_param{*options.control_cmd, control_payload.c_str()};
  int ctrl_ret = ops.Control(raw_handle, ControlCommand::kJson, &ctrl_param);
  if (ctrl_ret != 0) {
    std::cerr << "[OperatorRunner ERROR] ops.Control failed: code=" << ctrl_ret
              << " (Operator error: " << GetOperatorLastError() << ")"
              << std::endl;
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

/**
 * @brief 把各输出项的 output 字段合并成一条记录的 output。
 * 字段名在多个输出项中重复时，以输出项的 type 作前缀（"<type>.<字段>"）。
 */
inline nlohmann::json MergeOutputFields(
    const std::vector<std::string>& types,
    const std::vector<nlohmann::json>& fields) {
  nlohmann::json merged = nlohmann::json::object();
  if (fields.size() == 1) {
    if (fields[0].is_object()) merged = fields[0];
    return merged;
  }
  std::map<std::string, int> occurrences;
  for (const auto& item : fields) {
    if (!item.is_object()) continue;
    for (auto it = item.begin(); it != item.end(); ++it)
      ++occurrences[it.key()];
  }
  for (size_t i = 0; i < fields.size(); ++i) {
    if (!fields[i].is_object()) continue;
    for (auto it = fields[i].begin(); it != fields[i].end(); ++it) {
      const bool shared = occurrences[it.key()] > 1;
      merged[shared ? types[i] + "." + it.key() : it.key()] = it.value();
    }
  }
  return merged;
}

/**
 * @brief 按宿主结构（载体）运行一个部署配置的唯一公共流程：
 *   1. 用 ResolveOperatorConfigIo 取得 I/O 契约，找到请求构造和结果显示；
 *   2. 创建句柄，执行 Control；
 *   3. 按 batch_size 分批 Process，输入、输出 key 为各项的 "demo.<type>"；
 *   4. 对每条结果、每个输出项调用结果显示，合并写 results.jsonl/summary.json。
 * Demo 只构造载体、持有缓冲、调用 SDK、显示结果；校验和响应组装在 SDK。
 * @return 0 成功，3 参数/配置错误，4 数据集错误，5 Operator 失败，6
 * 写结果失败。
 */
inline int RunOperatorDemo(const DemoOptions& options) {
  using namespace llm_edgeflow::operator_api;

  std::string control_payload;
  if (int ret = LoadControlPayload(options, &control_payload); ret != 0) {
    return ret;
  }

  // 1. I/O 契约与载体。
  OperatorIoContract contract;
  std::string resolve_error;
  if (!ResolveDemoContract(options.config_path, &contract, &resolve_error)) {
    std::cerr << "[Config ERROR] " << resolve_error << std::endl;
    return 3;
  }

  const std::string input_carrier = DemoInputCarrierKey(contract.inputs);
  const BuildRequestsFn build_requests =
      DemoInputRegistry::Instance().Find(input_carrier);
  if (!build_requests) {
    std::cerr << "[Main ERROR] Unsupported input carrier '" << input_carrier
              << "' for config '" << options.config_path
              << "'. Supported input carriers:";
    for (const auto& name : DemoInputRegistry::Instance().List()) {
      std::cerr << "\n  - " << name;
    }
    std::cerr << std::endl;
    return 3;
  }
  std::vector<ShowResultFn> show_results;
  std::vector<std::string> output_types;
  std::string output_carriers;
  std::vector<std::string> label_names;  // 输出项的业务名，去重后命名结果目录
  std::string run_label;
  for (const auto& output : contract.outputs) {
    const ShowResultFn show =
        DemoOutputRegistry::Instance().Find(output.type_name);
    if (!show) {
      std::cerr << "[Main ERROR] Unsupported output carrier '"
                << output.type_name << "' for config '" << options.config_path
                << "'. Supported output carriers:";
      for (const auto& name : DemoOutputRegistry::Instance().List()) {
        std::cerr << "\n  - " << name;
      }
      std::cerr << std::endl;
      return 3;
    }
    show_results.push_back(show);
    output_types.push_back(output.type);
    if (!output_carriers.empty()) output_carriers += ",";
    output_carriers += output.type_name;
    if (std::find(label_names.begin(), label_names.end(), output.name) ==
        label_names.end()) {
      label_names.push_back(output.name);
      if (!run_label.empty()) run_label += "+";
      run_label += output.name;
    }
  }
  if (show_results.empty()) {
    std::cerr << "[Main ERROR] Config declares no output item: "
              << options.config_path << std::endl;
    return 3;
  }

  PrintBanner("LLM-EdgeFlow Demo", "Conf: " + options.config_path + " | " +
                                       input_carrier + " -> " +
                                       output_carriers);

  DemoRequestBatch batch;
  if (int ret = build_requests(options, contract.inputs, &batch); ret != 0) {
    return ret;
  }
  const size_t total_inputs = batch.requests.size();
  if (total_inputs == 0) {
    std::cerr << "[OperatorRunner ERROR] Inputs vector is empty." << std::endl;
    return 4;
  }
  batch.request_info.resize(total_inputs);

  // 2. 句柄与 Control。
  OperatorFunc ops{};
  void* raw_handle = nullptr;
  if (int ret =
          CreateOperatorInstance(options, "OperatorRunner", &ops, &raw_handle);
      ret != 0) {
    return ret;
  }
  OperatorHandleGuard guard(ops, raw_handle);
  if (int ret = ApplyOperatorControl(options, ops, raw_handle, control_payload);
      ret != 0) {
    return ret;
  }

  // 3. 分批 Process。
  std::vector<std::string> output_keys;
  output_keys.reserve(contract.outputs.size());
  for (const auto& output : contract.outputs) {
    output_keys.push_back(DemoIoKey(output));
  }

  const int max_batch_size = options.batch_size > 0 ? options.batch_size : 1;
  std::vector<DemoSampleResult> sample_results;
  sample_results.reserve(total_inputs);
  double total_elapsed_ms = 0.0;
  size_t processed_count = 0;

  std::cout << "\n>>> 执行结果验证 <<<" << std::endl;
  while (processed_count < total_inputs) {
    const size_t chunk_size = std::min(static_cast<size_t>(max_batch_size),
                                       total_inputs - processed_count);

    NamedIoBatch in_batch(chunk_size);
    NamedIoBatch out_batch(chunk_size);
    for (size_t i = 0; i < chunk_size; ++i) {
      in_batch[i] = batch.requests[processed_count + i];
      for (const auto& key : output_keys) {
        out_batch[i][key] = std::shared_ptr<void>();
      }
    }

    std::cout << "[OperatorRunner] Dispatching chunk [" << processed_count
              << ".." << (processed_count + chunk_size - 1) << " / "
              << total_inputs << "] (size=" << chunk_size
              << ", max_batch=" << max_batch_size << ") via ops.Process..."
              << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();
    int ret = ops.Process(raw_handle, in_batch, out_batch);
    auto end_time = std::chrono::high_resolution_clock::now();

    const double chunk_elapsed_ms =
        std::chrono::duration<double, std::milli>(end_time - start_time)
            .count();
    total_elapsed_ms += chunk_elapsed_ms;

    if (ret != 0) {
      std::string op_err = GetOperatorLastError();
      std::cerr << "[OperatorRunner ERROR] ops.Process failed at chunk "
                   "starting index "
                << processed_count << ": code=" << ret << " (" << op_err << ")"
                << std::endl;
      if (op_err.find("exceeds effective batch limit") != std::string::npos) {
        std::cerr << "[OperatorRunner HINT] 单次批次超过有效上限；用 "
                     "alg_pipeline_tool resolve-conf <conf> --root <root> "
                     "--depth <depth> 查看 effective_process_batch_limit"
                  << std::endl;
      }
      return 5;
    }

    // 4. 显示并收集每条结果。结果显示在输出块归还之前复制全部内容。
    const double per_sample_ms = chunk_elapsed_ms / chunk_size;
    for (size_t i = 0; i < chunk_size; ++i) {
      const size_t idx = processed_count + i;
      DemoSampleResult sample;
      sample.latency_ms = per_sample_ms;
      std::vector<nlohmann::json> fields;
      fields.reserve(output_keys.size());
      for (size_t k = 0; k < output_keys.size(); ++k) {
        const auto found = out_batch[i].find(output_keys[k]);
        if (found == out_batch[i].end() || !found->second) {
          std::cerr << "[OperatorRunner ERROR] Null output pointer received "
                       "for key '"
                    << output_keys[k] << "'." << std::endl;
          return 5;
        }
        uint64_t request_id = 0;
        int32_t status = 0;
        nlohmann::json output_fields = nlohmann::json::object();
        show_results[k](found->second.get(), batch.request_info[idx],
                        &request_id, &status, &output_fields);
        if (k == 0) sample.request_id = request_id;
        if (sample.status == 0) sample.status = status;
        fields.push_back(std::move(output_fields));
      }
      sample.output = MergeOutputFields(output_types, fields);
      sample_results.push_back(std::move(sample));
    }

    out_batch.clear();  // 释放输出块，触发 weak Deleter 回池
    processed_count += chunk_size;
  }

  std::cout << "[OperatorRunner] All " << total_inputs
            << " sample(s) dispatched in " << total_elapsed_ms << " ms."
            << std::endl;

  ResultWriter writer(options, run_label);
  std::string err;
  if (int w_ret = writer.WriteResults(sample_results, 0.0, &err); w_ret != 0) {
    std::cerr << "[OperatorRunner ERROR] Failed to write results: " << err
              << std::endl;
    return w_ret;
  }
  std::cout << "[OperatorRunner] Results written; see summary.json for sample "
               "success/failure counts."
            << std::endl;
  return 0;
}

}  // namespace alg_demo
