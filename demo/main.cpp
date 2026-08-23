#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "company_alg_interface.h"
#include "demo/demo_utils.h"
#include "platform/platform_operator_interface.h"

using namespace alg_demo;
using namespace llm_edgeflow::platform;

// =============================================================================
// 通用平台 Operator 标准执行助手函数 (模拟公司外部调度框架基于 OperatorFunc
// 调用)
// =============================================================================
template <typename TIn, typename TOut>
int RunPlatformOperator(const std::string& conf_file,
                        const std::string& input_slot_key,
                        const std::string& output_slot_key,
                        std::vector<TIn>& inputs, std::vector<TOut>& outputs,
                        ControlCommand ctrl_cmd = ControlCommand::kUpdateRules,
                        const char* control_json = nullptr) {
  std::string resolved_conf = ResolvePath(conf_file);

  // 1. 获取平台 Operator 函数表
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();

  // 2. 创建平台会话句柄 (.conf 部署配置 + 芯片类型 + Batch 大小)
  CreateParam param{};
  param.cfg_file_name = resolved_conf.c_str();
  param.platform_config.batch_size =
      std::max(1, static_cast<int>(inputs.size()));
  param.platform_config.device_id = 0;
  param.platform_config.type = ChipType::kAx650;
  param.depth_num = 2;

  void* handle = nullptr;
  int ret = ops.Create(&handle, &param);
  if (ret != 0 || !handle) {
    std::cerr << "[Platform Client] Failed to create handle with conf: "
              << resolved_conf << " (" << GetPlatformLastError() << ")"
              << std::endl;
    return ret;
  }

  // 3. (可选) 动态参数与词表热更新
  if (control_json) {
    std::cout << "[Platform Client] Invoking ops.Control to dynamically push "
                 "parameters..."
              << std::endl;
    ops.Control(handle, ctrl_cmd, const_cast<char*>(control_json));
  }

  // 4. 组装命名 I/O 批容器 (NamedIoBatch)
  outputs.assign(inputs.size(), TOut{});
  NamedIoBatch in_batch(inputs.size());
  NamedIoBatch out_batch(outputs.size());

  for (size_t i = 0; i < inputs.size(); ++i) {
    // 零拷贝借用外部指针
    in_batch[i][input_slot_key] =
        std::shared_ptr<void>(&inputs[i], [](void*) {});
    out_batch[i][output_slot_key] =
        std::shared_ptr<void>(&outputs[i], [](void*) {});
  }

  std::cout << "[Platform Client] Dispatching " << inputs.size()
            << " request(s) via ops.Process (NamedIoBatch)..." << std::endl;
  ret = ops.Process(handle, in_batch, out_batch);
  if (ret != 0) {
    std::cerr << "[Platform Client] ops.Process failed: code=" << ret << " ("
              << GetPlatformLastError() << ")" << std::endl;
  }

  // 5. 安全销毁句柄
  ops.Destroy(handle);
  return ret;
}

// =============================================================================
// 业务 1: 实体/名词提取业务演示 (使用 0.6B LLM)
// =============================================================================
void Demo1_EntityExtract(
    const std::string& conf = "configs/pipeline_entity_extract.conf",
    const std::string& data_file = "data/corpus_entity_extract.txt") {
  PrintBanner("【业务 1 演示】实体/名词提取业务 (使用 0.6B LLM 模型)",
              "Conf: " + conf);

  auto lines = ReadLinesFromFile(data_file);
  if (lines.empty()) {
    lines = {
        "张三在清华大学毕业后加入了一家北京的人工智能公司，作为算法工程师负责NP"
        "U芯片和深度学习大模型的研发项目。"};
  }

  std::vector<CompanyEntityInputStruct> inputs;
  for (size_t i = 0; i < lines.size(); ++i) {
    inputs.push_back({static_cast<uint64_t>(30001 + i), lines[i].c_str()});
  }

  std::vector<CompanyEntityOutputStruct> outputs;
  if (RunPlatformOperator(conf, "nlp_node.entity_in", "nlp_node.entity_out",
                          inputs, outputs) == 0) {
    std::cout << "\n>>> 业务 1 执行结果验证 <<<" << std::endl;
    for (size_t i = 0; i < outputs.size(); ++i) {
      PrintDivider();
      std::cout << "  Input Sentence : \"" << inputs[i].sentence_text << "\"\n"
                << "  Request ID     : " << outputs[i].request_id << "\n"
                << "  Extracted JSON : " << outputs[i].entities_json
                << std::endl;
    }
  }
  std::cout << "[Platform Client] Business 1 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 业务 2: 关注词规则匹配业务演示 (无需模型 / Control 动态词表)
// =============================================================================
void Demo2_KeywordMatch(
    const std::string& conf = "configs/pipeline_keyword_match.conf",
    const std::string& data_file = "data/corpus_keyword_match.txt") {
  PrintBanner(
      "【业务 2 演示】关注词匹配业务 (无需模型 / Control动态词表 / Batch=2)",
      "Conf: " + conf);

  auto lines = ReadLinesFromFile(data_file);
  if (lines.empty()) {
    lines = {"请帮我联系一下VIP专员，我有一笔大客户加急订单需要优先处理。",
             "今天天气真不错，阳光明媚，我想去公园散散步。"};
  }

  std::vector<CompanyKeywordInputStruct> inputs;
  for (size_t i = 0; i < lines.size(); ++i) {
    inputs.push_back({static_cast<uint64_t>(20001 + i), lines[i].c_str()});
  }

  const char* ctrl_json =
      "{\n"
      "  \"categories\": {\n"
      "    \"RISK_COMPLAINT\": [\"投诉\", \"欺诈\", \"假货\", \"黑心商家\"],\n"
      "    \"VIP_SERVICE\": [\"VIP\", \"大客户\", \"加急\", \"专员\"],\n"
      "    \"URGENT_HELP\": [\"报警\", \"救命\", \"紧急\"]\n"
      "  }\n"
      "}";

  std::vector<CompanyKeywordOutputStruct> outputs;
  if (RunPlatformOperator(conf, "client_channel.keyword_in",
                          "client_channel.keyword_out", inputs, outputs,
                          ControlCommand::kUpdateRules, ctrl_json) == 0) {
    std::cout << "\n>>> 业务 2 执行结果验证 <<<" << std::endl;
    for (size_t i = 0; i < outputs.size(); ++i) {
      PrintDivider();
      std::cout << "  Input #" << i << ": \"" << inputs[i].sentence_text
                << "\"\n"
                << "  Request ID : " << outputs[i].request_id << "\n"
                << "  Is Hit     : "
                << (outputs[i].is_hit ? "YES (命中)" : "NO (未命中)") << "\n"
                << "  JSON Output: " << outputs[i].match_result_json
                << std::endl;
    }
  }
  std::cout << "[Platform Client] Business 2 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 业务 3: 智能长文档问答业务演示 (多模型协同: Embedding + LLM)
// =============================================================================
void Demo3_SmartDocQa(const std::string& conf = "configs/pipeline_doc_qa.conf",
                      const std::string& data_file = "data/corpus_doc_qa.txt") {
  PrintBanner("【业务 3 演示】智能长文档问答业务 (多模型协同: Embedding + LLM)",
              "Conf: " + conf);

  auto sections = ParseTagSections(data_file);
  auto docs = sections["DOC"];
  auto queries = sections["QUERY"];
  if (docs.empty() || queries.empty()) {
    docs = {
        "企业级算法框架设计规范：采用4层分层架构，包含C-"
        "ABI适配层、Pipeline调度层、通用算子池与底层硬件引擎抽象。",
        "客户服务售后政策：支持7天无理由退货与全额退款。若商品存在质量问题，由"
        "平台承担双向运费并提供快速换货。"};
    queries = {"请简述该算法框架的架构设计与核心技术？",
               "商品有瑕疵，我想办理退款退货，售后流程是什么？"};
  }

  size_t count = std::min(docs.size(), queries.size());
  std::vector<CompanyDocInputStruct> inputs;
  for (size_t i = 0; i < count; ++i) {
    inputs.push_back({static_cast<uint64_t>(10001 + i), docs[i].c_str(),
                      queries[i].c_str()});
  }

  std::vector<CompanyDocOutputStruct> outputs;
  if (RunPlatformOperator(conf, "rag_channel.doc_in", "rag_channel.doc_out",
                          inputs, outputs) == 0) {
    std::cout << "\n>>> 业务 3 执行结果验证 <<<" << std::endl;
    for (size_t i = 0; i < outputs.size(); ++i) {
      PrintDivider();
      std::cout << "  Result #" << i
                << " | Request ID: " << outputs[i].request_id << "\n"
                << "  Chunk Count   : " << outputs[i].chunk_count
                << " (1-to-N Sub-items)\n"
                << "  Intent Name   : " << outputs[i].intent_name
                << " (Conf: " << std::fixed << std::setprecision(2)
                << outputs[i].confidence << ")\n"
                << "  LLM Answer    : " << outputs[i].answer_text << std::endl;
    }
  }
  std::cout << "[Platform Client] Business 3 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 业务 4: 智能对话风控质检演示 (3大模型+6节点协同流水线)
// =============================================================================
void Demo4_DialogueAudit(
    const std::string& conf = "configs/pipeline_dialogue_audit.conf",
    const std::string& data_file = "data/corpus_dialogue_audit.txt") {
  PrintBanner("【业务 4 演示】智能对话风控质检业务 (3大模型+6节点协同流水线)",
              "Conf: " + conf);

  auto sections = ParseTagSections(data_file);
  auto channels = sections["CHANNEL"];
  auto dialogues = sections["DIALOGUE"];
  if (channels.empty() || dialogues.empty()) {
    channels = {"VIP专席客服", "在线售后IM"};
    dialogues = {
        "亲，平台退款审核太慢了，你加我私人微信转账给我吧，我私下把商品寄给你，"
        "还能返现20元！",
        "您好，您的商品符合7天无理由退货政策，已为您在系统提交退款换货流程，请"
        "保持手机畅通。"};
  }

  size_t count = std::min(channels.size(), dialogues.size());
  std::vector<CompanyAuditInputStruct> inputs;
  for (size_t i = 0; i < count; ++i) {
    inputs.push_back({static_cast<uint64_t>(40001 + i), channels[i].c_str(),
                      dialogues[i].c_str()});
  }

  std::vector<CompanyAuditOutputStruct> outputs;
  if (RunPlatformOperator(conf, "audit_channel.audit_in",
                          "audit_channel.audit_out", inputs, outputs) == 0) {
    std::cout << "\n>>> 业务 4 执行结果验证 (多模型协同质检) <<<" << std::endl;
    for (size_t i = 0; i < outputs.size(); ++i) {
      PrintDivider();
      std::cout << "  Audit #" << i
                << " | Request ID: " << outputs[i].request_id << "\n"
                << "  Channel       : " << channels[i] << "\n"
                << "  Dialogue Text : \"" << dialogues[i] << "\"\n"
                << "  Risk Level    : " << outputs[i].risk_level
                << " (Score: " << std::fixed << std::setprecision(2)
                << outputs[i].risk_score << ")\n"
                << "  Matched Policy: " << outputs[i].matched_policy_clause
                << "\n"
                << "  Audit Verdict : " << outputs[i].audit_verdict_json
                << std::endl;
    }
  }
  std::cout << "[Platform Client] Business 4 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 业务 5: 智能多模态图文票据问答 (OCR 检测识别 + LLM 结构化)
// =============================================================================
void Demo5_OcrDocQa(
    const std::string& conf = "configs/pipeline_ocr_doc_qa.conf",
    const std::string& data_file = "data/corpus_ocr_doc_qa.txt") {
  PrintBanner(
      "【业务 5 演示】智能多模态图文票据问答 (OCR 检测识别 + LLM 结构化)",
      "Conf: " + conf);

  auto sections = ParseTagSections(data_file);
  std::string img = "./data/invoice_01.jpg";
  std::string prompt = "提取发票代码、号码与总金额";
  if (!sections["IMAGE"].empty()) img = sections["IMAGE"][0];
  if (!sections["PROMPT"].empty()) prompt = sections["PROMPT"][0];

  std::vector<CompanyOcrDocInputStruct> inputs = {
      {60001, img.c_str(), prompt.c_str()}};
  std::vector<CompanyOcrDocOutputStruct> outputs;

  if (RunPlatformOperator(conf, "camera_0.frame", "camera_0.od_out", inputs,
                          outputs) == 0) {
    std::cout << "\n>>> 业务 5 执行结果验证 <<<" << std::endl;
    PrintDivider();
    std::cout << "  Request ID     : " << outputs[0].request_id << "\n"
              << "  OCR Box Count  : " << outputs[0].detected_box_count << "\n"
              << "  Extracted JSON : " << outputs[0].extracted_invoice_json
              << std::endl;
  }
  std::cout << "[Platform Client] Business 5 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 业务 6: 语音识别与意图槽位抽取 (Audio PCM + ASR + NLU)
// =============================================================================
void Demo6_AudioAsr(
    const std::string& conf = "configs/pipeline_audio_asr_intent.conf",
    const std::string& data_file = "data/corpus_audio_asr.txt") {
  (void)data_file;
  PrintBanner("【业务 6 演示】语音识别与意图槽位抽取 (Audio PCM + ASR + NLU)",
              "Conf: " + conf);

  std::vector<float> pcm(16000, 0.01f);
  std::vector<CompanyAudioInputStruct> inputs = {
      {70001, pcm.data(), static_cast<int>(pcm.size()), 16000}};
  std::vector<CompanyAudioOutputStruct> outputs;

  if (RunPlatformOperator(conf, "mic_0.audio_in", "mic_0.audio_out", inputs,
                          outputs) == 0) {
    std::cout << "\n>>> 业务 6 执行结果验证 <<<" << std::endl;
    PrintDivider();
    std::cout << "  Request ID     : " << outputs[0].request_id << "\n"
              << "  ASR Text       : " << outputs[0].transcribed_text << "\n"
              << "  Intent / Slots : " << outputs[0].intent_slot_json
              << std::endl;
  }
  std::cout << "[Platform Client] Business 6 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 业务 7: 纯语义精排打分业务 (ONNX Cross-Encoder Matrix)
// =============================================================================
void Demo7_CrossRerank(
    const std::string& conf = "configs/pipeline_cross_rerank.conf",
    const std::string& data_file = "data/corpus_cross_rerank.txt") {
  PrintBanner("【业务 7 演示】纯语义精排打分业务 (ONNX Cross-Encoder Matrix)",
              "Conf: " + conf);

  auto sections = ParseTagSections(data_file);
  std::string query = "怎么办理7天无理由退款？";
  std::vector<std::string> passages = {
      "条款A: 境外交易加收3%手续费。",
      "条款B: 售后退款支持7天无理由，原路退回付款账户。",
      "条款C: 节假日人工客服支持延后一个工作日。"};
  if (!sections["QUERY"].empty()) query = sections["QUERY"][0];
  if (!sections["PASSAGE"].empty()) passages = sections["PASSAGE"];

  CompanyRerankBatchInputStruct req;
  req.request_id = 80001;
  req.query_text = query.c_str();
  req.candidate_count = std::min(static_cast<int>(passages.size()), 8);
  for (int i = 0; i < req.candidate_count; ++i) {
    req.candidate_passages[i] = passages[i].c_str();
  }

  std::vector<CompanyRerankBatchInputStruct> inputs = {req};
  std::vector<CompanyRerankBatchOutputStruct> outputs;

  if (RunPlatformOperator(conf, "ranker.rerank_in", "ranker.rerank_out", inputs,
                          outputs) == 0) {
    std::cout << "\n>>> 业务 7 执行结果验证 <<<" << std::endl;
    PrintDivider();
    std::cout << "  Query Text     : \"" << query << "\"" << std::endl;
    for (int k = 0; k < outputs[0].count; ++k) {
      int orig_idx = outputs[0].sorted_indices[k];
      std::cout << "  Rank #" << k << " [Score " << std::fixed
                << std::setprecision(4) << outputs[0].scores[k] << "] -> "
                << passages[orig_idx] << std::endl;
    }
  }
  std::cout << "[Platform Client] Business 7 completed and handle destroyed.\n"
            << std::endl;
}

// =============================================================================
// 主程序入口：支持 CLI 命令行单业务与全业务全景演示
// =============================================================================
int main(int argc, char* argv[]) {
  std::string conf_path = "";
  std::string data_path = "";
  int target_biz = 0;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg == "-c" || arg == "--config" || arg == "--conf") && i + 1 < argc) {
      conf_path = argv[++i];
    } else if ((arg == "-b" || arg == "--biz") && i + 1 < argc) {
      target_biz = std::stoi(argv[++i]);
    } else if ((arg == "-d" || arg == "--data") && i + 1 < argc) {
      data_path = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [options]\n\n"
                << "Options:\n"
                << "  -b, --biz <id>       Target business ID (1..7)\n"
                << "  -c, --conf <path>    Custom platform .conf path\n"
                << "  -d, --data <path>    Custom test corpus .txt file path\n"
                << "  -h, --help           Show this help message\n\n"
                << "Supported Business IDs:\n"
                << "  1: 实体/名词提取 (Entity Extraction)\n"
                << "  2: 关注词规则匹配 (Keyword Matching)\n"
                << "  3: 智能长文档问答 (Smart Doc QA)\n"
                << "  4: 智能对话风控质检 (Compliance Audit)\n"
                << "  5: 多模态图文票据问答 (OCR Doc QA)\n"
                << "  6: 语音识别与意图抽取 (Audio ASR & NLU)\n"
                << "  7: 纯语义精排打分 (Cross-Encoder Rerank)\n"
                << std::endl;
      return 0;
    }
  }

  // 1. 初始化平台全局环境
  OperatorFunc ops = Get_LLM_EDGEFLOW_OperatorTable();
  if (ops.Init() != 0) {
    std::cerr << "Global Init failed: " << GetPlatformLastError() << std::endl;
    return -1;
  }

  if (target_biz > 0) {
    // 单业务定向运行模式
    switch (target_biz) {
      case 1:
        Demo1_EntityExtract(
            conf_path.empty() ? "configs/pipeline_entity_extract.conf"
                              : conf_path,
            data_path.empty() ? "data/corpus_entity_extract.txt" : data_path);
        break;
      case 2:
        Demo2_KeywordMatch(
            conf_path.empty() ? "configs/pipeline_keyword_match.conf"
                              : conf_path,
            data_path.empty() ? "data/corpus_keyword_match.txt" : data_path);
        break;
      case 3:
        Demo3_SmartDocQa(
            conf_path.empty() ? "configs/pipeline_doc_qa.conf" : conf_path,
            data_path.empty() ? "data/corpus_doc_qa.txt" : data_path);
        break;
      case 4:
        Demo4_DialogueAudit(
            conf_path.empty() ? "configs/pipeline_dialogue_audit.conf"
                              : conf_path,
            data_path.empty() ? "data/corpus_dialogue_audit.txt" : data_path);
        break;
      case 5:
        Demo5_OcrDocQa(
            conf_path.empty() ? "configs/pipeline_ocr_doc_qa.conf" : conf_path,
            data_path.empty() ? "data/corpus_ocr_doc_qa.txt" : data_path);
        break;
      case 6:
        Demo6_AudioAsr(
            conf_path.empty() ? "configs/pipeline_audio_asr_intent.conf"
                              : conf_path,
            data_path.empty() ? "data/corpus_audio_asr.txt" : data_path);
        break;
      case 7:
        Demo7_CrossRerank(
            conf_path.empty() ? "configs/pipeline_cross_rerank.conf"
                              : conf_path,
            data_path.empty() ? "data/corpus_cross_rerank.txt" : data_path);
        break;
      default:
        std::cerr << "Unsupported business ID: " << target_biz << std::endl;
        break;
    }
  } else {
    // 默认全业务全景演示巡检模式 (1 ~ 7)
    std::cout
        << "#############################################################"
           "#####\n"
        << "   LLM-EdgeFlow 全业务全景端到端演示 (Platform Operator Runner) "
           " \n"
        << "#############################################################"
           "#####\n";

    Demo1_EntityExtract();
    Demo2_KeywordMatch();
    Demo3_SmartDocQa();
    Demo4_DialogueAudit();
    Demo5_OcrDocQa();
    Demo6_AudioAsr();
    Demo7_CrossRerank();

    std::cout
        << "#############################################################"
           "#####\n"
        << "   ALL 7 BUSINESSES EXECUTED SUCCESSFULLY VIA OPERATOR TABLE!   "
           "\n"
        << "#############################################################"
           "#####\n";
  }

  // 2. 释放平台全局资源
  ops.Deinit();
  return 0;
}
