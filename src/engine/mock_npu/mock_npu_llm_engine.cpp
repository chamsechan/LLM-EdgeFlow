#include "engine/mock_npu/mock_npu_llm_engine.h"

#include <iostream>

#include "engine/engine_registry.h"

namespace alg_framework {

MockNpuLlmEngine::MockNpuLlmEngine() = default;

bool MockNpuLlmEngine::Load(const std::string& model_path,
                            const nlohmann::json& engine_config) {
  model_path_ = model_path;
  max_batch_size_ = engine_config.value("max_batch_size", 2);
  max_seq_len_ = engine_config.value("max_seq_len", 1024);
  device_id_ = engine_config.value("device_id", -1);
  is_loaded_ = true;
  std::cout << "[MockNpuLlmEngine] Loaded LLM from: " << model_path
            << ", Fixed MaxBatchSize: " << max_batch_size_
            << ", MaxSeqLen: " << max_seq_len_ << ", Device: " << device_id_
            << std::endl;
  return true;
}

const std::string& MockNpuLlmEngine::EngineType() const {
  static const std::string type = kEngineType;
  return type;
}

int MockNpuLlmEngine::Generate(const std::string& prompt,
                               const GenerateOption& opt,
                               std::string* output_text) {
  (void)opt;
  if (!is_loaded_ || !output_text) return -2001;
  *output_text = GenerateSingleResponse(prompt);
  return 0;
}

int MockNpuLlmEngine::InferTraceableBatch(
    const std::vector<TraceableItem<std::string>>& input_prompts,
    const GenerateOption& opt,
    std::vector<TraceableItem<std::string>>* output_texts) {
  (void)opt;
  if (!is_loaded_) return -2001;

  std::string dummy_pad = "<PAD_PROMPT>";

  return FixedBatchExecutor::Execute<std::string, std::string>(
      input_prompts, max_batch_size_, dummy_pad,
      [this](const std::vector<std::string>& batch_in,
             std::vector<std::string>* batch_out) {
        return this->RawNpuHardwareLlmInfer(batch_in, batch_out);
      },
      output_texts);
}

int MockNpuLlmEngine::RawNpuHardwareLlmInfer(
    const std::vector<std::string>& batch_prompts,
    std::vector<std::string>* batch_outputs) {
  if (batch_prompts.size() != max_batch_size_) {
    std::cerr << "[MockNpuLlmEngine] HARDWARE ERROR: Batch size "
              << batch_prompts.size() << " != Fixed MaxBatch "
              << max_batch_size_ << std::endl;
    return -2002;
  }

  std::cout
      << "  [NPU Hardware] Executing NPU LLM Generation kernel with batch="
      << max_batch_size_ << std::endl;
  batch_outputs->resize(max_batch_size_);

  for (size_t i = 0; i < max_batch_size_; ++i) {
    if (batch_prompts[i] == "<PAD_PROMPT>") {
      (*batch_outputs)[i] = "";
    } else {
      (*batch_outputs)[i] = GenerateSingleResponse(batch_prompts[i]);
    }
  }

  return 0;
}

std::string MockNpuLlmEngine::GenerateSingleResponse(
    const std::string& prompt) {
  // 1. 实体与名词提取请求
  if (prompt.find("实体") != std::string::npos ||
      prompt.find("名词") != std::string::npos) {
    std::vector<std::string> extracted_nouns;
    std::vector<std::string> candidates = {
        "北京",     "上海",       "深圳",   "杭州", "清华大学", "浙江大学",
        "人工智能", "算法工程师", "公司",   "芯片", "NPU",      "GPU",
        "手机",     "电脑",       "张三",   "李四", "项目",     "架构",
        "深度学习", "大模型",     "知识库", "售后", "订单"};
    for (const auto& w : candidates) {
      if (prompt.find(w) != std::string::npos) {
        extracted_nouns.push_back(w);
      }
    }
    if (extracted_nouns.empty()) {
      extracted_nouns.push_back("自然语言");
      extracted_nouns.push_back("文本");
    }
    nlohmann::json j;
    j["nouns"] = extracted_nouns;
    return j.dump();
  }

  // 2. 合规质检与风控审核请求
  if (prompt.find("合规") != std::string::npos ||
      prompt.find("风控") != std::string::npos ||
      prompt.find("质检") != std::string::npos) {
    if (prompt.find("转账") != std::string::npos ||
        prompt.find("微信") != std::string::npos ||
        prompt.find("私下") != std::string::npos ||
        prompt.find("密码") != std::string::npos ||
        prompt.find("欺诈") != std::string::npos) {
      return "{\"risk_level\":\"HIGH_RISK\",\"risk_score\":0.96,\"verdict\":"
             "\"严重违规：存在诱导私下交易或索取敏感隐私风险\",\"suggestion\":"
             "\"请立即终止私下交易引导，严格使用官方交易链路保障权益。\"}";
    }
    return "{\"risk_level\":\"SAFE\",\"risk_score\":0.10,\"verdict\":"
           "\"合规正常：对话符合客服标准行为规范\",\"suggestion\":"
           "\"对话正常，继续保持专业服务。\"}";
  }

  // 3. 发票与单据 OCR 提取请求
  if (prompt.find("发票") != std::string::npos ||
      prompt.find("单据") != std::string::npos ||
      prompt.find("OCR") != std::string::npos) {
    return "{\"invoice_code\": \"011002200111\", \"invoice_number\": "
           "\"88765432\", "
           "\"total_amount\": \"¥1280.00\", \"purchaser\": "
           "\"某某科技创新有限公司\", "
           "\"tax_amount\": \"¥76.80\"}";
  }

  // 4. 意图与问答请求
  if (prompt.find("退款") != std::string::npos ||
      prompt.find("退货") != std::string::npos) {
    return "【LLM意图分析】检测到售后退款诉求。建议操作：7天无理由退货审核流程"
           "。";
  }
  if (prompt.find("技术") != std::string::npos ||
      prompt.find("架构") != std::string::npos) {
    return "【LLM总结】文档核心为现代软件工程化设计，包含松耦合、状态隔离与跨平"
           "台编译。";
  }
  return "【LLM标准答复】已根据输入文档上下文完成智能检索与摘要生成。";
}

EngineDefinition MakeMockNpuLlmDefinition() {
  EngineDefinition def;
  def.engine_type = MockNpuLlmEngine::kEngineType;
  def.capability = "llm";
  def.description = "Mock NPU LLM engine";
  def.config_fields = {
      ConfigFieldDefinition{"max_batch_size", ConfigValueKind::kInteger, false,
                            2, 1.0, 4096.0},
      ConfigFieldDefinition{"max_seq_len", ConfigValueKind::kInteger, false,
                            1024, 1.0, 1048576.0},
      ConfigFieldDefinition{"device_id", ConfigValueKind::kInteger, false, -1,
                            -1.0, 1024.0}};
  def.thread_model = EngineThreadModel::kSerialized;
  return def;
}

REGISTER_ENGINE_WITH_DEFINITION(MockNpuLlmEngine, MakeMockNpuLlmDefinition());

}  // namespace alg_framework
