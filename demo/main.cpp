#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "company_alg_interface.h"

// 辅助函数：根据运行路径解析正确的配置文件路径
std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

// =============================================================================
// 演示业务 1: 关注词匹配业务 (无需模型 / Control动态词表 / Batch=2)
// =============================================================================
void RunDemoBusiness1_KeywordMatch() {
  std::cout
      << "\n=================================================================="
      << std::endl;
  std::cout << "  【业务 1 演示】关注词匹配业务 (无需模型 / Control动态词表 / "
               "Batch=2)"
            << std::endl;
  std::cout
      << "=================================================================="
      << std::endl;

  CompanyAlgParamCreate create_param;
  std::string cfg = GetConfigPath("configs/pipeline_keyword_match.json");
  create_param.config_file_path = cfg.c_str();
  create_param.model_root_dir = "./models";
  create_param.device_id = 0;
  create_param.biz_type = ALG_BIZ_TYPE_KEYWORD_MATCH;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &create_param);
  if (ret != 0 || !handle) {
    std::cerr << "Alg_Create failed for Business 1: " << ret << std::endl;
    return;
  }

  // 1. 通过 Control 动态下发一个包含分类的关注词表 JSON
  CompanyAlgParamControl ctrl;
  ctrl.control_cmd = 1;
  ctrl.json_param_str =
      "{\n"
      "  \"categories\": {\n"
      "    \"RISK_COMPLAINT\": [\"投诉\", \"欺诈\", \"假货\", \"黑心商家\"],\n"
      "    \"VIP_SERVICE\": [\"VIP\", \"大客户\", \"加急\", \"专员\"],\n"
      "    \"URGENT_HELP\": [\"报警\", \"救命\", \"紧急\"]\n"
      "  }\n"
      "}";
  std::cout
      << "[Client] Invoking Alg_Control to dynamically push word categories..."
      << std::endl;
  Alg_Control(handle, &ctrl);

  // 2. 构造 input vector，长度为 2，每个元素为一句话
  CompanyKeywordInputStruct req0;
  req0.request_id = 20001;
  req0.sentence_text =
      "请帮我联系一下VIP专员，我有一笔大客户加急订单需要优先处理。";

  CompanyKeywordInputStruct req1;
  req1.request_id = 20002;
  req1.sentence_text = "今天天气真不错，阳光明媚，我想去公园散散步。";

  std::vector<void*> inputs = {&req0, &req1};

  CompanyKeywordOutputStruct out0;
  CompanyKeywordOutputStruct out1;
  std::vector<void*> outputs = {&out0, &out1};

  std::cout << "[Client] Sending 2 sentences to Alg_Process()..." << std::endl;
  ret = Alg_Process(handle, inputs, outputs);
  if (ret == 0) {
    std::cout << "\n>>> 业务 1 执行结果验证 <<<" << std::endl;
    for (size_t i = 0; i < outputs.size(); ++i) {
      auto* out = static_cast<CompanyKeywordOutputStruct*>(outputs[i]);
      std::cout << "-----------------------------------------------------------"
                   "-------"
                << std::endl;
      std::cout << "  Input #" << i << ": \""
                << (i == 0 ? req0.sentence_text : req1.sentence_text) << "\""
                << std::endl;
      std::cout << "  Request ID : " << out->request_id << std::endl;
      std::cout << "  Is Hit     : "
                << (out->is_hit ? "YES (命中)" : "NO (未命中)") << std::endl;
      std::cout << "  JSON Output: " << out->match_result_json << std::endl;
    }
  }

  Alg_Destroy(handle);
  std::cout << "[Client] Business 1 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 演示业务 2: 实体/名词提取业务 (使用 0.6B LLM 模型)
// =============================================================================
void RunDemoBusiness2_EntityExtraction() {
  std::cout
      << "\n=================================================================="
      << std::endl;
  std::cout << "  【业务 2 演示】实体/名词提取业务 (使用 0.6B LLM 模型)"
            << std::endl;
  std::cout
      << "=================================================================="
      << std::endl;

  CompanyAlgParamCreate create_param;
  std::string cfg = GetConfigPath("configs/pipeline_entity_extract.json");
  create_param.config_file_path = cfg.c_str();
  create_param.model_root_dir = "./models";
  create_param.device_id = 0;
  create_param.biz_type = ALG_BIZ_TYPE_ENTITY_EXTRACT;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &create_param);
  if (ret != 0 || !handle) {
    std::cerr << "Alg_Create failed for Business 2: " << ret << std::endl;
    return;
  }

  CompanyEntityInputStruct req;
  req.request_id = 30001;
  req.sentence_text =
      "张三在清华大学毕业后加入了一家北京的人工智能公司，作为算法工程师负责NPU"
      "芯片和深度学习大模型的研发项目。";

  std::vector<void*> inputs = {&req};

  CompanyEntityOutputStruct out;
  std::vector<void*> outputs = {&out};

  std::cout << "[Client] Sending 1 sentence for entity/noun extraction to "
               "Alg_Process()..."
            << std::endl;
  ret = Alg_Process(handle, inputs, outputs);
  if (ret == 0) {
    std::cout << "\n>>> 业务 2 执行结果验证 <<<" << std::endl;
    std::cout
        << "------------------------------------------------------------------"
        << std::endl;
    std::cout << "  Input Sentence : \"" << req.sentence_text << "\""
              << std::endl;
    std::cout << "  Request ID     : " << out.request_id << std::endl;
    std::cout << "  Extracted JSON : " << out.entities_json << std::endl;
    std::cout
        << "------------------------------------------------------------------"
        << std::endl;
  }

  Alg_Destroy(handle);
  std::cout << "[Client] Business 2 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 演示业务 3: 智能长文档切片检索与问答业务 (多模型协同: Embedding + LLM)
// =============================================================================
void RunDemoBusiness3_DocQa() {
  std::cout
      << "\n=================================================================="
      << std::endl;
  std::cout
      << "  【业务 3 演示】智能长文档问答业务 (多模型协同: Embedding + LLM)"
      << std::endl;
  std::cout
      << "=================================================================="
      << std::endl;

  CompanyAlgParamCreate create_param;
  std::string cfg = GetConfigPath("configs/pipeline_doc_qa.json");
  create_param.config_file_path = cfg.c_str();
  create_param.model_root_dir = "./models";
  create_param.device_id = 0;
  create_param.biz_type = ALG_BIZ_TYPE_DOC_QA;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &create_param);
  if (ret != 0 || !handle) {
    std::cerr << "Alg_Create failed for Business 3: " << ret << std::endl;
    return;
  }

  CompanyDocInputStruct req0;
  req0.request_id = 10001;
  req0.doc_text =
      "企业级算法框架设计规范：采用4层分层架构，包含C-"
      "ABI适配层、Pipeline调度层、通用算子池与底层硬件引擎抽象。支持在NPU和GPU"
      "上运行，并通过静态批处理机制对齐多变样本。";
  req0.query_text = "请简述该算法框架的架构设计与核心技术？";

  CompanyDocInputStruct req1;
  req1.request_id = 10002;
  req1.doc_text =
      "客户服务售后政策：支持7天无理由退货与全额退款。若商品存在质量问题，由平"
      "台承担双向运费并提供快速换货。退款将在审核通过后原路返回。";
  req1.query_text = "商品有瑕疵，我想办理退款退货，售后流程是什么？";

  std::vector<void*> inputs = {&req0, &req1};

  CompanyDocOutputStruct out0;
  CompanyDocOutputStruct out1;
  std::vector<void*> outputs = {&out0, &out1};

  std::cout << "[Client] Sending batch of 2 requests to Alg_Process()..."
            << std::endl;
  ret = Alg_Process(handle, inputs, outputs);
  if (ret == 0) {
    std::cout << "\n>>> 业务 3 执行结果验证 <<<" << std::endl;
    for (size_t i = 0; i < outputs.size(); ++i) {
      auto* out = static_cast<CompanyDocOutputStruct*>(outputs[i]);
      std::cout << "-----------------------------------------------------------"
                   "-------"
                << std::endl;
      std::cout << "  Result #" << i << " | Request ID: " << out->request_id
                << std::endl;
      std::cout << "  Chunk Count   : " << out->chunk_count
                << " (1-to-N Sub-items)" << std::endl;
      std::cout << "  Intent Name   : " << out->intent_name
                << " (Conf: " << std::fixed << std::setprecision(2)
                << out->confidence << ")" << std::endl;
      std::cout << "  LLM Answer    : " << out->answer_text << std::endl;
    }
  }

  Alg_Destroy(handle);
  std::cout << "[Client] Business 3 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 演示业务 4: 智能对话风控质检业务 (3模型+6节点: Embedding + Reranker + LLM)
// =============================================================================
void RunDemoBusiness4_DialogueAudit() {
  std::cout
      << "\n=================================================================="
      << std::endl;
  std::cout << "  【业务 4 演示】智能对话风控质检业务 (3大模型+6节点协同流水线)"
            << std::endl;
  std::cout << "   - Model 1: bge_m3_npu (Embedding 引擎, Batch=4)"
            << std::endl;
  std::cout << "   - Model 2: bge_reranker_large_npu (Cross-Encoder Rerank "
               "引擎, Batch=4)"
            << std::endl;
  std::cout << "   - Model 3: qwen2.5_7b_audit_npu (LLM 推理引擎, Batch=2)"
            << std::endl;
  std::cout
      << "=================================================================="
      << std::endl;

  CompanyAlgParamCreate create_param;
  std::string cfg = GetConfigPath("configs/pipeline_dialogue_audit.json");
  create_param.config_file_path = cfg.c_str();
  create_param.model_root_dir = "./models";
  create_param.device_id = 0;
  create_param.biz_type = ALG_BIZ_TYPE_COMPLIANCE_AUDIT;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &create_param);
  if (ret != 0 || !handle) {
    std::cerr << "Alg_Create failed for Business 4: " << ret << std::endl;
    return;
  }

  // 构造 2 个具有代表性的对话审核请求
  CompanyAuditInputStruct req0;
  req0.request_id = 40001;
  req0.channel_name = "VIP专席客服";
  req0.user_text =
      "亲，平台退款审核太慢了，你加我私人微信转账给我吧，我私下把商品寄给你，还"
      "能返现20元！";

  CompanyAuditInputStruct req1;
  req1.request_id = 40002;
  req1.channel_name = "在线售后IM";
  req1.user_text =
      "您好，您的商品符合7天无理由退货政策，已为您在系统提交退款换货流程，请保"
      "持手机畅通。";

  std::vector<void*> inputs = {&req0, &req1};

  CompanyAuditOutputStruct out0;
  CompanyAuditOutputStruct out1;
  std::vector<void*> outputs = {&out0, &out1};

  std::cout
      << "[Client] Sending 2 dialogue requests to Compliance Audit Pipeline..."
      << std::endl;
  ret = Alg_Process(handle, inputs, outputs);
  if (ret == 0) {
    std::cout << "\n>>> 业务 4 执行结果验证 (多模型协同质检) <<<" << std::endl;
    for (size_t i = 0; i < outputs.size(); ++i) {
      auto* out = static_cast<CompanyAuditOutputStruct*>(outputs[i]);
      std::cout << "-----------------------------------------------------------"
                   "-------"
                << std::endl;
      std::cout << "  Audit #" << i << " | Request ID: " << out->request_id
                << std::endl;
      std::cout << "  Channel       : "
                << (i == 0 ? req0.channel_name : req1.channel_name)
                << std::endl;
      std::cout << "  Dialogue Text : \""
                << (i == 0 ? req0.user_text : req1.user_text) << "\""
                << std::endl;
      std::cout << "  Risk Level    : " << out->risk_level
                << " (Score: " << std::fixed << std::setprecision(2)
                << out->risk_score << ")" << std::endl;
      std::cout << "  Matched Policy: " << out->matched_policy_clause
                << std::endl;
      std::cout << "  Audit Verdict : " << out->audit_verdict_json << std::endl;
    }
  }

  Alg_Destroy(handle);
  std::cout << "[Client] Business 4 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 演示业务 5: 多模态发票票据结构化抽取 (Image + Text -> OCR -> LLM)
// =============================================================================
void RunDemoBusiness5_OcrDocQa() {
  std::cout
      << "\n=================================================================="
      << std::endl;
  std::cout
      << "  【业务 5 演示】智能多模态图文票据问答 (OCR 检测识别 + LLM 结构化)"
      << std::endl;
  std::cout << "   - Model 1: ch_ppocr_v4_det_rec_npu (OCR 引擎, Batch=2)"
            << std::endl;
  std::cout << "   - Model 2: qwen_1.5b_npu (LLM 引擎, Batch=2)" << std::endl;
  std::cout
      << "=================================================================="
      << std::endl;

  CompanyAlgParamCreate param;
  param.config_file_path = "../configs/pipeline_ocr_doc_qa.json";
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_OCR_DOC_QA;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  if (ret != 0 || !handle) {
    std::cerr << "[Client] Failed to create handle for Business 5!"
              << std::endl;
    return;
  }

  CompanyOcrDocInputStruct req0{60001, "./data/invoice_01.jpg",
                                "提取发票代码、号码与总金额"};
  std::vector<void*> inputs = {&req0};

  CompanyOcrDocOutputStruct out0;
  std::vector<void*> outputs = {&out0};

  std::cout << "[Client] Sending 1 invoice image to Alg_Process()..."
            << std::endl;
  ret = Alg_Process(handle, inputs, outputs);
  if (ret == 0) {
    std::cout << "\n>>> 业务 5 执行结果验证 <<<" << std::endl;
    std::cout
        << "------------------------------------------------------------------"
        << std::endl;
    std::cout << "  Request ID     : " << out0.request_id << std::endl;
    std::cout << "  OCR Box Count  : " << out0.detected_box_count << std::endl;
    std::cout << "  Extracted JSON : " << out0.extracted_invoice_json
              << std::endl;
  }

  Alg_Destroy(handle);
  std::cout << "[Client] Business 5 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 演示业务 6: 语音交互识别与意图槽位抽取 (PCM Audio Stream -> ASR -> NLU)
// =============================================================================
void RunDemoBusiness6_AudioAsrIntent() {
  std::cout
      << "\n=================================================================="
      << std::endl;
  std::cout << "  【业务 6 演示】语音识别与意图槽位抽取 (Audio PCM + ASR + NLU)"
            << std::endl;
  std::cout << "   - Model 1: paraformer_asr_npu (Speech ASR 引擎, Batch=2)"
            << std::endl;
  std::cout
      << "=================================================================="
      << std::endl;

  CompanyAlgParamCreate param;
  param.config_file_path = "../configs/pipeline_audio_asr_intent.json";
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_AUDIO_ASR_INTENT;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  if (ret != 0 || !handle) {
    std::cerr << "[Client] Failed to create handle for Business 6!"
              << std::endl;
    return;
  }

  std::vector<float> pcm(16000, 0.01f);
  CompanyAudioInputStruct req0{70001, pcm.data(), static_cast<int>(pcm.size()),
                               16000};
  std::vector<void*> inputs = {&req0};

  CompanyAudioOutputStruct out0;
  std::vector<void*> outputs = {&out0};

  std::cout << "[Client] Sending 1 PCM audio stream to Alg_Process()..."
            << std::endl;
  ret = Alg_Process(handle, inputs, outputs);
  if (ret == 0) {
    std::cout << "\n>>> 业务 6 执行结果验证 <<<" << std::endl;
    std::cout
        << "------------------------------------------------------------------"
        << std::endl;
    std::cout << "  Request ID     : " << out0.request_id << std::endl;
    std::cout << "  ASR Text       : " << out0.transcribed_text << std::endl;
    std::cout << "  Intent / Slots : " << out0.intent_slot_json << std::endl;
  }

  Alg_Destroy(handle);
  std::cout << "[Client] Business 6 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 演示业务 7: 纯语义精排打分业务 (Query + Candidates -> ONNX Cross-Encoder)
// =============================================================================
void RunDemoBusiness7_CrossRerank() {
  std::cout
      << "\n=================================================================="
      << std::endl;
  std::cout << "  【业务 7 演示】纯语义精排打分业务 (ONNX Cross-Encoder Matrix)"
            << std::endl;
  std::cout << "   - Model 1: bge_reranker_large (ONNX Rerank 引擎, Batch=4)"
            << std::endl;
  std::cout
      << "=================================================================="
      << std::endl;

  CompanyAlgParamCreate param;
  param.config_file_path = "../configs/pipeline_cross_rerank.json";
  param.model_root_dir = "./models";
  param.device_id = 0;
  param.biz_type = ALG_BIZ_TYPE_CROSS_RERANK;

  void* handle = nullptr;
  int ret = Alg_Create(&handle, &param);
  if (ret != 0 || !handle) {
    std::cerr << "[Client] Failed to create handle for Business 7!"
              << std::endl;
    return;
  }

  const char* passages[3] = {"条款A: 境外交易加收3%手续费。",
                             "条款B: 售后退款支持7天无理由，原路退回付款账户。",
                             "条款C: 节假日人工客服支持延后一个工作日。"};

  CompanyRerankBatchInputStruct req0;
  req0.request_id = 80001;
  req0.query_text = "怎么办理7天无理由退款？";
  req0.candidate_count = 3;
  for (int i = 0; i < 3; ++i) req0.candidate_passages[i] = passages[i];

  std::vector<void*> inputs = {&req0};
  CompanyRerankBatchOutputStruct out0;
  std::vector<void*> outputs = {&out0};

  std::cout << "[Client] Sending 1 Query with 3 Candidate Passages..."
            << std::endl;
  ret = Alg_Process(handle, inputs, outputs);
  if (ret == 0) {
    std::cout << "\n>>> 业务 7 执行结果验证 <<<" << std::endl;
    std::cout
        << "------------------------------------------------------------------"
        << std::endl;
    std::cout << "  Query Text     : \"" << req0.query_text << "\""
              << std::endl;
    for (int i = 0; i < out0.count; ++i) {
      int idx = out0.sorted_indices[i];
      std::cout << "  Rank #" << i << " [Score " << std::fixed
                << std::setprecision(4) << out0.scores[i] << "] -> "
                << passages[idx] << std::endl;
    }
  }

  Alg_Destroy(handle);
  std::cout << "[Client] Business 7 completed and handle destroyed.\n"
            << std::endl;
}

int main() {
  std::cout
      << "##################################################################"
      << std::endl;
  std::cout
      << "   Alg-SDK Framework Multi-Business End-to-End Test Suite         "
      << std::endl;
  std::cout
      << "   (Verifying Framework Usability across 7 Distinct Modalities)   "
      << std::endl;
  std::cout
      << "##################################################################"
      << std::endl;

  // 全局初始化
  Alg_Init();

  // 依次通过不同配置文件切换并运行 7 个不同的业务
  RunDemoBusiness1_KeywordMatch();
  RunDemoBusiness2_EntityExtraction();
  RunDemoBusiness3_DocQa();
  RunDemoBusiness4_DialogueAudit();
  RunDemoBusiness5_OcrDocQa();
  RunDemoBusiness6_AudioAsrIntent();
  RunDemoBusiness7_CrossRerank();

  // 全局清理
  Alg_DeInit();

  std::cout
      << "##################################################################"
      << std::endl;
  std::cout
      << "   ALL 7 BUSINESSES EXECUTED SUCCESSFULLY VIA CONFIG SWITCHING!   "
      << std::endl;
  std::cout
      << "##################################################################"
      << std::endl;
  return 0;
}
