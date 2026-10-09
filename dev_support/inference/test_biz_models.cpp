#include "dev_support/inference/test_biz_models.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "contracts/parameters.h"
#include "engine/backend_interface.h"
#include "engine/fixed_batch_executor.h"

namespace llm_edgeflow {
namespace test {
namespace {

struct EmbeddingParams {
  int embedding_dim = 384;
};

Parameters<EmbeddingParams> EmbeddingParamSpec() {
  return Parameters<EmbeddingParams>(
      {Field("embedding_dim", &EmbeddingParams::embedding_dim)
           .Default(384)
           .Range(1, 65536)});
}

struct LlmParams {
  int max_seq_len = 512;
};

Parameters<LlmParams> LlmParamSpec() {
  return Parameters<LlmParams>({Field("max_seq_len", &LlmParams::max_seq_len)
                                    .Default(512)
                                    .Range(1, 1048576)});
}

bool RequireSession(const ModelCreateContext& context,
                    std::string* diagnostic) noexcept {
  if (!context.backend_session) {
    if (diagnostic) *diagnostic = "Test fixture backend session is null";
    return false;
  }
  if (context.backend_session->Protocol() != ExecutionProtocol::kFixture) {
    if (diagnostic) *diagnostic = "Test model requires fixture protocol";
    return false;
  }
  return true;
}

std::string GenerateBizResponse(const std::string& prompt) {
  if (prompt.find("实体") != std::string::npos ||
      prompt.find("名词") != std::string::npos) {
    return R"({"nouns":["张三","清华大学","北京","人工智能","算法工程师","NPU","芯片","深度学习","大模型","项目","公司"]})";
  }
  if (prompt.find("高风险") != std::string::npos ||
      prompt.find("私下") != std::string::npos ||
      prompt.find("转账") != std::string::npos ||
      prompt.find("微信") != std::string::npos) {
    return R"({"risk_level":"HIGH_RISK","risk_score":0.96,"verdict":"严重违规：存在诱导私下交易或索取敏感隐私风险","suggestion":"请立即终止私下交易引导，严格使用官方交易链路保障权益。"})";
  }
  if (prompt.find("风险") != std::string::npos ||
      prompt.find("合规") != std::string::npos) {
    return R"({"risk_level":"SAFE","risk_score":0.10,"verdict":"合规正常：对话符合客服标准行为规范","suggestion":"对话正常，继续保持专业服务。"})";
  }
  if (prompt.find("发票") != std::string::npos ||
      prompt.find("单据") != std::string::npos ||
      prompt.find("OCR") != std::string::npos) {
    return R"({"invoice_code":"011002200111","invoice_number":"88765432","total_amount":"¥1280.00","purchaser":"某某科技创新有限公司","tax_amount":"¥76.80"})";
  }
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

ModelDefinition Definition(const char* impl_name, const char* model_type,
                           const char* backend, ParameterSet params) {
  ModelDefinition definition;
  definition.impl_name = impl_name;
  definition.model_type = model_type;
  definition.description = "Test-only business response fixture model";
  definition.required_protocol = ExecutionProtocol::kFixture;
  definition.fixture_backends = {backend};
  definition.concurrency = InferenceConcurrency::kSerialized;
  definition.params = std::move(params);
  return definition;
}

}  // namespace

TestBizEmbeddingModel::TestBizEmbeddingModel(
    std::shared_ptr<IBackendSession> session, size_t embedding_dim)
    : embedding_dim_(embedding_dim), session_(std::move(session)) {}

std::shared_ptr<IModel> TestBizEmbeddingModel::Create(
    const ModelCreateContext& context, std::string* diagnostic) {
  if (!RequireSession(context, diagnostic)) return nullptr;
  const auto& params = context.Params<EmbeddingParams>();
  return std::make_shared<TestBizEmbeddingModel>(context.backend_session,
                                                 params.embedding_dim);
}

const std::string& TestBizEmbeddingModel::ImplName() const noexcept {
  static const std::string type = kImplName;
  return type;
}
const std::string& TestBizEmbeddingModel::ModelType() const noexcept {
  static const std::string model_type = "embedding";
  return model_type;
}
InferenceConcurrency TestBizEmbeddingModel::Concurrency() const noexcept {
  return InferenceConcurrency::kSerialized;
}
int TestBizEmbeddingModel::Embed(const TextBatch& inputs,
                                 EmbeddingBatch* outputs,
                                 std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  const BatchPolicy policy = session_->GetBatchPolicy();
  return FixedBatchExecutor::Execute<std::string, std::vector<float>>(
      inputs, policy,
      [this, &inputs](const BatchSlice& slice,
                      std::vector<std::vector<float>>* batch_outputs) {
        batch_outputs->assign(slice.execution_count,
                              std::vector<float>(embedding_dim_, 0.0f));
        for (size_t i = 0; i < slice.valid_count; ++i) {
          auto& vector = (*batch_outputs)[i];
          const auto& text = inputs[slice.offset + i].data;
          for (size_t c = 0; c < text.size(); ++c) {
            const size_t index =
                (static_cast<unsigned char>(text[c]) * 31 + c * 17) %
                embedding_dim_;
            vector[index] +=
                1.0f +
                static_cast<float>(static_cast<unsigned char>(text[c]) % 7);
          }
          float norm = 0.0f;
          for (float value : vector) norm += value * value;
          norm = std::sqrt(norm);
          if (norm > 1.0e-6f) {
            for (float& value : vector) value /= norm;
          }
        }
        return 0;
      },
      outputs);
}

TestBizRerankModel::TestBizRerankModel(std::shared_ptr<IBackendSession> session)
    : session_(std::move(session)) {}

std::shared_ptr<IModel> TestBizRerankModel::Create(
    const ModelCreateContext& context, std::string* diagnostic) {
  if (!RequireSession(context, diagnostic)) return nullptr;
  (void)context.Params<NoParameters>();
  return std::make_shared<TestBizRerankModel>(context.backend_session);
}
const std::string& TestBizRerankModel::ImplName() const noexcept {
  static const std::string type = kImplName;
  return type;
}
const std::string& TestBizRerankModel::ModelType() const noexcept {
  static const std::string model_type = "rerank";
  return model_type;
}
InferenceConcurrency TestBizRerankModel::Concurrency() const noexcept {
  return InferenceConcurrency::kSerialized;
}
int TestBizRerankModel::Score(const QueryCandidatesBatch& inputs,
                              ScoreBatch* outputs,
                              std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  const BatchPolicy policy = session_->GetBatchPolicy();
  return FixedBatchExecutor::Execute<QueryCandidatePair, float>(
      inputs, policy,
      [&inputs](const BatchSlice& slice, std::vector<float>* batch_outputs) {
        batch_outputs->assign(slice.execution_count, 0.0f);
        const std::vector<std::string> words = {"违规", "欺诈", "退款", "敏感",
                                                "泄密", "账号", "密码", "保密",
                                                "合规", "禁止"};
        for (size_t i = 0; i < slice.valid_count; ++i) {
          const auto& pair = inputs[slice.offset + i].data;
          float score = 0.2f;
          for (const auto& word : words) {
            if (pair.query.find(word) != std::string::npos &&
                pair.candidate.find(word) != std::string::npos) {
              score += 0.35f;
            }
          }
          (*batch_outputs)[i] = std::min(1.0f, score);
        }
        return 0;
      },
      outputs);
}

TestBizLlmModel::TestBizLlmModel(std::shared_ptr<IBackendSession> session)
    : session_(std::move(session)) {}
std::shared_ptr<IModel> TestBizLlmModel::Create(
    const ModelCreateContext& context, std::string* diagnostic) {
  if (!RequireSession(context, diagnostic)) return nullptr;
  (void)context.Params<LlmParams>();
  return std::make_shared<TestBizLlmModel>(context.backend_session);
}
const std::string& TestBizLlmModel::ImplName() const noexcept {
  static const std::string type = kImplName;
  return type;
}
const std::string& TestBizLlmModel::ModelType() const noexcept {
  static const std::string model_type = "llm";
  return model_type;
}
InferenceConcurrency TestBizLlmModel::Concurrency() const noexcept {
  return InferenceConcurrency::kSerialized;
}
int TestBizLlmModel::Generate(const TextBatch& prompts, const GenerateOptions&,
                              TextBatch* outputs,
                              std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  const BatchPolicy policy = session_->GetBatchPolicy();
  return FixedBatchExecutor::Execute<std::string, std::string>(
      prompts, policy,
      [&prompts](const BatchSlice& slice,
                 std::vector<std::string>* batch_outputs) {
        batch_outputs->assign(slice.execution_count, std::string());
        for (size_t i = 0; i < slice.valid_count; ++i) {
          (*batch_outputs)[i] =
              GenerateBizResponse(prompts[slice.offset + i].data);
        }
        return 0;
      },
      outputs);
}

TestBizOcrModel::TestBizOcrModel(std::shared_ptr<IBackendSession> session)
    : session_(std::move(session)) {}
std::shared_ptr<IModel> TestBizOcrModel::Create(
    const ModelCreateContext& context, std::string* diagnostic) {
  if (!RequireSession(context, diagnostic)) return nullptr;
  (void)context.Params<NoParameters>();
  return std::make_shared<TestBizOcrModel>(context.backend_session);
}
const std::string& TestBizOcrModel::ImplName() const noexcept {
  static const std::string type = kImplName;
  return type;
}
const std::string& TestBizOcrModel::ModelType() const noexcept {
  static const std::string model_type = "ocr";
  return model_type;
}
InferenceConcurrency TestBizOcrModel::Concurrency() const noexcept {
  return InferenceConcurrency::kSerialized;
}
int TestBizOcrModel::Recognize(const ImageRefBatch& images,
                               OcrDocumentBatch* outputs,
                               std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  const BatchPolicy policy = session_->GetBatchPolicy();
  return FixedBatchExecutor::Execute<std::string, OcrDocumentItem>(
      images, policy,
      [](const BatchSlice& slice, std::vector<OcrDocumentItem>* batch_outputs) {
        batch_outputs->assign(slice.execution_count, OcrDocumentItem());
        for (size_t i = 0; i < slice.valid_count; ++i) {
          auto& document = (*batch_outputs)[i];
          document.boxes = {
              {10.0f, 20.0f, 200.0f, 30.0f, "发票代码: 011002200111", 0.99f},
              {10.0f, 60.0f, 180.0f, 30.0f, "发票号码: 88765432", 0.99f},
              {10.0f, 100.0f, 150.0f, 30.0f, "开票日期: 2026年08月15日", 0.98f},
              {10.0f, 140.0f, 220.0f, 35.0f,
               "购买方名称: 北京某某科技有限责任公司", 0.97f},
              {10.0f, 180.0f, 120.0f, 30.0f, "价税合计(大写): 壹万贰仟元整",
               0.99f},
              {10.0f, 220.0f, 100.0f, 30.0f, "小写金额: ¥12000.00", 0.99f}};
          for (const auto& box : document.boxes) {
            if (!document.combined_text.empty()) document.combined_text += "\n";
            document.combined_text += box.text;
          }
        }
        return 0;
      },
      outputs);
}

TestBizAsrModel::TestBizAsrModel(std::shared_ptr<IBackendSession> session)
    : session_(std::move(session)) {}
std::shared_ptr<IModel> TestBizAsrModel::Create(
    const ModelCreateContext& context, std::string* diagnostic) {
  if (!RequireSession(context, diagnostic)) return nullptr;
  (void)context.Params<NoParameters>();
  return std::make_shared<TestBizAsrModel>(context.backend_session);
}
const std::string& TestBizAsrModel::ImplName() const noexcept {
  static const std::string type = kImplName;
  return type;
}
const std::string& TestBizAsrModel::ModelType() const noexcept {
  static const std::string model_type = "asr";
  return model_type;
}
InferenceConcurrency TestBizAsrModel::Concurrency() const noexcept {
  return InferenceConcurrency::kSerialized;
}
int TestBizAsrModel::Transcribe(const AudioPcmBatch& audio,
                                const TranscribeOptions&, TextBatch* outputs,
                                std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  const BatchPolicy policy = session_->GetBatchPolicy();
  return FixedBatchExecutor::Execute<AudioPcmPayload, std::string>(
      audio, policy,
      [&audio](const BatchSlice& slice,
               std::vector<std::string>* batch_outputs) {
        batch_outputs->assign(slice.execution_count, std::string());
        for (size_t i = 0; i < slice.valid_count; ++i) {
          float sum = 0.0f;
          for (float value : audio[slice.offset + i].data.pcm_data)
            sum += value;
          (*batch_outputs)[i] =
              sum > 120.0f
                  ? "帮我导航到清华科技园，避开拥堵路段。"
                  : (sum > 40.0f ? "今天北京天气怎么样？"
                                 : "把空调温度调到24度，风量开到二档。");
        }
        return 0;
      },
      outputs);
}

static const ModelDefinition kEmbeddingDefinition =
    Definition(TestBizEmbeddingModel::kImplName, "embedding",
               "test_tensor_backend", EmbeddingParamSpec());
static const ModelDefinition kRerankDefinition = Definition(
    TestBizRerankModel::kImplName, "rerank", "test_tensor_backend", {});
static const ModelDefinition kLlmDefinition =
    Definition(TestBizLlmModel::kImplName, "llm", "test_causal_lm_backend",
               LlmParamSpec());
static const ModelDefinition kOcrDefinition =
    Definition(TestBizOcrModel::kImplName, "ocr", "test_tensor_backend", {});
static const ModelDefinition kAsrDefinition =
    Definition(TestBizAsrModel::kImplName, "asr", "test_tensor_backend", {});

REGISTER_MODEL_WITH_DEFINITION(TestBizEmbeddingModel, kEmbeddingDefinition);
REGISTER_MODEL_WITH_DEFINITION(TestBizRerankModel, kRerankDefinition);
REGISTER_MODEL_WITH_DEFINITION(TestBizLlmModel, kLlmDefinition);
REGISTER_MODEL_WITH_DEFINITION(TestBizOcrModel, kOcrDefinition);
REGISTER_MODEL_WITH_DEFINITION(TestBizAsrModel, kAsrDefinition);

}  // namespace test
}  // namespace llm_edgeflow
