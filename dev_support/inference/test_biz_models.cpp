#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "contracts/diagnostic.h"
#include "contracts/parameters.h"
#include "engine/backend_registry.h"
#include "engine/fixed_batch_executor.h"
#include "engine/model_identity.h"
#include "engine/model_registry.h"

namespace llm_edgeflow::test {
namespace {
// Demo fixtures expose no vendor execution protocol or fabricated inference
// API.
class FixtureSession final : public IBackendSession {
 public:
  FixtureSession(std::string type, BatchPolicy policy,
                 InferenceConcurrency concurrency)
      : type_(std::move(type)), policy_(policy), concurrency_(concurrency) {}
  const std::string& BackendType() const noexcept override { return type_; }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kFixture;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return concurrency_;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return policy_; }

 private:
  const std::string type_;
  const BatchPolicy policy_;
  const InferenceConcurrency concurrency_;
};

struct BackendParams {
  int fixed_batch_size;
};
class FixtureBackend final : public IInferenceBackend {
 public:
  FixtureBackend(std::string type, size_t dynamic_max,
                 InferenceConcurrency concurrency)
      : type_(std::move(type)),
        dynamic_max_(dynamic_max),
        concurrency_(concurrency) {}
  const std::string& BackendType() const noexcept override { return type_; }
  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec, std::string* diagnostic) noexcept override {
    try {
      if (spec.requested_protocol != ExecutionProtocol::kFixture) {
        SetDiagnosticNoexcept(diagnostic,
                              "Mock Backend requires the fixture protocol");
        return nullptr;
      }
      const auto fixed =
          static_cast<size_t>(spec.Params<BackendParams>().fixed_batch_size);
      return std::make_shared<FixtureSession>(
          type_, BatchPolicy{fixed ? fixed : dynamic_max_, fixed},
          concurrency_);
    } catch (const std::exception& error) {
      SetDiagnosticNoexcept(diagnostic, error.what());
    } catch (...) {
      SetDiagnosticNoexcept(diagnostic, "Unknown mock Backend load exception");
    }
    return nullptr;
  }

 private:
  const std::string type_;
  const size_t dynamic_max_;
  const InferenceConcurrency concurrency_;
};

bool RegisterFixtureBackend(const char* name, size_t dynamic_max,
                            InferenceConcurrency concurrency) {
  BackendDefinition definition;
  definition.backend_type = name;
  definition.description = "Demo mock resource holder; no vendor execution";
  definition.supported_protocols = {ExecutionProtocol::kFixture};
  definition.concurrency = concurrency;
  definition.params = Parameters<BackendParams>{
      Field("fixed_batch_size", &BackendParams::fixed_batch_size)
          .Default(0)
          .Range(0, 16)};
  return BackendRegistry::Instance().Register(
      definition, [name, dynamic_max, concurrency] {
        return std::make_unique<FixtureBackend>(name, dynamic_max, concurrency);
      });
}
const bool kTensorRegistered = RegisterFixtureBackend(
    "test_tensor_backend", 16, InferenceConcurrency::kConcurrent);
const bool kCausalRegistered = RegisterFixtureBackend(
    "test_causal_lm_backend", 1, InferenceConcurrency::kSerialized);

bool RequireFixtureSession(const ModelCreateContext& context,
                           std::string* diagnostic) {
  if (context.backend_session &&
      context.backend_session->Protocol() == ExecutionProtocol::kFixture)
    return true;
  SetDiagnosticNoexcept(diagnostic, "Mock Model requires a fixture session");
  return false;
}

template <typename Derived, typename Capability>
class FixtureModel : public ModelIdentity<Derived, Capability> {
 public:
  inline static constexpr auto kConcurrency = InferenceConcurrency::kConcurrent;
  explicit FixtureModel(std::shared_ptr<IBackendSession> session)
      : session_(std::move(session)) {}

 protected:
  template <typename Input, typename Output, typename Produce>
  int Emit(const std::vector<TraceableItem<Input>>& inputs,
           std::vector<TraceableItem<Output>>* outputs, std::string* diagnostic,
           Produce produce) const noexcept {
    return FixedBatchExecutor::Execute<Input, Output>(
        inputs, session_->GetBatchPolicy(),
        [&](const BatchSlice& slice, std::vector<Output>* values) {
          values->reserve(slice.execution_count);
          for (size_t i = 0; i < slice.execution_count; ++i)
            values->push_back(produce(i < slice.valid_count
                                          ? inputs[slice.offset + i].data
                                          : Input{}));
          return 0;
        },
        outputs, diagnostic);
  }

 private:
  const std::shared_ptr<IBackendSession> session_;
};

struct EmbeddingParams {
  int embedding_dim;
};
std::vector<float> MockEmbedding(int dimensions) {
  std::vector<float> value(static_cast<size_t>(dimensions), 0.0f);
  value.front() = 1.0f;
  return value;
}
class TestBizEmbeddingModel final
    : public FixtureModel<TestBizEmbeddingModel, IEmbeddingModel> {
 public:
  inline static constexpr char kImplName[] = "test_biz_embedding";
  TestBizEmbeddingModel(std::shared_ptr<IBackendSession> session,
                        int dimensions)
      : FixtureModel(std::move(session)), value_(MockEmbedding(dimensions)) {}
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic) {
    if (!RequireFixtureSession(context, diagnostic)) return nullptr;
    return std::make_shared<TestBizEmbeddingModel>(
        context.backend_session,
        context.Params<EmbeddingParams>().embedding_dim);
  }
  int Embed(const TextBatch& inputs, EmbeddingBatch* outputs,
            std::string* diagnostic) noexcept override {
    return Emit(inputs, outputs, diagnostic,
                [this](const std::string&) { return value_; });
  }

 private:
  const std::vector<float> value_;
};

class TestBizRerankModel final
    : public FixtureModel<TestBizRerankModel, IRerankModel> {
 public:
  inline static constexpr char kImplName[] = "test_biz_rerank";
  using FixtureModel::FixtureModel;
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic) {
    if (!RequireFixtureSession(context, diagnostic)) return nullptr;
    (void)context.Params<NoParameters>();
    return std::make_shared<TestBizRerankModel>(context.backend_session);
  }
  int Score(const QueryCandidatesBatch& inputs, ScoreBatch* outputs,
            std::string* diagnostic) noexcept override {
    return Emit(inputs, outputs, diagnostic,
                [](const QueryCandidatePair&) { return 0.5f; });
  }
};

struct LlmParams {
  int max_seq_len;
};
std::string MockSchemaResponse(const std::string& prompt) {
  if (prompt.find("实体与名词抽取助手") != std::string::npos)
    return R"({"nouns":["mock entity"]})";
  if (prompt.find("\"risk_level\"") != std::string::npos)
    return R"({"risk_level":"SAFE","risk_score":0.25,"verdict":"mock verdict","suggestion":"fixture only","mock":true})";
  if (prompt.find("OCR识别出的发票单据") != std::string::npos)
    return R"({"invoice_number":"MOCK-001","amount":1.0,"mock":true})";
  return "[mock] generated text";
}
class TestBizLlmModel final : public FixtureModel<TestBizLlmModel, ILlmModel> {
 public:
  inline static constexpr char kImplName[] = "test_biz_llm";
  using FixtureModel::FixtureModel;
  bool SupportsRandomSeed() const noexcept override { return true; }
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic) {
    if (!RequireFixtureSession(context, diagnostic)) return nullptr;
    (void)context
        .Params<LlmParams>();  // Compatibility field for current mock Profiles.
    return std::make_shared<TestBizLlmModel>(context.backend_session);
  }
  int Generate(const TextBatch& inputs, const GenerateOptions&,
               TextBatch* outputs, std::string* diagnostic) noexcept override {
    return Emit(inputs, outputs, diagnostic, MockSchemaResponse);
  }
};

class TestBizOcrModel final : public FixtureModel<TestBizOcrModel, IOcrModel> {
 public:
  inline static constexpr char kImplName[] = "test_biz_ocr";
  using FixtureModel::FixtureModel;
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic) {
    if (!RequireFixtureSession(context, diagnostic)) return nullptr;
    (void)context.Params<NoParameters>();
    return std::make_shared<TestBizOcrModel>(context.backend_session);
  }
  int Recognize(const ImageFrameBatch& inputs, OcrDocumentBatch* outputs,
                std::string* diagnostic) noexcept override {
    return Emit(inputs, outputs, diagnostic, [](const ImageFrame&) {
      return OcrDocumentItem{{{0, 0, 1, 1, "[mock] invoice text", 1.0f}},
                             "[mock] invoice text"};
    });
  }
};

class TestBizAsrModel final : public FixtureModel<TestBizAsrModel, IAsrModel> {
 public:
  inline static constexpr char kImplName[] = "test_biz_asr";
  using FixtureModel::FixtureModel;
  bool SupportsLanguage(std::string_view language) const noexcept override {
    return language == "zh" || language == "en" || language == "auto";
  }
  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string* diagnostic) {
    if (!RequireFixtureSession(context, diagnostic)) return nullptr;
    (void)context.Params<NoParameters>();
    return std::make_shared<TestBizAsrModel>(context.backend_session);
  }
  int Transcribe(const AudioPcmBatch& inputs, const TranscribeOptions&,
                 TextBatch* outputs,
                 std::string* diagnostic) noexcept override {
    return Emit(inputs, outputs, diagnostic,
                [](const AudioPcmPayload&) { return std::string("播放音乐"); });
  }
};

template <typename Model>
ModelDefinition MockDefinition(const char* backend, ParameterSet params = {}) {
  auto definition = MakeModelDefinition<Model>();
  definition.description =
      "Deterministic Demo mock; not inference or business accuracy evidence";
  definition.required_protocol = ExecutionProtocol::kFixture;
  definition.fixture_backends = {backend};
  definition.params = std::move(params);
  return definition;
}
REGISTER_MODEL_WITH_DEFINITION(TestBizEmbeddingModel,
                               MockDefinition<TestBizEmbeddingModel>(
                                   "test_tensor_backend",
                                   Parameters<EmbeddingParams>{
                                       Field("embedding_dim",
                                             &EmbeddingParams::embedding_dim)
                                           .Default(384)
                                           .Range(1, 65536)}));
REGISTER_MODEL_WITH_DEFINITION(
    TestBizRerankModel,
    MockDefinition<TestBizRerankModel>("test_tensor_backend"));
REGISTER_MODEL_WITH_DEFINITION(
    TestBizLlmModel,
    MockDefinition<TestBizLlmModel>(
        "test_causal_lm_backend",
        Parameters<LlmParams>{Field("max_seq_len", &LlmParams::max_seq_len)
                                  .Default(512)
                                  .Range(1, 1048576)}));
REGISTER_MODEL_WITH_DEFINITION(
    TestBizOcrModel, MockDefinition<TestBizOcrModel>("test_tensor_backend"));
REGISTER_MODEL_WITH_DEFINITION(
    TestBizAsrModel, MockDefinition<TestBizAsrModel>("test_tensor_backend"));
}  // namespace
}  // namespace llm_edgeflow::test
