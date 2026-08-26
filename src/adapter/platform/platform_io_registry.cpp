#include "adapter/platform/platform_io_registry.h"

#include <iostream>
#include <sstream>
#include <unordered_set>

#include "adapter/business_adapter_registry.h"

namespace alg_framework {

PlatformIoRegistry& PlatformIoRegistry::Instance() {
  static PlatformIoRegistry instance;
  return instance;
}

PlatformIoRegistry::PlatformIoRegistry() { RegisterDefaults(); }

bool PlatformIoRegistry::ValidateDescriptor(const PlatformIoDescriptor& desc,
                                            std::string* error_msg) noexcept {
  try {
    const int biz_key = static_cast<int>(desc.biz_type);
    auto fail = [error_msg](const std::string& message) {
      if (error_msg) *error_msg = message;
      return false;
    };

    if (desc.biz_type == ALG_BIZ_TYPE_UNKNOWN || desc.biz_name.empty()) {
      return fail("Invalid descriptor: ALG_BIZ_TYPE_UNKNOWN or empty biz_name");
    }

    auto adapter =
        BusinessAdapterRegistry::Instance().GetAdapter(desc.biz_type);
    if (!adapter) {
      return fail("No BusinessAdapter registered for Platform I/O BizType " +
                  std::to_string(biz_key));
    }

    const auto& adapter_desc = adapter->GetDescriptor();
    if (desc.biz_name != adapter->BizName() ||
        desc.biz_name != adapter_desc.biz_name) {
      return fail("Platform I/O biz_name '" + desc.biz_name +
                  "' does not match BusinessAdapter '" + adapter->BizName() +
                  "' for BizType " + std::to_string(biz_key));
    }

    // 第一阶段每个样本严格映射为一个 C 输入 DTO 和一个 C 输出 DTO。
    if (desc.input_groups.size() != 1 || desc.output_groups.size() != 1) {
      return fail("Descriptor for BizType " + std::to_string(biz_key) +
                  " must declare exactly one input group and one output group");
    }

    if (desc.input_groups.front().c_type_name.empty() ||
        desc.input_groups.front().c_type_name != adapter_desc.input_type_name) {
      return fail("Input c_type_name for BizType " + std::to_string(biz_key) +
                  " must match BusinessAdapter type '" +
                  adapter_desc.input_type_name + "'");
    }
    if (desc.output_groups.front().c_type_name.empty() ||
        desc.output_groups.front().c_type_name !=
            adapter_desc.output_type_name) {
      return fail("Output c_type_name for BizType " + std::to_string(biz_key) +
                  " must match BusinessAdapter type '" +
                  adapter_desc.output_type_name + "'");
    }

    auto validate_groups = [&fail, biz_key](
                               const std::vector<PlatformIoSlotGroup>& groups,
                               IoDirection expected_direction,
                               const char* direction_name) {
      std::unordered_set<std::string> seen;
      for (const auto& group : groups) {
        if (group.canonical_suffix.empty() ||
            group.direction != expected_direction) {
          return fail("Invalid " + std::string(direction_name) +
                      " group canonical_suffix or direction for BizType " +
                      std::to_string(biz_key));
        }
        if (!seen.insert(group.canonical_suffix).second) {
          return fail("Duplicate suffix '" + group.canonical_suffix + "' in " +
                      direction_name + " groups for BizType " +
                      std::to_string(biz_key));
        }
        for (const auto& alias : group.aliases) {
          if (alias.empty() || !seen.insert(alias).second) {
            return fail("Duplicate or empty alias '" + alias + "' in " +
                        direction_name + " groups for BizType " +
                        std::to_string(biz_key));
          }
        }
      }
      return true;
    };

    return validate_groups(desc.input_groups, IoDirection::kInput, "input") &&
           validate_groups(desc.output_groups, IoDirection::kOutput, "output");
  } catch (const std::exception& e) {
    if (error_msg) {
      try {
        *error_msg =
            std::string("Descriptor validation exception: ") + e.what();
      } catch (...) {
      }
    }
    return false;
  } catch (...) {
    if (error_msg) {
      try {
        *error_msg = "Unknown descriptor validation exception";
      } catch (...) {
      }
    }
    return false;
  }
}

bool PlatformIoRegistry::RegisterDescriptor(const PlatformIoDescriptor& desc) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const int biz_key = static_cast<int>(desc.biz_type);

  std::string validation_error;
  if (!ValidateDescriptor(desc, &validation_error)) {
    has_conflict_ = true;
    registration_errors_.push_back(validation_error);
    std::cerr << "[PlatformIoRegistry ERROR] " << validation_error << std::endl;
    return false;
  }

  if (descriptors_.find(biz_key) != descriptors_.end()) {
    has_conflict_ = true;
    std::string err =
        "Duplicate PlatformIoDescriptor registration for BizType " +
        std::to_string(biz_key) + " (" + desc.biz_name + ")";
    registration_errors_.push_back(err);
    std::cerr << "[PlatformIoRegistry ERROR] " << err << std::endl;
    return false;
  }
  descriptors_[biz_key] = desc;
  return true;
}

bool PlatformIoRegistry::HasConflict() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return has_conflict_;
}

void PlatformIoRegistry::RegisterDefaults() {
  auto make_desc = [](CompanyAlgBizType biz_type, std::string in_suffix,
                      std::vector<std::string> in_aliases,
                      std::string out_suffix,
                      std::vector<std::string> out_aliases) {
    auto adapter = BusinessAdapterRegistry::Instance().GetAdapter(biz_type);
    std::string biz_name = adapter ? adapter->BizName() : "";
    std::string in_c_type =
        adapter ? adapter->GetDescriptor().input_type_name : "";
    std::string out_c_type =
        adapter ? adapter->GetDescriptor().output_type_name : "";
    return PlatformIoDescriptor{
        biz_type,
        std::move(biz_name),
        {{std::move(in_suffix), std::move(in_aliases), std::move(in_c_type),
          IoDirection::kInput, true}},
        {{std::move(out_suffix), std::move(out_aliases), std::move(out_c_type),
          IoDirection::kOutput, true}}};
  };

  // 业务 1: 关注词匹配
  RegisterDescriptor(make_desc(ALG_BIZ_TYPE_KEYWORD_MATCH, "keyword_in",
                               {"sentence_in"}, "keyword_out", {"match_out"}));

  // 业务 2: 实体/名词提取
  RegisterDescriptor(make_desc(ALG_BIZ_TYPE_ENTITY_EXTRACT, "entity_in",
                               {"text_in"}, "entity_out", {"extracted_out"}));

  // 业务 3: 智能长文档问答
  RegisterDescriptor(make_desc(ALG_BIZ_TYPE_DOC_QA, "doc_in", {"qa_in"},
                               "doc_out", {"qa_out"}));

  // 业务 4: 对话合规质检
  RegisterDescriptor(make_desc(ALG_BIZ_TYPE_COMPLIANCE_AUDIT, "audit_in",
                               {"dialogue_in"}, "audit_out", {"verdict_out"}));

  // 业务 5: OCR 图文票据
  RegisterDescriptor(make_desc(ALG_BIZ_TYPE_OCR_DOC_QA, "ocr_doc_in",
                               {"frame", "image_in"}, "ocr_doc_out",
                               {"od_out", "ocr_out"}));

  // 业务 6: 语音识别与意图
  RegisterDescriptor(make_desc(ALG_BIZ_TYPE_AUDIO_ASR_INTENT, "audio_in",
                               {"pcm_stream"}, "audio_out", {"asr_out"}));

  // 业务 7: 语义精排
  RegisterDescriptor(make_desc(ALG_BIZ_TYPE_CROSS_RERANK, "rerank_in",
                               {"pair_in"}, "rerank_out", {"scores_out"}));
}

const PlatformIoDescriptor* PlatformIoRegistry::GetDescriptor(
    CompanyAlgBizType biz_type) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto it = descriptors_.find(static_cast<int>(biz_type));
  if (it != descriptors_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool PlatformIoRegistry::ParseKey(const std::string& key,
                                  std::string* out_namespace,
                                  std::string* out_suffix) noexcept {
  if (key.empty()) return false;
  size_t pos = key.rfind('.');
  if (pos == std::string::npos || pos == 0 || pos == key.size() - 1) {
    return false;
  }
  if (out_namespace) *out_namespace = key.substr(0, pos);
  if (out_suffix) *out_suffix = key.substr(pos + 1);
  return true;
}

int PlatformIoRegistry::ExtractInputs(
    CompanyAlgBizType biz_type,
    const llm_edgeflow::platform::NamedIoBatch& inputs,
    std::vector<const void*>* out_ptrs, std::string* error_msg) const noexcept {
  if (!out_ptrs) return -3;
  out_ptrs->clear();

  const auto* desc = GetDescriptor(biz_type);
  if (!desc) {
    if (error_msg) {
      *error_msg = "No PlatformIoDescriptor registered for BizType " +
                   std::to_string(biz_type);
    }
    return -5;  // 业务未注册 / 不匹配
  }

  if (inputs.empty()) {
    if (error_msg) *error_msg = "Inputs batch is empty";
    return -3;
  }

  out_ptrs->reserve(inputs.size());

  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto& sample_map = inputs[i];
    if (sample_map.empty()) {
      if (error_msg) {
        *error_msg = "Input sample #" + std::to_string(i) + " is empty map";
      }
      return -3;
    }

    const void* matched_raw_ptr = nullptr;
    size_t matched_count = 0;

    // 遍历每一个输入槽位组进行匹配与必需性验证
    for (const auto& group : desc->input_groups) {
      const void* group_ptr = nullptr;
      for (const auto& [slot_key, opaque_data] : sample_map) {
        std::string ns, suffix;
        if (!ParseKey(slot_key, &ns, &suffix)) {
          if (error_msg) {
            *error_msg = "Invalid key format (missing dot): '" + slot_key +
                         "' at sample #" + std::to_string(i);
          }
          return -3;
        }

        if (group.Matches(suffix)) {
          if (!opaque_data || opaque_data.get() == nullptr) {
            if (error_msg) {
              *error_msg = "Null shared_ptr payload for key '" + slot_key +
                           "' at sample #" + std::to_string(i);
            }
            return -3;
          }
          if (group_ptr != nullptr) {
            if (error_msg) {
              *error_msg = "Duplicate slot in group '" +
                           group.canonical_suffix + "' ('" + slot_key +
                           "') at sample #" + std::to_string(i);
            }
            return -3;
          }
          group_ptr = opaque_data.get();
        }
      }

      if (group.is_required && group_ptr == nullptr) {
        if (error_msg) {
          *error_msg = "Missing required input slot group '" +
                       group.canonical_suffix + "' at sample #" +
                       std::to_string(i);
        }
        return -3;
      }

      if (group_ptr != nullptr) {
        matched_raw_ptr = group_ptr;
        matched_count++;
      }
    }

    // 检查是否有任何未识别的多余槽位
    for (const auto& [slot_key, opaque_data] : sample_map) {
      std::string ns, suffix;
      ParseKey(slot_key, &ns, &suffix);
      bool recognized = false;
      for (const auto& group : desc->input_groups) {
        if (group.Matches(suffix)) {
          recognized = true;
          break;
        }
      }
      if (!recognized) {
        if (error_msg) {
          *error_msg = "Unrecognized input slot key '" + slot_key +
                       "' (suffix: " + suffix + ") at sample #" +
                       std::to_string(i);
        }
        return -3;
      }
    }

    if (matched_raw_ptr == nullptr || matched_count == 0) {
      if (error_msg) {
        *error_msg =
            "No valid input slot matched for sample #" + std::to_string(i);
      }
      return -3;
    }

    out_ptrs->push_back(matched_raw_ptr);
  }

  return 0;
}

int PlatformIoRegistry::ExtractOutputs(
    CompanyAlgBizType biz_type,
    const llm_edgeflow::platform::NamedIoBatch& outputs,
    std::vector<void*>* out_ptrs, std::string* error_msg) const noexcept {
  if (!out_ptrs) return -4;
  out_ptrs->clear();

  const auto* desc = GetDescriptor(biz_type);
  if (!desc) {
    if (error_msg) {
      *error_msg = "No PlatformIoDescriptor registered for BizType " +
                   std::to_string(biz_type);
    }
    return -5;
  }

  if (outputs.empty()) {
    if (error_msg) *error_msg = "Outputs batch is empty";
    return -4;
  }

  out_ptrs->reserve(outputs.size());

  for (size_t i = 0; i < outputs.size(); ++i) {
    const auto& sample_map = outputs[i];
    if (sample_map.empty()) {
      if (error_msg) {
        *error_msg = "Output sample #" + std::to_string(i) + " is empty map";
      }
      return -4;
    }

    void* matched_raw_ptr = nullptr;
    size_t matched_count = 0;

    for (const auto& group : desc->output_groups) {
      void* group_ptr = nullptr;
      for (const auto& [slot_key, opaque_data] : sample_map) {
        std::string ns, suffix;
        if (!ParseKey(slot_key, &ns, &suffix)) {
          if (error_msg) {
            *error_msg = "Invalid key format (missing dot): '" + slot_key +
                         "' at sample #" + std::to_string(i);
          }
          return -4;
        }

        if (group.Matches(suffix)) {
          if (!opaque_data || opaque_data.get() == nullptr) {
            if (error_msg) {
              *error_msg = "Null output payload pointer for key '" + slot_key +
                           "' at sample #" + std::to_string(i);
            }
            return -4;
          }
          if (group_ptr != nullptr) {
            if (error_msg) {
              *error_msg = "Duplicate output slot in group '" +
                           group.canonical_suffix + "' ('" + slot_key +
                           "') at sample #" + std::to_string(i);
            }
            return -4;
          }
          group_ptr = opaque_data.get();
        }
      }

      if (group.is_required && group_ptr == nullptr) {
        if (error_msg) {
          *error_msg = "Missing required output slot group '" +
                       group.canonical_suffix + "' at sample #" +
                       std::to_string(i);
        }
        return -4;
      }

      if (group_ptr != nullptr) {
        matched_raw_ptr = group_ptr;
        matched_count++;
      }
    }

    // 检查是否有任何未识别的多余输出槽位
    for (const auto& [slot_key, opaque_data] : sample_map) {
      std::string ns, suffix;
      ParseKey(slot_key, &ns, &suffix);
      bool recognized = false;
      for (const auto& group : desc->output_groups) {
        if (group.Matches(suffix)) {
          recognized = true;
          break;
        }
      }
      if (!recognized) {
        if (error_msg) {
          *error_msg = "Unrecognized output slot key '" + slot_key +
                       "' (suffix: " + suffix + ") at sample #" +
                       std::to_string(i);
        }
        return -4;
      }
    }

    if (matched_raw_ptr == nullptr || matched_count == 0) {
      if (error_msg) {
        *error_msg =
            "No valid output slot matched for sample #" + std::to_string(i);
      }
      return -4;
    }

    out_ptrs->push_back(matched_raw_ptr);
  }

  return 0;
}

}  // namespace alg_framework
