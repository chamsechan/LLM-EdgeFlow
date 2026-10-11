#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "contracts/diagnostic.h"
#include "contracts/parameters.h"
#include "edgeflow/operator/interface.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {
namespace {
namespace api = operator_api;
using Json = nlohmann::json;
constexpr char kBusinessBackend[] = "business_response_fixture";

// Only the model boundary is replaced. Converters, planning and Nodes are real.
class BusinessSession final : public IBackendSession {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string name = kBusinessBackend;
    return name;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kFixture;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return {64, 0}; }
};

class BusinessBackend final : public IInferenceBackend {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string name = kBusinessBackend;
    return name;
  }
  std::shared_ptr<IBackendSession> Load(const BackendLoadSpec& spec,
                                        std::string* error) noexcept override {
    try {
      if (spec.requested_protocol != ExecutionProtocol::kFixture)
        return nullptr;
      (void)spec.Params<NoParameters>();
      return std::make_shared<BusinessSession>();
    } catch (...) {
      SetDiagnosticNoexcept(error, "Cannot load business fixture session");
      return nullptr;
    }
  }
};

template <typename Interface>
class BusinessModel : public Interface {
 public:
  BusinessModel(std::string type, std::shared_ptr<IBackendSession> session)
      : type_(std::move(type)),
        impl_("business_response_" + type_),
        session_(std::move(session)) {}
  const std::string& ImplName() const noexcept override { return impl_; }
  const std::string& ModelType() const noexcept override { return type_; }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }

 private:
  std::string type_;
  std::string impl_;
  std::shared_ptr<IBackendSession> session_;
};

struct ScriptParams {
  std::map<std::string, std::string> responses;
};
class ScriptedBusinessLlm final : public BusinessModel<ILlmModel> {
 public:
  ScriptedBusinessLlm(const ModelCreateContext& context)
      : BusinessModel("llm", context.backend_session),
        responses_(context.Params<ScriptParams>().responses) {}
  int Generate(const TextBatch& prompts, const GenerateOptions&,
               TextBatch* outputs, std::string* error) noexcept override {
    try {
      outputs->clear();
      for (const auto& prompt : prompts) {
        const auto found = responses_.find(prompt.data);
        if (found == responses_.end()) {
          outputs->clear();
          SetDiagnosticNoexcept(error,
                                "Unexpected complete prompt: " + prompt.data);
          return -1;
        }
        outputs->emplace_back(prompt.req_id, prompt.sub_id, found->second);
      }
      return 0;
    } catch (...) {
      outputs->clear();
      SetDiagnosticNoexcept(error, "Business LLM fixture failed");
      return -1;
    }
  }

 private:
  const std::map<std::string, std::string> responses_;
};

class BusinessEmbedding final : public BusinessModel<IEmbeddingModel> {
 public:
  explicit BusinessEmbedding(const ModelCreateContext& ctx)
      : BusinessModel("embedding", ctx.backend_session) {}
  int Embed(const TextBatch& inputs, EmbeddingBatch* outputs,
            std::string*) noexcept override {
    try {
      outputs->clear();
      for (const auto& input : inputs) {
        const bool first = input.data.find("AAAA") != std::string::npos ||
                           input.data.find("CCCC") != std::string::npos ||
                           input.data.find("退款") != std::string::npos;
        outputs->emplace_back(
            input.req_id, input.sub_id,
            first ? std::vector<float>{1, 0} : std::vector<float>{0, 1});
      }
      return 0;
    } catch (...) {
      outputs->clear();
      return -1;
    }
  }
};

class BusinessRerank final : public BusinessModel<IRerankModel> {
 public:
  explicit BusinessRerank(const ModelCreateContext& ctx)
      : BusinessModel("rerank", ctx.backend_session) {}
  int Score(const QueryCandidatesBatch& inputs, ScoreBatch* outputs,
            std::string*) noexcept override {
    try {
      outputs->clear();
      for (const auto& input : inputs)
        outputs->emplace_back(
            input.req_id, input.sub_id,
            input.data.candidate.find(input.data.query) != std::string::npos
                ? 0.75f
                : 0.25f);
      return 0;
    } catch (...) {
      outputs->clear();
      return -1;
    }
  }
};

class BusinessOcr final : public BusinessModel<IOcrModel> {
 public:
  explicit BusinessOcr(const ModelCreateContext& ctx)
      : BusinessModel("ocr", ctx.backend_session) {}
  int Recognize(const ImageFrameBatch& inputs, OcrDocumentBatch* outputs,
                std::string*) noexcept override {
    try {
      outputs->clear();
      for (const auto& input : inputs) {
        const bool first = input.data.data[0] == 11;
        OcrDocumentItem document;
        document.combined_text = first ? "编号11" : "编号22";
        document.boxes.assign(first ? 1 : 2,
                              OcrBoxRecord{1, 2, 3, 4, "文字", 1.0f});
        outputs->emplace_back(input.req_id, input.sub_id, std::move(document));
      }
      return 0;
    } catch (...) {
      outputs->clear();
      return -1;
    }
  }
};

class BusinessAsr final : public BusinessModel<IAsrModel> {
 public:
  explicit BusinessAsr(const ModelCreateContext& ctx)
      : BusinessModel("asr", ctx.backend_session) {}
  bool SupportsLanguage(std::string_view language) const noexcept override {
    return language == "zh";
  }
  int Transcribe(const AudioPcmBatch& inputs, const TranscribeOptions&,
                 TextBatch* outputs, std::string*) noexcept override {
    try {
      outputs->clear();
      for (const auto& input : inputs)
        outputs->emplace_back(
            input.req_id, input.sub_id,
            input.data.pcm_data[0] == 0.25f ? "播放音乐" : "导航到北京");
      return 0;
    } catch (...) {
      outputs->clear();
      return -1;
    }
  }
};

bool RegisterBusinessFixtures() {
  static const bool registered = [] {
    BackendDefinition backend;
    backend.backend_type = kBusinessBackend;
    backend.supported_protocols = {ExecutionProtocol::kFixture};
    backend.concurrency = InferenceConcurrency::kConcurrent;
    if (!BackendRegistry::Instance().Register(
            backend, [] { return std::make_unique<BusinessBackend>(); }))
      return false;
    const auto add = [](const char* category, ModelRegistry::Creator creator,
                        ParameterSet params = {}) {
      ModelDefinition model;
      model.impl_name = std::string("business_response_") + category;
      model.model_type = category;
      model.required_protocol = ExecutionProtocol::kFixture;
      model.fixture_backends = {kBusinessBackend};
      model.concurrency = InferenceConcurrency::kConcurrent;
      model.params = std::move(params);
      return ModelRegistry::Instance().Register(model, std::move(creator));
    };
    return add(
               "llm",
               [](const ModelCreateContext& ctx, std::string*) {
                 return std::make_shared<ScriptedBusinessLlm>(ctx);
               },
               Parameters<ScriptParams>{
                   Field("responses", &ScriptParams::responses).Required()}) &&
           add("embedding",
               [](const ModelCreateContext& ctx, std::string*) {
                 return std::make_shared<BusinessEmbedding>(ctx);
               }) &&
           add("rerank",
               [](const ModelCreateContext& ctx, std::string*) {
                 return std::make_shared<BusinessRerank>(ctx);
               }) &&
           add("ocr",
               [](const ModelCreateContext& ctx, std::string*) {
                 return std::make_shared<BusinessOcr>(ctx);
               }) &&
           add("asr", [](const ModelCreateContext& ctx, std::string*) {
             return std::make_shared<BusinessAsr>(ctx);
           });
  }();
  return registered;
}

Json Model(const char* type, Json params = Json::object()) {
  return {{"type", type},
          {"name", type},
          {"file", "neutral.fixture"},
          {"params", std::move(params)},
          {"backend", {{"type", kBusinessBackend}}}};
}
Json Node(const char* type, const char* name, Json inputs,
          Json params = Json::object()) {
  return {{"type", type},
          {"name", name},
          {"inputs", std::move(inputs)},
          {"params", std::move(params)}};
}
Json Generate(const std::string& input) {
  return Node(
      "llm_generate", "generate", {{"input", input}},
      {{"bind_model", "llm"}, {"endpoints", {{"answer", Json::object()}}}});
}
Json Pipeline(const char* business, Json inputs, Json outputs, Json nodes,
              Json models) {
  Json inbound = Json::array();
  for (const auto& type : inputs)
    inbound.push_back({{"type", type}, {"name", business}});
  return {{"io",
           {{"input", std::move(inbound)},
            {"output", Json::array({{{"type", "json_out"},
                                     {"name", business},
                                     {"inputs", std::move(outputs)}}})}}},
          {"pipeline", std::move(nodes)},
          {"models", std::move(models)}};
}
CompanyString Borrow(std::string& value) {
  return {static_cast<int32_t>(value.size()), value.data()};
}

class BusinessResponsesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(RegisterBusinessFixtures());
    char directory[] = "/tmp/edgeflow-business-responses-XXXXXX";
    ASSERT_NE(mkdtemp(directory), nullptr);
    directory_ = directory;
    ASSERT_EQ(ops_.Init(), 0) << api::GetOperatorLastError();
    initialized_ = true;
  }
  void TearDown() override {
    outputs_.clear();
    if (handle_) {
      EXPECT_EQ(ops_.Destroy(handle_), 0) << api::GetOperatorLastError();
    }
    if (initialized_) {
      EXPECT_EQ(ops_.DeInit(), 0);
    }
    if (!directory_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(directory_, error);
      EXPECT_FALSE(error) << error.message();
    }
  }
  void Create(Json pipeline) {
    std::ofstream(directory_ / "pipeline.json") << pipeline.dump(2);
    std::ofstream(directory_ / "deploy.conf")
        << R"({"pipe_path":"pipeline.json"})";
    std::ofstream(directory_ / "neutral.fixture") << "model boundary fixture";
    api::CreateParam create;
    const std::string model_root = directory_.string();
    create.model_path = model_root.c_str();
    create.cfg_file_name = "deploy.conf";
    create.compute_platform = api::ComputePlatform::kCpu;
    create.max_frame_depth = 2;
    ASSERT_EQ(ops_.Create(&handle_, &create), 0) << api::GetOperatorLastError();
    ASSERT_NE(handle_, nullptr);
  }
  void Check(const api::NamedIoBatch& inputs, int service,
             const std::array<Json, 2>& expected) {
    ASSERT_NE(handle_, nullptr);
    const std::string key = "response.json_out";
    outputs_ = api::NamedIoBatch(2);
    for (auto& row : outputs_) row[key] = nullptr;
    ASSERT_EQ(ops_.Process(handle_, inputs, outputs_), 0)
        << api::GetOperatorLastError();
    ASSERT_EQ(outputs_.size(), 2u);
    for (size_t i = 0; i < expected.size(); ++i) {
      SCOPED_TRACE(i);
      ASSERT_EQ(outputs_[i].size(), 1u);
      const auto* response =
          static_cast<const CompanyOperatorOutput*>(outputs_[i].at(key).get());
      ASSERT_NE(response, nullptr);
      EXPECT_EQ(response->service_type, service);
      EXPECT_EQ(response->status_code, 0);
      ASSERT_NE(response->result_json, nullptr);
      ASSERT_NE(response->result_json->data, nullptr);
      ASSERT_GT(response->result_json->length, 0);
      EXPECT_EQ(Json::parse(std::string(response->result_json->data,
                                        response->result_json->length)),
                expected[i]);
    }
    outputs_.clear();
  }
  api::OperatorFunc ops_ = api::Get_LLM_EDGEFLOW_OperatorTable();
  void* handle_ = nullptr;
  api::NamedIoBatch outputs_;
  std::filesystem::path directory_;
  bool initialized_ = false;
};

TEST_F(BusinessResponsesTest,
       TranslateSelectsQueryFromCompleteJsonAndPairsTranslatedRows) {
  Create(Pipeline(
      "translate", {"entity_in"}, {{"translation", "generate.text"}},
      {Node("text_template", "prompt", {{"primary", "input.query"}},
            {{"template", "译:{{primary}}"}}),
       Generate("prompt.text")},
      {Model("llm", {{"responses",
                      {{"译:hello", "你好"}, {"译:goodbye", "再见"}}}})}));
  std::array<std::string, 2> raw{R"({"query":"hello","unused":"goodbye"})",
                                 R"({"query":"goodbye","unused":"hello"})"};
  std::array<CompanyString, 2> text{Borrow(raw[0]), Borrow(raw[1])};
  std::array<CompanyOperatorEntityInput, 2> requests{
      {{kMockServiceTranslate, &text[0]}, {kMockServiceTranslate, &text[1]}}};
  api::NamedIoBatch inputs(2);
  for (size_t i = 0; i < requests.size(); ++i)
    inputs[i]["request.entity_in"] =
        api::MakeBorrowedOperatorInput(&requests[i]);
  Check(inputs, kMockServiceTranslate,
        {Json{{"translated", "你好"}}, Json{{"translated", "再见"}}});
}

TEST_F(BusinessResponsesTest,
       EntityExtractionPublishesTheWholeStructuredDocumentForEachSentence) {
  Create(Pipeline(
      "entity_extract", {"entity_in"}, {{"entities", "parse.document"}},
      {Node("text_template", "prompt", {{"primary", "input.sentence_text"}},
            {{"template", "实体:{{primary}}"}}),
       Generate("prompt.text"),
       Node("structured_json_parse", "parse", {{"text", "generate.text"}})},
      {Model(
          "llm",
          {{"responses",
            {{"实体:张三在北京",
              R"({"entities":[{"name":"张三","type":"person"},{"name":"北京","type":"place"}]})"},
             {"实体:上海公司",
              R"({"entities":[{"name":"上海","type":"place"}],"source":"company"})"}}}})}));
  std::array<std::string, 2> raw{"张三在北京", "上海公司"};
  std::array<CompanyString, 2> text{Borrow(raw[0]), Borrow(raw[1])};
  std::array<CompanyOperatorEntityInput, 2> requests{
      {{kMockServiceEntityExtract, &text[0]},
       {kMockServiceEntityExtract, &text[1]}}};
  api::NamedIoBatch inputs(2);
  for (size_t i = 0; i < requests.size(); ++i)
    inputs[i]["request.entity_in"] =
        api::MakeBorrowedOperatorInput(&requests[i]);
  Check(
      inputs, kMockServiceEntityExtract,
      {Json::parse(
           R"({"entities":[{"name":"张三","type":"person"},{"name":"北京","type":"place"}]})"),
       Json::parse(
           R"({"entities":[{"name":"上海","type":"place"}],"source":"company"})")});
}

TEST_F(BusinessResponsesTest,
       DialogueAuditCombinesIndependentVerdictsAndBestPolicyPerRequest) {
  Create(Pipeline(
      "dialogue_audit", {"audit_in"},
      {{"verdict", "parse.document"}, {"matched_policy", "rerank.ranked"}},
      {Node("text_corpus_source", "policies", Json::object(),
            {{"corpus", {"退款制度", "密码制度"}}}),
       Node("text_embedding", "query_vectors", {{"text", "input.user_text"}},
            {{"bind_model", "embedding"}}),
       Node("text_embedding", "policy_vectors", {{"text", "policies.corpus"}},
            {{"bind_model", "embedding"}}),
       Node("vector_top_k", "retrieve",
            {{"queries", "query_vectors.embedding"},
             {"candidates", "policy_vectors.embedding"},
             {"candidate_texts", "policies.corpus"}},
            {{"top_k", 2}}),
       Node("text_rerank", "rerank",
            {{"queries", "input.user_text"}, {"candidates", "retrieve.ranked"}},
            {{"bind_model", "rerank"}}),
       Node("text_template", "prompt",
            {{"primary", "input.user_text"}, {"context", "rerank.ranked"}},
            {{"template", "审核:{{primary}}|{{context}}"}}),
       Generate("prompt.text"),
       Node("structured_json_parse", "parse", {{"text", "generate.text"}})},
      {Model("embedding"), Model("rerank"),
       Model(
           "llm",
           {{"responses",
             {{"审核:退款|退款制度",
               R"({"risk_level":"SAFE","risk_score":0.25,"verdict":"官方退款"})"},
              {"审核:密码|密码制度",
               R"({"risk_level":"HIGH_RISK","risk_score":0.75,"verdict":"禁止索取密码"})"}}}})}));
  std::array<std::string, 2> raw{"退款", "密码"};
  std::array<std::string, 2> channels{"web", "phone"};
  std::array<CompanyString, 2> text{Borrow(raw[0]), Borrow(raw[1])};
  std::array<CompanyString, 2> channel{Borrow(channels[0]),
                                       Borrow(channels[1])};
  std::array<CompanyOperatorAuditInput, 2> requests{
      {{kMockServiceDialogueAudit, &text[0], &channel[0]},
       {kMockServiceDialogueAudit, &text[1], &channel[1]}}};
  api::NamedIoBatch inputs(2);
  for (size_t i = 0; i < requests.size(); ++i)
    inputs[i]["request.audit_in"] =
        api::MakeBorrowedOperatorInput(&requests[i]);
  Check(
      inputs, kMockServiceDialogueAudit,
      {Json::parse(
           R"({"risk_level":"SAFE","risk_score":0.25,"matched_policy_clause":"退款制度","audit_verdict":{"risk_level":"SAFE","risk_score":0.25,"verdict":"官方退款"}})"),
       Json::parse(
           R"({"risk_level":"HIGH_RISK","risk_score":0.75,"matched_policy_clause":"密码制度","audit_verdict":{"risk_level":"HIGH_RISK","risk_score":0.75,"verdict":"禁止索取密码"}})")});
}

TEST_F(BusinessResponsesTest,
       DocumentQaKeepsChunkCountsIntentAndRetrievedAnswerWithOriginalRows) {
  Create(Pipeline(
      "doc_qa", {"doc_in"},
      {{"answer_text", "generate.text"},
       {"intent", "intent.matches"},
       {"chunk_count", "chunks.chunk_counts"}},
      {Node("text_chunk", "chunks", {{"text", "input.doc_text"}},
            {{"chunk_size", 4}, {"overlap", 0}}),
       Node("text_embedding", "query_vectors", {{"text", "input.query_text"}},
            {{"bind_model", "embedding"}}),
       Node("text_embedding", "chunk_vectors", {{"text", "chunks.chunks"}},
            {{"bind_model", "embedding"}}),
       Node("vector_top_k", "retrieve",
            {{"queries", "query_vectors.embedding"},
             {"candidates", "chunk_vectors.embedding"},
             {"candidate_texts", "chunks.chunks"}},
            {{"top_k", 1}}),
       Node("text_rule_match", "intent", {{"text", "input.query_text"}},
            {{"categories", {{"FIRST", {"AAAA"}}, {"SECOND", {"BBBB"}}}}}),
       Node("text_template", "prompt",
            {{"primary", "input.query_text"}, {"context", "retrieve.ranked"}},
            {{"template", "文档:{{primary}}|{{context}}"}}),
       Generate("prompt.text")},
      {Model("embedding"), Model("llm", {{"responses",
                                          {{"文档:AAAA|AAAA", "甲答案"},
                                           {"文档:BBBB|BBBB", "乙答案"}}}})}));
  std::array<std::string, 2> docs{"AAAA", "BBBBCCCC"};
  std::array<std::string, 2> queries{"AAAA", "BBBB"};
  std::array<CompanyString, 2> doc{Borrow(docs[0]), Borrow(docs[1])};
  std::array<CompanyString, 2> query{Borrow(queries[0]), Borrow(queries[1])};
  std::array<CompanyOperatorDocInput, 2> requests{
      {{kMockServiceDocQa, &doc[0], &query[0]},
       {kMockServiceDocQa, &doc[1], &query[1]}}};
  api::NamedIoBatch inputs(2);
  for (size_t i = 0; i < requests.size(); ++i)
    inputs[i]["request.doc_in"] = api::MakeBorrowedOperatorInput(&requests[i]);
  Check(inputs, kMockServiceDocQa,
        {Json{{"intent_name", "FIRST"},
              {"confidence", 1.0},
              {"answer_text", "甲答案"},
              {"chunk_count", 1}},
         Json{{"intent_name", "SECOND"},
              {"confidence", 1.0},
              {"answer_text", "乙答案"},
              {"chunk_count", 2}}});
}

TEST_F(BusinessResponsesTest,
       InvoiceQaAssociatesImageBoxCountsAndQuestionsAcrossBothInputSlots) {
  Create(Pipeline(
      "ocr_invoice_qa", {"frame", "string"},
      {{"result", "parse.document"}, {"document", "ocr.document"}},
      {Node("ocr_detect", "ocr", {{"images", "input.image"}},
            {{"bind_model", "ocr"}}),
       Node("text_template", "prompt",
            {{"primary", "input.question"}, {"document", "ocr.document"}},
            {{"template", "发票:{{primary}}|{{document}}"}}),
       Generate("prompt.text"),
       Node("structured_json_parse", "parse", {{"text", "generate.text"}})},
      {Model("ocr"),
       Model("llm", {{"responses",
                      {{"发票:问甲|编号11",
                        R"({"invoice_number":"11","amount":12.5})"},
                       {"发票:问乙|编号22",
                        R"({"invoice_number":"22","amount":25.0})"}}}})}));
  std::array<std::array<uint8_t, 3>, 2> pixels{{{11, 1, 2}, {22, 3, 4}}};
  std::array<CompanyFrame, 2> frames{
      {{kMockServiceOcrInvoiceQa, 1, 1, 3, pixels[0].data(), nullptr},
       {kMockServiceOcrInvoiceQa, 1, 1, 3, pixels[1].data(), nullptr}}};
  std::array<std::string, 2> questions{"问甲", "问乙"};
  std::array<CompanyString, 2> query{Borrow(questions[0]),
                                     Borrow(questions[1])};
  api::NamedIoBatch inputs(2);
  for (size_t i = 0; i < frames.size(); ++i) {
    inputs[i]["request.frame"] = api::MakeBorrowedOperatorInput(&frames[i]);
    inputs[i]["request.string"] = api::MakeBorrowedOperatorInput(&query[i]);
  }
  Check(
      inputs, kMockServiceOcrInvoiceQa,
      {Json::parse(
           R"({"detected_box_count":1,"result":{"invoice_number":"11","amount":12.5}})"),
       Json::parse(
           R"({"detected_box_count":2,"result":{"invoice_number":"22","amount":25.0}})")});
}

TEST_F(BusinessResponsesTest,
       AudioIntentCombinesDistinctTranscriptsKeywordAndNamedRegexSlots) {
  Create(Pipeline(
      "audio_asr_intent", {"audio_in"},
      {{"transcribed_text", "asr.text"}, {"intent_slot", "intent.matches"}},
      {Node("asr_transcribe", "asr", {{"audio", "input.audio"}},
            {{"bind_model", "asr"}}),
       Node("text_rule_match", "intent", {{"text", "asr.text"}},
            {{"categories", {{"MUSIC", {"音乐"}}}},
             {"rules",
              Json::array({{{"id", "destination"},
                            {"strategy", "regex"},
                            {"pattern", "导航到(?<place>.+)"},
                            {"category", "NAVIGATION"},
                            {"constants", {{"avoid_toll", false}}}}})}})},
      {Model("asr")}));
  std::array<std::vector<float>, 2> pcm{std::vector<float>(1600, 0.25f),
                                        std::vector<float>(1600, 0.5f)};
  std::array<CompanyOperatorAudioInput, 2> requests{
      {{kMockServiceAudioAsrIntent, pcm[0].data(), 1600, 16000},
       {kMockServiceAudioAsrIntent, pcm[1].data(), 1600, 16000}}};
  api::NamedIoBatch inputs(2);
  for (size_t i = 0; i < requests.size(); ++i)
    inputs[i]["request.audio_in"] =
        api::MakeBorrowedOperatorInput(&requests[i]);
  Check(
      inputs, kMockServiceAudioAsrIntent,
      {Json::parse(
           R"({"transcribed_text":"播放音乐","intent_slot":{"intent":"MUSIC","matched_word":"音乐","confidence":1.0,"slots":{},"matches":[{"category":"MUSIC","matched_word":"音乐"}]}})"),
       Json::parse(
           R"json({"transcribed_text":"导航到北京","intent_slot":{"intent":"NAVIGATION","matched_word":"导航到(?<place>.+)","confidence":1.0,"slots":{"place":"北京","avoid_toll":false},"matches":[{"category":"NAVIGATION","rule_id":"destination","pattern":"导航到(?<place>.+)","score":1.0}]}})json")});
}

TEST_F(BusinessResponsesTest,
       CrossRerankReturnsAllSortedScoresAndOriginalCandidateIndicesPerRow) {
  Create(Pipeline("cross_rerank", {"rerank_in"}, {{"ranked", "rerank.ranked"}},
                  {Node("text_rerank", "rerank",
                        {{"queries", "input.query_text"},
                         {"candidates", "input.candidates"}},
                        {{"bind_model", "rerank"}, {"top_k", 2}})},
                  {Model("rerank")}));
  std::array<std::string, 2> queries{"甲", "乙"};
  std::array<std::array<std::string, 2>, 2> candidates{
      {{"无关", "甲说明"}, {"乙说明", "无关"}}};
  std::array<CompanyString, 2> query{Borrow(queries[0]), Borrow(queries[1])};
  std::array<std::array<CompanyString, 2>, 2> passages{
      {{Borrow(candidates[0][0]), Borrow(candidates[0][1])},
       {Borrow(candidates[1][0]), Borrow(candidates[1][1])}}};
  std::array<CompanyOperatorRerankInput, 2> requests{};
  api::NamedIoBatch inputs(2);
  for (size_t i = 0; i < requests.size(); ++i) {
    requests[i].service_type = kMockServiceCrossRerank;
    requests[i].query_text = &query[i];
    requests[i].candidate_count = 2;
    requests[i].candidate_passages[0] = &passages[i][0];
    requests[i].candidate_passages[1] = &passages[i][1];
    inputs[i]["request.rerank_in"] =
        api::MakeBorrowedOperatorInput(&requests[i]);
  }
  Check(inputs, kMockServiceCrossRerank,
        {Json::parse(
             R"({"scores":[0.75,0.25],"sorted_indices":[1,0],"count":2})"),
         Json::parse(
             R"({"scores":[0.75,0.25],"sorted_indices":[0,1],"count":2})")});
}

}  // namespace
}  // namespace llm_edgeflow
