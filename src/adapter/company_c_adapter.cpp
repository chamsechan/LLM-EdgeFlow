#include <cstring>
#include <iostream>
#include <memory>

#include "company_alg_interface.h"
#include "core/alg_context.h"
#include "core/pipeline.h"

/**
 * @brief 句柄内部实例数据结构
 */
struct AlgHandleInstance {
  std::unique_ptr<alg_framework::Pipeline> pipeline;
  CompanyAlgBizType biz_type = ALG_BIZ_TYPE_UNKNOWN;
  int device_id = 0;
};

extern "C" {

int Alg_Init() {
  std::cout
      << "[Company C Adapter] Alg_Init: Global runtime resources initialized."
      << std::endl;
  return 0;
}

int Alg_Create(void** hndl, const CompanyAlgParamCreate* param_create) {
  if (!hndl || !param_create) {
    std::cerr
        << "[Company C Adapter] Alg_Create failed: Null pointer arguments."
        << std::endl;
    return -1;
  }

  try {
    auto instance = std::make_unique<AlgHandleInstance>();
    instance->biz_type = param_create->biz_type;
    instance->device_id = param_create->device_id;
    instance->pipeline = std::make_unique<alg_framework::Pipeline>();

    const char* cfg_path =
        param_create->config_file_path ? param_create->config_file_path : "";
    if (strlen(cfg_path) == 0) {
      std::cerr
          << "[Company C Adapter] Alg_Create failed: Empty config_file_path."
          << std::endl;
      return -2;
    }

    std::cout << "[Company C Adapter] Creating Pipeline for BizType ["
              << instance->biz_type << "] with config: " << cfg_path
              << std::endl;
    if (!instance->pipeline->BuildFromConfigFile(cfg_path)) {
      std::cerr << "[Company C Adapter] Failed to build pipeline from config."
                << std::endl;
      return -3;
    }

    *hndl = static_cast<void*>(instance.release());
    std::cout
        << "[Company C Adapter] Alg_Create: Handle created successfully at "
        << *hndl << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_Create exception: " << e.what()
              << std::endl;
    return -99;
  } catch (...) {
    return -100;
  }
}

int Alg_Process(void* hndl, const std::vector<void*>& inputs,
                std::vector<void*>& outputs) {
  if (!hndl) {
    std::cerr << "[Company C Adapter] Alg_Process failed: Null handle."
              << std::endl;
    return -1;
  }
  if (inputs.empty()) {
    std::cerr << "[Company C Adapter] Alg_Process failed: Empty inputs."
              << std::endl;
    return -2;
  }

  auto* instance = static_cast<AlgHandleInstance*>(hndl);

  try {
    alg_framework::AlgContext req_ctx;
    size_t batch_size = inputs.size();

    // =========================================================================
    // 业务 1: 关注词匹配业务 (ALG_BIZ_TYPE_KEYWORD_MATCH)
    // =========================================================================
    if (instance->biz_type == ALG_BIZ_TYPE_KEYWORD_MATCH) {
      std::vector<uint64_t> req_ids;
      std::vector<std::string> sentences;
      req_ids.reserve(batch_size);
      sentences.reserve(batch_size);

      for (size_t i = 0; i < batch_size; ++i) {
        auto* in = static_cast<CompanyKeywordInputStruct*>(inputs[i]);
        if (!in) return -3;
        req_ids.push_back(in->request_id);
        sentences.push_back(in->sentence_text ? in->sentence_text : "");
      }

      req_ctx.Set("raw_request_ids", std::move(req_ids));
      req_ctx.Set("input_sentences", std::move(sentences));

      int ret = instance->pipeline->Execute(&req_ctx);
      if (ret != 0) return ret;

      auto* res = req_ctx.Get<std::vector<CompanyKeywordOutputStruct>>(
          "keyword_match_outputs");
      if (!res || res->size() != batch_size) return -4;

      for (size_t i = 0; i < batch_size && i < outputs.size(); ++i) {
        auto* out_ptr = static_cast<CompanyKeywordOutputStruct*>(outputs[i]);
        if (out_ptr) *out_ptr = (*res)[i];
      }
      return 0;
    }

    // =========================================================================
    // 业务 2: 实体/名词提取业务 (ALG_BIZ_TYPE_ENTITY_EXTRACT)
    // =========================================================================
    if (instance->biz_type == ALG_BIZ_TYPE_ENTITY_EXTRACT) {
      std::vector<uint64_t> req_ids;
      std::vector<std::string> sentences;
      req_ids.reserve(batch_size);
      sentences.reserve(batch_size);

      for (size_t i = 0; i < batch_size; ++i) {
        auto* in = static_cast<CompanyEntityInputStruct*>(inputs[i]);
        if (!in) return -3;
        req_ids.push_back(in->request_id);
        sentences.push_back(in->sentence_text ? in->sentence_text : "");
      }

      req_ctx.Set("raw_request_ids", std::move(req_ids));
      req_ctx.Set("input_sentences", std::move(sentences));

      int ret = instance->pipeline->Execute(&req_ctx);
      if (ret != 0) return ret;

      auto* res = req_ctx.Get<std::vector<CompanyEntityOutputStruct>>(
          "entity_extract_outputs");
      if (!res || res->size() != batch_size) return -4;

      for (size_t i = 0; i < batch_size && i < outputs.size(); ++i) {
        auto* out_ptr = static_cast<CompanyEntityOutputStruct*>(outputs[i]);
        if (out_ptr) *out_ptr = (*res)[i];
      }
      return 0;
    }

    // =========================================================================
    // 业务 4: 智能对话风控质检业务 (ALG_BIZ_TYPE_COMPLIANCE_AUDIT: 多模型协同)
    // =========================================================================
    if (instance->biz_type == ALG_BIZ_TYPE_COMPLIANCE_AUDIT) {
      std::vector<uint64_t> req_ids;
      std::vector<std::string> user_texts;
      std::vector<std::string> channel_names;
      req_ids.reserve(batch_size);
      user_texts.reserve(batch_size);
      channel_names.reserve(batch_size);

      for (size_t i = 0; i < batch_size; ++i) {
        auto* in = static_cast<CompanyAuditInputStruct*>(inputs[i]);
        if (!in) return -3;
        req_ids.push_back(in->request_id);
        user_texts.push_back(in->user_text ? in->user_text : "");
        channel_names.push_back(in->channel_name ? in->channel_name : "");
      }

      req_ctx.Set("raw_request_ids", std::move(req_ids));
      req_ctx.Set("user_texts", std::move(user_texts));
      req_ctx.Set("channel_names", std::move(channel_names));

      int ret = instance->pipeline->Execute(&req_ctx);
      if (ret != 0) return ret;

      auto* res = req_ctx.Get<std::vector<CompanyAuditOutputStruct>>(
          "compliance_audit_outputs");
      if (!res || res->size() != batch_size) return -4;

      for (size_t i = 0; i < batch_size && i < outputs.size(); ++i) {
        auto* out_ptr = static_cast<CompanyAuditOutputStruct*>(outputs[i]);
        if (out_ptr) *out_ptr = (*res)[i];
      }
      return 0;
    }

    // =========================================================================
    // 业务 3: 智能长文档问答业务 (ALG_BIZ_TYPE_DOC_QA)
    // =========================================================================
    if (instance->biz_type == ALG_BIZ_TYPE_DOC_QA ||
        instance->biz_type == ALG_BIZ_TYPE_UNKNOWN) {
      std::vector<uint64_t> raw_req_ids;
      std::vector<std::string> raw_docs;
      std::vector<std::string> raw_queries;

      raw_req_ids.reserve(batch_size);
      raw_docs.reserve(batch_size);
      raw_queries.reserve(batch_size);

      for (size_t i = 0; i < batch_size; ++i) {
        auto* in_doc = static_cast<CompanyDocInputStruct*>(inputs[i]);
        if (!in_doc) return -3;
        raw_req_ids.push_back(in_doc->request_id);
        raw_docs.push_back(in_doc->doc_text ? in_doc->doc_text : "");
        raw_queries.push_back(in_doc->query_text ? in_doc->query_text : "");
      }

      req_ctx.Set("raw_request_ids", std::move(raw_req_ids));
      req_ctx.Set("raw_docs", std::move(raw_docs));
      req_ctx.Set("raw_queries", std::move(raw_queries));

      int ret = instance->pipeline->Execute(&req_ctx);
      if (ret != 0) return ret;

      auto* final_docs =
          req_ctx.Get<std::vector<CompanyDocOutputStruct>>("final_doc_outputs");
      if (!final_docs || final_docs->size() != batch_size) return -4;

      for (size_t i = 0; i < batch_size && i < outputs.size(); ++i) {
        auto* out_ptr = static_cast<CompanyDocOutputStruct*>(outputs[i]);
        if (out_ptr) *out_ptr = (*final_docs)[i];
      }
      return 0;
    }

    // =========================================================================
    // 业务 5: 智能多模态图文票据问答 (ALG_BIZ_TYPE_OCR_DOC_QA)
    // =========================================================================
    if (instance->biz_type == ALG_BIZ_TYPE_OCR_DOC_QA) {
      std::vector<uint64_t> raw_req_ids;
      std::vector<std::string> raw_images;
      std::vector<std::string> raw_queries;

      for (size_t i = 0; i < batch_size; ++i) {
        auto* in_ocr = static_cast<CompanyOcrDocInputStruct*>(inputs[i]);
        if (!in_ocr) return -3;
        raw_req_ids.push_back(in_ocr->request_id);
        raw_images.push_back(in_ocr->image_path ? in_ocr->image_path : "");
        raw_queries.push_back(in_ocr->query_prompt ? in_ocr->query_prompt : "");
      }

      req_ctx.Set("raw_request_ids", std::move(raw_req_ids));
      req_ctx.Set("raw_image_paths", std::move(raw_images));
      req_ctx.Set("raw_queries", std::move(raw_queries));

      int ret = instance->pipeline->Execute(&req_ctx);
      if (ret != 0) return ret;

      auto* res = req_ctx.Get<std::vector<CompanyOcrDocOutputStruct>>(
          "ocr_doc_final_outputs");
      if (!res || res->size() != batch_size) return -4;

      for (size_t i = 0; i < batch_size && i < outputs.size(); ++i) {
        auto* out_ptr = static_cast<CompanyOcrDocOutputStruct*>(outputs[i]);
        if (out_ptr) *out_ptr = (*res)[i];
      }
      return 0;
    }

    // =========================================================================
    // 业务 6: 语音识别与意图槽位抽取 (ALG_BIZ_TYPE_AUDIO_ASR_INTENT)
    // =========================================================================
    if (instance->biz_type == ALG_BIZ_TYPE_AUDIO_ASR_INTENT) {
      std::vector<uint64_t> raw_req_ids;
      std::vector<alg_framework::IAudioAsrEngine::AudioPcmData> raw_audios;

      for (size_t i = 0; i < batch_size; ++i) {
        auto* in_audio = static_cast<CompanyAudioInputStruct*>(inputs[i]);
        if (!in_audio) return -3;
        raw_req_ids.push_back(in_audio->request_id);

        alg_framework::IAudioAsrEngine::AudioPcmData pcm_data;
        if (in_audio->pcm_buffer && in_audio->pcm_length > 0) {
          pcm_data.pcm_data.assign(in_audio->pcm_buffer,
                                   in_audio->pcm_buffer + in_audio->pcm_length);
        }
        pcm_data.sample_rate =
            in_audio->sample_rate > 0 ? in_audio->sample_rate : 16000;
        raw_audios.push_back(std::move(pcm_data));
      }

      req_ctx.Set("raw_request_ids", std::move(raw_req_ids));
      req_ctx.Set("raw_audio_inputs", std::move(raw_audios));

      int ret = instance->pipeline->Execute(&req_ctx);
      if (ret != 0) return ret;

      auto* res = req_ctx.Get<std::vector<CompanyAudioOutputStruct>>(
          "audio_final_outputs");
      if (!res || res->size() != batch_size) return -4;

      for (size_t i = 0; i < batch_size && i < outputs.size(); ++i) {
        auto* out_ptr = static_cast<CompanyAudioOutputStruct*>(outputs[i]);
        if (out_ptr) *out_ptr = (*res)[i];
      }
      return 0;
    }

    // =========================================================================
    // 业务 7: 纯语义精排矩阵打分 (ALG_BIZ_TYPE_CROSS_RERANK)
    // =========================================================================
    if (instance->biz_type == ALG_BIZ_TYPE_CROSS_RERANK) {
      std::vector<CompanyRerankBatchInputStruct> raw_inputs;
      for (size_t i = 0; i < batch_size; ++i) {
        auto* in_rerank =
            static_cast<CompanyRerankBatchInputStruct*>(inputs[i]);
        if (!in_rerank) return -3;
        raw_inputs.push_back(*in_rerank);
      }

      req_ctx.Set("raw_rerank_inputs", std::move(raw_inputs));

      int ret = instance->pipeline->Execute(&req_ctx);
      if (ret != 0) return ret;

      auto* res = req_ctx.Get<std::vector<CompanyRerankBatchOutputStruct>>(
          "rerank_batch_final_outputs");
      if (!res || res->size() != batch_size) return -4;

      for (size_t i = 0; i < batch_size && i < outputs.size(); ++i) {
        auto* out_ptr =
            static_cast<CompanyRerankBatchOutputStruct*>(outputs[i]);
        if (out_ptr) *out_ptr = (*res)[i];
      }
      return 0;
    }

    std::cerr << "[Company C Adapter] Unsupported biz_type: "
              << instance->biz_type << std::endl;
    return -5;
  } catch (const std::exception& e) {
    std::cerr << "[Company C Adapter] Alg_Process exception: " << e.what()
              << std::endl;
    return -99;
  } catch (...) {
    return -100;
  }
}

int Alg_Control(void* hndl, const CompanyAlgParamControl* param_control) {
  if (!hndl || !param_control) return -1;
  if (!param_control->json_param_str) return -2;
  auto* instance = static_cast<AlgHandleInstance*>(hndl);
  return instance->pipeline->Control(param_control->control_cmd,
                                     param_control->json_param_str);
}

int Alg_Destroy(void* hndl) {
  if (!hndl) return -1;
  auto* instance = static_cast<AlgHandleInstance*>(hndl);
  std::cout << "[Company C Adapter] Alg_Destroy: Destroying handle at " << hndl
            << std::endl;
  delete instance;
  return 0;
}

int Alg_DeInit() {
  std::cout
      << "[Company C Adapter] Alg_DeInit: Global runtime resources released."
      << std::endl;
  return 0;
}

}  // extern "C"
