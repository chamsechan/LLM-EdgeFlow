#include "adapter/platform/platform_io_registry.h"

#include <iostream>
#include <sstream>

namespace alg_framework {

PlatformIoRegistry& PlatformIoRegistry::Instance() {
  static PlatformIoRegistry instance;
  return instance;
}

PlatformIoRegistry::PlatformIoRegistry() { RegisterDefaults(); }

bool PlatformIoRegistry::RegisterDescriptor(const PlatformIoDescriptor& desc) {
  std::lock_guard<std::mutex> lock(mutex_);
  int biz_key = static_cast<int>(desc.biz_type);
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
  std::lock_guard<std::mutex> lock(mutex_);
  return has_conflict_;
}

void PlatformIoRegistry::RegisterDefaults() {
  // 业务 1: 关注词匹配
  RegisterDescriptor(PlatformIoDescriptor{ALG_BIZ_TYPE_KEYWORD_MATCH,
                                          "KeywordMatch",
                                          {{"keyword_in",
                                            {"sentence_in"},
                                            "CompanyKeywordInputStruct",
                                            IoDirection::kInput,
                                            true}},
                                          {{"keyword_out",
                                            {"match_out"},
                                            "CompanyKeywordOutputStruct",
                                            IoDirection::kOutput,
                                            true}}});

  // 业务 2: 实体/名词提取
  RegisterDescriptor(PlatformIoDescriptor{ALG_BIZ_TYPE_ENTITY_EXTRACT,
                                          "EntityExtract",
                                          {{"entity_in",
                                            {"text_in"},
                                            "CompanyEntityInputStruct",
                                            IoDirection::kInput,
                                            true}},
                                          {{"entity_out",
                                            {"extracted_out"},
                                            "CompanyEntityOutputStruct",
                                            IoDirection::kOutput,
                                            true}}});

  // 业务 3: 智能长文档问答
  RegisterDescriptor(PlatformIoDescriptor{ALG_BIZ_TYPE_DOC_QA,
                                          "DocQA",
                                          {{"doc_in",
                                            {"qa_in"},
                                            "CompanyDocInputStruct",
                                            IoDirection::kInput,
                                            true}},
                                          {{"doc_out",
                                            {"qa_out"},
                                            "CompanyDocOutputStruct",
                                            IoDirection::kOutput,
                                            true}}});

  // 业务 4: 对话合规质检
  RegisterDescriptor(PlatformIoDescriptor{ALG_BIZ_TYPE_COMPLIANCE_AUDIT,
                                          "ComplianceAudit",
                                          {{"audit_in",
                                            {"dialogue_in"},
                                            "CompanyAuditInputStruct",
                                            IoDirection::kInput,
                                            true}},
                                          {{"audit_out",
                                            {"verdict_out"},
                                            "CompanyAuditOutputStruct",
                                            IoDirection::kOutput,
                                            true}}});

  // 业务 5: OCR 图文票据
  RegisterDescriptor(PlatformIoDescriptor{ALG_BIZ_TYPE_OCR_DOC_QA,
                                          "OcrDocQA",
                                          {{"frame",
                                            {"image_in"},
                                            "CompanyOcrDocInputStruct",
                                            IoDirection::kInput,
                                            true}},
                                          {{"od_out",
                                            {"ocr_out"},
                                            "CompanyOcrDocOutputStruct",
                                            IoDirection::kOutput,
                                            true}}});

  // 业务 6: 语音识别与意图
  RegisterDescriptor(PlatformIoDescriptor{ALG_BIZ_TYPE_AUDIO_ASR_INTENT,
                                          "AudioAsrIntent",
                                          {{"audio_in",
                                            {"pcm_stream"},
                                            "CompanyAudioInputStruct",
                                            IoDirection::kInput,
                                            true}},
                                          {{"audio_out",
                                            {"asr_out"},
                                            "CompanyAudioOutputStruct",
                                            IoDirection::kOutput,
                                            true}}});

  // 业务 7: 语义精排
  RegisterDescriptor(PlatformIoDescriptor{ALG_BIZ_TYPE_CROSS_RERANK,
                                          "CrossRerank",
                                          {{"rerank_in",
                                            {"pair_in"},
                                            "CompanyRerankBatchInputStruct",
                                            IoDirection::kInput,
                                            true}},
                                          {{"rerank_out",
                                            {"scores_out"},
                                            "CompanyRerankBatchOutputStruct",
                                            IoDirection::kOutput,
                                            true}}});
}

const PlatformIoDescriptor* PlatformIoRegistry::GetDescriptor(
    CompanyAlgBizType biz_type) const {
  std::lock_guard<std::mutex> lock(mutex_);
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
