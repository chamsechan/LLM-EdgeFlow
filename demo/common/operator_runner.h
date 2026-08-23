#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "adapter/business_adapter_registry.h"
#include "company_alg_interface.h"
#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "nlohmann/json.hpp"
#include "platform/platform_operator_interface.h"

namespace alg_demo {

/**
 * @brief 将 Demo 业务名映射为标准 CompanyAlgBizType 枚举
 */
inline CompanyAlgBizType DemoBusinessToBizType(std::string_view demo_biz) {
  if (demo_biz == "entity_extract") return ALG_BIZ_TYPE_ENTITY_EXTRACT;
  if (demo_biz == "keyword_match") return ALG_BIZ_TYPE_KEYWORD_MATCH;
  if (demo_biz == "doc_qa") return ALG_BIZ_TYPE_DOC_QA;
  if (demo_biz == "dialogue_audit") return ALG_BIZ_TYPE_COMPLIANCE_AUDIT;
  if (demo_biz == "ocr_doc_qa") return ALG_BIZ_TYPE_OCR_DOC_QA;
  if (demo_biz == "audio_asr") return ALG_BIZ_TYPE_AUDIO_ASR_INTENT;
  if (demo_biz == "cross_rerank") return ALG_BIZ_TYPE_CROSS_RERANK;
  return ALG_BIZ_TYPE_UNKNOWN;
}

/**
 * @brief 校验 Pipeline business_name 与 Demo 业务是否兼容 (基于
 * BusinessAdapterRegistry 单一事实源)
 */
inline bool IsBusinessCompatible(std::string_view expected_demo_biz,
                                 const std::string& actual_pipe_biz) {
  CompanyAlgBizType expected_type = DemoBusinessToBizType(expected_demo_biz);
  if (expected_type == ALG_BIZ_TYPE_UNKNOWN) {
    return false;
  }
  auto adapter = alg_framework::BusinessAdapterRegistry::Instance()
                     .GetAdapterByPipelineName(actual_pipe_biz);
  if (!adapter) {
    return false;
  }
  return adapter->BizType() == expected_type;
}

/**
 * @brief 校验 .conf 与 Pipeline JSON 中的 business_name 是否与 Demo Case 兼容
 */
inline bool ValidateConfigBusinessMatch(const std::string& conf_path,
                                        std::string_view expected_biz,
                                        std::string* error_msg) {
  std::string resolved_conf = ResolvePath(conf_path);
  std::ifstream c_ifs(resolved_conf);
  if (!c_ifs.is_open()) {
    if (error_msg) {
      *error_msg = "Cannot open deployment .conf: '" + conf_path +
                   "' (resolved: '" + resolved_conf + "')";
    }
    return false;
  }

  nlohmann::json conf_json;
  try {
    c_ifs >> conf_json;
  } catch (const std::exception& e) {
    if (error_msg) {
      *error_msg =
          "Invalid JSON in .conf file '" + resolved_conf + "': " + e.what();
    }
    return false;
  }

  std::string pipe_rel;
  if (conf_json.contains("data") && conf_json["data"].is_object() &&
      conf_json["data"].contains("pipe_path")) {
    pipe_rel = conf_json["data"]["pipe_path"].get<std::string>();
  } else if (conf_json.contains("pipe_path")) {
    pipe_rel = conf_json["pipe_path"].get<std::string>();
  }

  if (pipe_rel.empty()) {
    if (error_msg) {
      *error_msg = "Missing 'pipe_path' in .conf file '" + resolved_conf + "'";
    }
    return false;
  }

  // 拼接或解析 pipeline JSON 路径
  std::filesystem::path conf_dir =
      std::filesystem::path(resolved_conf).parent_path();
  std::filesystem::path candidate = conf_dir / pipe_rel;
  std::string resolved_pipe = ResolvePath(candidate.string());
  if (!std::filesystem::exists(resolved_pipe)) {
    resolved_pipe = ResolvePath(pipe_rel);
  }

  std::ifstream p_ifs(resolved_pipe);
  if (!p_ifs.is_open()) {
    if (error_msg) {
      *error_msg = "Cannot open pipeline JSON: '" + pipe_rel +
                   "' (resolved: '" + resolved_pipe + "')";
    }
    return false;
  }

  nlohmann::json pipe_json;
  try {
    p_ifs >> pipe_json;
  } catch (const std::exception& e) {
    if (error_msg) {
      *error_msg =
          "Invalid JSON in pipeline file '" + resolved_pipe + "': " + e.what();
    }
    return false;
  }

  if (!pipe_json.contains("business_name") ||
      !pipe_json["business_name"].is_string()) {
    if (error_msg) {
      *error_msg =
          "Pipeline JSON missing 'business_name' in '" + resolved_pipe + "'";
    }
    return false;
  }

  std::string actual_biz = pipe_json["business_name"].get<std::string>();
  if (!IsBusinessCompatible(expected_biz, actual_biz)) {
    if (error_msg) {
      *error_msg = "Business mismatch: Pipeline declares business_name '" +
                   actual_biz +
                   "', which is incompatible with demo business '" +
                   std::string(expected_biz) + "'";
    }
    return false;
  }

  return true;
}

/**
 * @brief Platform Operator 句柄 RAII 生命周期管理器
 */
struct OperatorHandleGuard {
  llm_edgeflow::platform::OperatorFunc ops;
  void* handle = nullptr;

  OperatorHandleGuard(llm_edgeflow::platform::OperatorFunc f, void* h)
      : ops(f), handle(h) {}

  ~OperatorHandleGuard() {
    if (handle) {
      ops.Destroy(handle);
      handle = nullptr;
    }
  }

  // 禁止拷贝
  OperatorHandleGuard(const OperatorHandleGuard&) = delete;
  OperatorHandleGuard& operator=(const OperatorHandleGuard&) = delete;
};

/**
 * @brief 通用 Platform Operator 生命周期与按批分块执行器 (P1-1, P1-2, P1-3)
 * @tparam TInput 业务 C 输入结构体类型
 * @tparam TOutput 业务 C 输出结构体类型
 * @param options Demo 运行选项
 * @param input_slot 输入槽位名称 (如 "nlp_node.entity_in")
 * @param output_slot 输出槽位名称 (如 "nlp_node.entity_out")
 * @param inputs 输入结构体列表
 * @param outputs 输出结构体列表
 * @param out_latencies_ms 每个样本的耗时列表
 * @param ctrl_cmd 控制命令类型
 * @param default_ctrl_json 默认控制 JSON (若 options 未指定 control_file)
 * @return 0 成功, 非 0 错误退出码 (3: 配置/Control错误, 4: 数据集错误, 5:
 * 平台执行错误)
 */
template <typename TInput, typename TOutput>
int RunPlatformOperator(
    const DemoOptions& options, std::string_view input_slot,
    std::string_view output_slot, std::vector<TInput>& inputs,
    std::vector<TOutput>* outputs,
    std::vector<double>* out_latencies_ms = nullptr,
    llm_edgeflow::platform::ControlCommand ctrl_cmd =
        llm_edgeflow::platform::ControlCommand::kUpdateRules,
    const char* default_ctrl_json = nullptr) {
  using namespace llm_edgeflow::platform;

  if (inputs.empty()) {
    std::cerr << "[OperatorRunner ERROR] Inputs vector is empty." << std::endl;
    return 4;
  }
  if (!outputs) {
    std::cerr << "[OperatorRunner ERROR] Null outputs pointer provided."
              << std::endl;
    return 5;
  }

  // 1. 验证配置文件与业务精确匹配 (P1-3)
  std::string err;
  if (!ValidateConfigBusinessMatch(options.config_path, options.business,
                                   &err)) {
    std::cerr << "[OperatorRunner ERROR] Config validation failed: " << err
              << std::endl;
    return 3;
  }

  ChipType chip_type = ChipType::kUnknown;
  if (!ParseChipType(options.chip, &chip_type)) {
    std::cerr << "[OperatorRunner ERROR] Unsupported chip: " << options.chip
              << std::endl;
    return 3;
  }

  std::string resolved_conf = ResolvePath(options.config_path);

  // 2. 获取平台函数表
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();

  // 3. 创建平台会话句柄 (严格使用 options.batch_size 作为单次 Process 的最大
  // Batch，P1-1)
  int max_batch_size = options.batch_size > 0 ? options.batch_size : 1;

  CreateParam param{};
  param.cfg_file_name = resolved_conf.c_str();
  param.platform_config.batch_size = max_batch_size;
  param.platform_config.device_id = options.device_id;
  param.platform_config.type = chip_type;
  param.depth_num = options.depth_num > 0 ? options.depth_num : 1;

  void* raw_handle = nullptr;
  int ret = ops.Create(&raw_handle, &param);
  if (ret != 0 || !raw_handle) {
    std::cerr << "[OperatorRunner ERROR] Failed ops.Create with conf: "
              << resolved_conf << " (Platform error: " << GetPlatformLastError()
              << ")" << std::endl;
    return 5;
  }

  // RAII 守卫确保离开函数时 Destroy 必然调用
  OperatorHandleGuard guard(ops, raw_handle);

  // 4. 下发 Control 命令 (Fail-Closed:
  // 显式指定读取或下发失败立即报错退出，P1-2)
  std::string control_payload;
  if (options.control_file.has_value() && !options.control_file->empty()) {
    if (!ReadTextFile(*options.control_file, &control_payload, &err)) {
      std::cerr
          << "[OperatorRunner ERROR] Failed to read explicit control file '"
          << *options.control_file << "': " << err << std::endl;
      return 3;
    }
    // 校验 JSON 语法有效性
    try {
      auto parsed_check = nlohmann::json::parse(control_payload);
      if (!parsed_check.is_object()) {
        std::cerr << "[OperatorRunner ERROR] Control file content must be a "
                     "JSON object"
                  << std::endl;
        return 3;
      }
    } catch (const std::exception& e) {
      std::cerr
          << "[OperatorRunner ERROR] Invalid JSON syntax in control file: "
          << e.what() << std::endl;
      return 3;
    }
  } else if (default_ctrl_json != nullptr) {
    control_payload = default_ctrl_json;
  }

  if (!control_payload.empty()) {
    std::cout << "[OperatorRunner] Invoking ops.Control to dynamically push "
                 "parameters..."
              << std::endl;
    ControlUpdateRulesParam ctrl_param{control_payload.c_str()};
    int ctrl_ret = ops.Control(raw_handle, ctrl_cmd, &ctrl_param);
    if (ctrl_ret != 0) {
      std::cerr << "[OperatorRunner ERROR] ops.Control failed: code="
                << ctrl_ret << " (Platform error: " << GetPlatformLastError()
                << ")" << std::endl;
      return 5;
    }
  }

  // 5. 按 max_batch_size 分块调度执行 Process (P1-1)
  size_t total_inputs = inputs.size();
  outputs->assign(total_inputs, TOutput{});
  if (out_latencies_ms) {
    out_latencies_ms->assign(total_inputs, 0.0);
  }

  std::string in_key(input_slot);
  std::string out_key(output_slot);

  size_t processed_count = 0;
  double total_elapsed_ms = 0.0;

  while (processed_count < total_inputs) {
    size_t chunk_size = std::min(static_cast<size_t>(max_batch_size),
                                 total_inputs - processed_count);

    NamedIoBatch in_batch(chunk_size);
    NamedIoBatch out_batch(chunk_size);

    for (size_t i = 0; i < chunk_size; ++i) {
      size_t idx = processed_count + i;
      in_batch[i][in_key] = std::shared_ptr<void>(&inputs[idx], [](void*) {});
      out_batch[i][out_key] =
          std::shared_ptr<void>(&(*outputs)[idx], [](void*) {});
    }

    std::cout << "[OperatorRunner] Dispatching chunk [" << processed_count
              << ".." << (processed_count + chunk_size - 1) << " / "
              << total_inputs << "] (size=" << chunk_size
              << ", max_batch=" << max_batch_size << ") via ops.Process ("
              << in_key << " -> " << out_key << ")..." << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();
    ret = ops.Process(raw_handle, in_batch, out_batch);
    auto end_time = std::chrono::high_resolution_clock::now();

    double chunk_elapsed_ms =
        std::chrono::duration<double, std::milli>(end_time - start_time)
            .count();
    total_elapsed_ms += chunk_elapsed_ms;

    if (ret != 0) {
      std::cerr << "[OperatorRunner ERROR] ops.Process failed at chunk "
                   "starting index "
                << processed_count << ": code=" << ret << " ("
                << GetPlatformLastError() << ")" << std::endl;
      return 5;
    }

    if (out_latencies_ms) {
      double per_sample_ms =
          chunk_size > 0 ? (chunk_elapsed_ms / chunk_size) : 0.0;
      for (size_t i = 0; i < chunk_size; ++i) {
        (*out_latencies_ms)[processed_count + i] = per_sample_ms;
      }
    }

    processed_count += chunk_size;
  }

  std::cout << "[OperatorRunner] All " << total_inputs
            << " sample(s) processed successfully in " << total_elapsed_ms
            << " ms." << std::endl;
  return 0;
}

}  // namespace alg_demo
