#include "adapter/platform/platform_io_registry.h"

#include <iostream>

namespace alg_framework {

PlatformIoRegistry& PlatformIoRegistry::Instance() {
  static PlatformIoRegistry instance;
  return instance;
}

PlatformIoRegistry::PlatformIoRegistry() { RegisterDefaults(); }

void PlatformIoRegistry::RegisterDefaults() {
  // 业务 1: 关注词匹配
  descriptors_[ALG_BIZ_TYPE_KEYWORD_MATCH] = PlatformIoDescriptor{
      ALG_BIZ_TYPE_KEYWORD_MATCH,
      {{"keyword_in", "CompanyKeywordInputStruct", IoDirection::kInput, true},
       {"sentence_in", "CompanyKeywordInputStruct", IoDirection::kInput, true},
       {"keyword_out", "CompanyKeywordOutputStruct", IoDirection::kOutput,
        true},
       {"match_out", "CompanyKeywordOutputStruct", IoDirection::kOutput,
        true}}};

  // 业务 2: 实体/名词提取
  descriptors_[ALG_BIZ_TYPE_ENTITY_EXTRACT] = PlatformIoDescriptor{
      ALG_BIZ_TYPE_ENTITY_EXTRACT,
      {{"entity_in", "CompanyEntityInputStruct", IoDirection::kInput, true},
       {"text_in", "CompanyEntityInputStruct", IoDirection::kInput, true},
       {"entity_out", "CompanyEntityOutputStruct", IoDirection::kOutput, true},
       {"extracted_out", "CompanyEntityOutputStruct", IoDirection::kOutput,
        true}}};

  // 业务 3: 智能长文档问答
  descriptors_[ALG_BIZ_TYPE_DOC_QA] = PlatformIoDescriptor{
      ALG_BIZ_TYPE_DOC_QA,
      {{"doc_in", "CompanyDocInputStruct", IoDirection::kInput, true},
       {"qa_in", "CompanyDocInputStruct", IoDirection::kInput, true},
       {"doc_out", "CompanyDocOutputStruct", IoDirection::kOutput, true},
       {"qa_out", "CompanyDocOutputStruct", IoDirection::kOutput, true}}};

  // 业务 4: 对话合规质检
  descriptors_[ALG_BIZ_TYPE_COMPLIANCE_AUDIT] = PlatformIoDescriptor{
      ALG_BIZ_TYPE_COMPLIANCE_AUDIT,
      {{"audit_in", "CompanyAuditInputStruct", IoDirection::kInput, true},
       {"dialogue_in", "CompanyAuditInputStruct", IoDirection::kInput, true},
       {"audit_out", "CompanyAuditOutputStruct", IoDirection::kOutput, true},
       {"verdict_out", "CompanyAuditOutputStruct", IoDirection::kOutput,
        true}}};

  // 业务 5: OCR 图文票据
  descriptors_[ALG_BIZ_TYPE_OCR_DOC_QA] = PlatformIoDescriptor{
      ALG_BIZ_TYPE_OCR_DOC_QA,
      {{"frame", "CompanyOcrDocInputStruct", IoDirection::kInput, true},
       {"image_in", "CompanyOcrDocInputStruct", IoDirection::kInput, true},
       {"od_out", "CompanyOcrDocOutputStruct", IoDirection::kOutput, true},
       {"ocr_out", "CompanyOcrDocOutputStruct", IoDirection::kOutput, true}}};

  // 业务 6: 语音识别与意图
  descriptors_[ALG_BIZ_TYPE_AUDIO_ASR_INTENT] = PlatformIoDescriptor{
      ALG_BIZ_TYPE_AUDIO_ASR_INTENT,
      {{"audio_in", "CompanyAudioInputStruct", IoDirection::kInput, true},
       {"pcm_stream", "CompanyAudioInputStruct", IoDirection::kInput, true},
       {"audio_out", "CompanyAudioOutputStruct", IoDirection::kOutput, true},
       {"asr_out", "CompanyAudioOutputStruct", IoDirection::kOutput, true}}};

  // 业务 7: 语义精排
  descriptors_[ALG_BIZ_TYPE_CROSS_RERANK] = PlatformIoDescriptor{
      ALG_BIZ_TYPE_CROSS_RERANK,
      {{"rerank_in", "CompanyRerankBatchInputStruct", IoDirection::kInput,
        true},
       {"pair_in", "CompanyRerankBatchInputStruct", IoDirection::kInput, true},
       {"rerank_out", "CompanyRerankBatchOutputStruct", IoDirection::kOutput,
        true},
       {"scores_out", "CompanyRerankBatchOutputStruct", IoDirection::kOutput,
        true}}};
}

const PlatformIoDescriptor* PlatformIoRegistry::GetDescriptor(
    CompanyAlgBizType biz_type) const {
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
  if (!out_ptrs) {
    if (error_msg) *error_msg = "Null out_ptrs argument";
    return -3;
  }
  out_ptrs->clear();

  if (inputs.empty()) {
    if (error_msg) *error_msg = "Empty inputs batch";
    return -3;
  }

  const auto* desc = GetDescriptor(biz_type);
  if (!desc) {
    if (error_msg) {
      *error_msg = "No PlatformIoDescriptor for biz_type: " +
                   std::to_string(static_cast<int>(biz_type));
    }
    return -5;
  }

  out_ptrs->reserve(inputs.size());

  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto& item_map = inputs[i];
    if (item_map.empty()) {
      if (error_msg) {
        *error_msg =
            "Empty NamedIo map for input sample index " + std::to_string(i);
      }
      return -3;
    }

    const void* matched_ptr = nullptr;
    std::string matched_suffix;

    for (const auto& [k, v] : item_map) {
      std::string ns, suffix;
      if (!ParseKey(k, &ns, &suffix)) {
        if (error_msg) {
          *error_msg = "Invalid key format (missing dot or empty part): '" + k +
                       "' at sample " + std::to_string(i);
        }
        return -3;
      }

      // 查找该 suffix 是否属于合法的 Input Slot
      bool is_valid_input = false;
      for (const auto& slot : desc->slots) {
        if (slot.direction == IoDirection::kInput && slot.suffix == suffix) {
          is_valid_input = true;
          break;
        }
      }

      if (!is_valid_input) {
        if (error_msg) {
          *error_msg = "Unsupported input suffix '" + suffix + "' in key '" +
                       k + "' for biz_type " +
                       std::to_string(static_cast<int>(biz_type)) +
                       " at sample " + std::to_string(i);
        }
        return -3;
      }

      if (matched_ptr != nullptr) {
        if (error_msg) {
          *error_msg = "Duplicate input slot detected in sample " +
                       std::to_string(i) + " (first: '" + matched_suffix +
                       "', second: '" + suffix + "')";
        }
        return -3;
      }

      if (!v || v.get() == nullptr) {
        if (error_msg) {
          *error_msg = "Null shared_ptr payload for key '" + k +
                       "' at sample " + std::to_string(i);
        }
        return -3;
      }

      matched_ptr = v.get();
      matched_suffix = suffix;
    }

    if (!matched_ptr) {
      if (error_msg) {
        *error_msg =
            "No required input slot found for sample " + std::to_string(i);
      }
      return -3;
    }

    out_ptrs->push_back(matched_ptr);
  }

  return 0;
}

int PlatformIoRegistry::ExtractOutputs(
    CompanyAlgBizType biz_type,
    const llm_edgeflow::platform::NamedIoBatch& outputs,
    std::vector<void*>* out_ptrs, std::string* error_msg) const noexcept {
  if (!out_ptrs) {
    if (error_msg) *error_msg = "Null out_ptrs argument";
    return -4;
  }
  out_ptrs->clear();

  if (outputs.empty()) {
    if (error_msg) *error_msg = "Empty outputs batch";
    return -4;
  }

  const auto* desc = GetDescriptor(biz_type);
  if (!desc) {
    if (error_msg) {
      *error_msg = "No PlatformIoDescriptor for biz_type: " +
                   std::to_string(static_cast<int>(biz_type));
    }
    return -5;
  }

  out_ptrs->reserve(outputs.size());

  for (size_t i = 0; i < outputs.size(); ++i) {
    const auto& item_map = outputs[i];
    if (item_map.empty()) {
      if (error_msg) {
        *error_msg =
            "Empty NamedIo map for output sample index " + std::to_string(i);
      }
      return -4;
    }

    void* matched_ptr = nullptr;
    std::string matched_suffix;

    for (const auto& [k, v] : item_map) {
      std::string ns, suffix;
      if (!ParseKey(k, &ns, &suffix)) {
        if (error_msg) {
          *error_msg = "Invalid key format (missing dot or empty part): '" + k +
                       "' at output sample " + std::to_string(i);
        }
        return -4;
      }

      // 查找该 suffix 是否属于合法的 Output Slot
      bool is_valid_output = false;
      for (const auto& slot : desc->slots) {
        if (slot.direction == IoDirection::kOutput && slot.suffix == suffix) {
          is_valid_output = true;
          break;
        }
      }

      if (!is_valid_output) {
        if (error_msg) {
          *error_msg = "Unsupported output suffix '" + suffix + "' in key '" +
                       k + "' for biz_type " +
                       std::to_string(static_cast<int>(biz_type)) +
                       " at output sample " + std::to_string(i);
        }
        return -4;
      }

      if (matched_ptr != nullptr) {
        if (error_msg) {
          *error_msg = "Duplicate output slot detected in output sample " +
                       std::to_string(i) + " (first: '" + matched_suffix +
                       "', second: '" + suffix + "')";
        }
        return -4;
      }

      if (!v || v.get() == nullptr) {
        if (error_msg) {
          *error_msg = "Null shared_ptr output payload for key '" + k +
                       "' at output sample " + std::to_string(i);
        }
        return -4;
      }

      matched_ptr = v.get();
      matched_suffix = suffix;
    }

    if (!matched_ptr) {
      if (error_msg) {
        *error_msg = "No required output slot found for output sample " +
                     std::to_string(i);
      }
      return -4;
    }

    out_ptrs->push_back(matched_ptr);
  }

  return 0;
}

}  // namespace alg_framework
