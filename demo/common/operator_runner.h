#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "company_alg_interface.h"
#include "demo/common/dataset_reader.h"
#include "demo/common/demo_options.h"
#include "demo/common/result_writer.h"
#include "nlohmann/json.hpp"
#include "operator/company_operator_types.h"
#include "operator/operator_interface.h"

namespace alg_demo {

/**
 * @brief 将 Demo 业务名映射为标准 CompanyAlgBizType 枚举
 */
inline CompanyAlgBizType DemoBizToBizType(std::string_view demo_biz) {
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
 * @brief 校验 .conf 与 Pipeline JSON 中的 biz_name 是否与 Demo Case 兼容
 */
inline bool ValidateConfigBizMatch(const std::string& conf_path,
                                   std::string_view expected_biz,
                                   std::string* error_msg) {
  CompanyAlgBizType expected_type = DemoBizToBizType(expected_biz);
  if (expected_type == ALG_BIZ_TYPE_UNKNOWN) {
    if (error_msg) {
      *error_msg = "Unknown demo biz: " + std::string(expected_biz);
    }
    return false;
  }

  std::string model_root;
  std::string cfg_rel;
  ResolveModelRootAndConfig(conf_path, &model_root, &cfg_rel);

  char err_buf[512] = {0};
  int ret = llm_edgeflow::operator_api::ValidateOperatorConfigBinding(
      model_root.c_str(), cfg_rel.c_str(), static_cast<int32_t>(expected_type),
      err_buf, sizeof(err_buf));

  if (ret != 0) {
    if (error_msg) {
      *error_msg = (err_buf[0] != '\0')
                       ? std::string(err_buf)
                       : "Operator config validation failed (code " +
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
 * @brief 通用 Operator 单槽位生命周期与调度执行器
 */
template <typename TInput, typename TOutput, typename TResultExtractor>
int RunOperatorWithExtractor(
    const DemoOptions& options, std::string_view input_slot,
    std::string_view output_slot, const std::vector<TInput>& inputs,
    TResultExtractor&& extractor,
    std::vector<double>* out_latencies_ms = nullptr,
    llm_edgeflow::operator_api::ControlCommand ctrl_cmd =
        llm_edgeflow::operator_api::ControlCommand::kUpdateRules,
    const char* default_ctrl_json = nullptr) {
  using namespace llm_edgeflow::operator_api;

  if (inputs.empty()) {
    std::cerr << "[OperatorRunner ERROR] Inputs vector is empty." << std::endl;
    return 4;
  }

  std::string err;
  if (!ValidateConfigBizMatch(options.config_path, options.biz, &err)) {
    std::cerr << "[OperatorRunner ERROR] Config validation failed: " << err
              << std::endl;
    return 3;
  }

  ComputePlatform chip_type = ComputePlatform::kUnknown;
  if (!ParseComputePlatform(options.chip, &chip_type)) {
    std::cerr << "[OperatorRunner ERROR] Unsupported compute platform / chip: "
              << options.chip << std::endl;
    return 3;
  }

  std::string model_root;
  std::string cfg_rel;
  ResolveModelRootAndConfig(options.config_path, &model_root, &cfg_rel);

  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();

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

  void* raw_handle = nullptr;
  int ret = ops.Create(&raw_handle, &param);
  if (ret != 0 || !raw_handle) {
    std::string op_err = GetOperatorLastError();
    std::cerr << "[OperatorRunner ERROR] Failed ops.Create with conf: "
              << options.config_path << " (Operator error: " << op_err << ")"
              << std::endl;
    return 5;
  }

  OperatorHandleGuard guard(ops, raw_handle);

  std::string control_payload;
  if (options.control_file.has_value() && !options.control_file->empty()) {
    if (!ReadTextFile(*options.control_file, &control_payload, &err)) {
      std::cerr
          << "[OperatorRunner ERROR] Failed to read explicit control file '"
          << *options.control_file << "': " << err << std::endl;
      return 3;
    }
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
                << ctrl_ret << " (Operator error: " << GetOperatorLastError()
                << ")" << std::endl;
      return 5;
    }
  }

  size_t total_inputs = inputs.size();
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
      in_batch[i][in_key] = MakeBorrowedOperatorInput(&inputs[idx]);
      out_batch[i][out_key] = std::shared_ptr<void>();
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
      std::string op_err = GetOperatorLastError();
      std::cerr << "[OperatorRunner ERROR] ops.Process failed at chunk "
                   "starting index "
                << processed_count << ": code=" << ret << " (" << op_err << ")"
                << std::endl;
      return 5;
    }

    for (size_t i = 0; i < chunk_size; ++i) {
      size_t idx = processed_count + i;
      if (out_batch[i][out_key] && out_batch[i][out_key].get()) {
        const auto* out_ptr =
            static_cast<const TOutput*>(out_batch[i][out_key].get());
        extractor(idx, *out_ptr);
      }
    }

    if (out_latencies_ms) {
      double per_sample_ms =
          chunk_size > 0 ? (chunk_elapsed_ms / chunk_size) : 0.0;
      for (size_t i = 0; i < chunk_size; ++i) {
        (*out_latencies_ms)[processed_count + i] = per_sample_ms;
      }
    }

    out_batch.clear();
    processed_count += chunk_size;
  }

  std::cout << "[OperatorRunner] All " << total_inputs
            << " sample(s) processed successfully in " << total_elapsed_ms
            << " ms." << std::endl;
  return 0;
}

}  // namespace alg_demo
