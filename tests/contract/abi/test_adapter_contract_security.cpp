#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/io_converter_registry.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/model_file_resolver.h"
#include "adapter/shared_algorithm_runtime.h"
#include "core/common_contracts.h"
#include "dev_support/inference/test_causal_lm_backend.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"
#include "platform_mock/error_codes.h"
#include "tests/support/adapter_examples/flat_struct_adapter.h"
#include "tests/support/adapter_examples/nested_array_adapter.h"
#include "tests/support/adapter_examples/nested_pointer_tree_adapter.h"
#include "tests/support/adapter_examples/tagged_union_adapter.h"
#include "tests/support/adapter_harness.h"
#include "tests/support/adapter_test_views.h"

namespace llm_edgeflow {
namespace {
constexpr auto kSentenceText = MakeBlackboardKey<TextBatch>("sentence_text");
constexpr auto kMatches = MakeBlackboardKey<RuleMatchBatch>("matches");
constexpr auto kTranslation = MakeBlackboardKey<TextBatch>("translation");
}  // namespace
}  // namespace llm_edgeflow

namespace llm_edgeflow {

static std::string GetConfigPath(const std::string& rel_path) {
  if (std::filesystem::exists(rel_path)) return rel_path;
  if (std::filesystem::exists("../" + rel_path)) return "../" + rel_path;
  return rel_path;
}

class AdapterContractSecurityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IoConverterRegistry::Instance().ResetConflictForTesting();
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Init();
  }
  void TearDown() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().DeInit();
    IoConverterRegistry::Instance().ResetConflictForTesting();
  }
};

namespace {

class ProbeSession final : public test::TestCausalLmSession {
 public:
  explicit ProbeSession(std::string backend)
      : TestCausalLmSession("probe.fixture"), backend_(std::move(backend)) {}
  const std::string& BackendType() const noexcept override { return backend_; }

 private:
  std::string backend_;
};

class ProbeBackend final : public IInferenceBackend {
 public:
  explicit ProbeBackend(std::string type) : type_(std::move(type)) {}
  const std::string& BackendType() const noexcept override { return type_; }
  std::shared_ptr<IBackendSession> Load(const BackendLoadSpec&,
                                        std::string* error) noexcept override {
    try {
      if (error) error->clear();
      return std::make_shared<ProbeSession>(type_);
    } catch (...) {
      return nullptr;
    }
  }

 private:
  std::string type_;
};

[[maybe_unused]] const bool probe_backends_registered = [] {
  bool registered = true;
  for (const char* type :
       {"translation_probe_backend", "failing_create_backend"}) {
    BackendDefinition definition;
    definition.backend_type = type;
    definition.supported_protocols = {ExecutionProtocol::kFixture};
    definition.concurrency = InferenceConcurrency::kSerialized;
    registered &= BackendRegistry::Instance().Register(
        definition, [type] { return std::make_unique<ProbeBackend>(type); });
  }
  return registered;
}();

// 测试通过 Operator API 执行随附的翻译 Pipeline，该夹具记录实际的生成调用，
// 不做任何 JSON 处理。
class TranslationProbeModel final : public ILlmModel {
 public:
  bool SupportsRandomSeed() const noexcept override { return true; }
  inline static constexpr char kImplName[] = "test_translation_probe";
  inline static std::weak_ptr<TranslationProbeModel> latest;

  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string* error) {
    auto model = std::make_shared<TranslationProbeModel>();
    latest = model;
    if (error) error->clear();
    return model;
  }
  const std::string& ImplName() const noexcept override {
    static const std::string type = kImplName;
    return type;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string model_type = "llm";
    return model_type;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }

  int Generate(const TextBatch& prompts, const GenerateOptions&,
               TextBatch* outputs,
               std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
    try {
      calls.push_back(prompts);
      if (failure != 0) return failure;
      if (!outputs) return -1;
      outputs->clear();
      for (const auto& prompt : prompts) {
        const auto found = responses.find(prompt.data);
        outputs->emplace_back(
            prompt.req_id, prompt.sub_id,
            found == responses.end() ? response : found->second);
      }
      return 0;
    } catch (...) {
      return -1;
    }
  }

  std::vector<TextBatch> calls;
  std::map<std::string, std::string> responses;
  std::string response;
  int failure = 0;
};

// 在模型创建时失败，使 Operator Create 进入加载阶段。
class FailingCreateModel {
 public:
  inline static constexpr char kImplName[] = "test_failing_create_model";
  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string* error) {
    if (error) *error = "probe model refused to load";
    return nullptr;
  }
};

REGISTER_MODEL_WITH_DEFINITION(FailingCreateModel, [] {
  ModelDefinition definition;
  definition.impl_name = FailingCreateModel::kImplName;
  definition.model_type = "llm";
  definition.description = "Test-only model whose creation fails";
  definition.required_protocol = ExecutionProtocol::kFixture;
  definition.fixture_backends = {"failing_create_backend"};
  definition.concurrency = InferenceConcurrency::kSerialized;
  return definition;
}());

REGISTER_MODEL_WITH_DEFINITION(TranslationProbeModel, [] {
  ModelDefinition definition;
  definition.impl_name = TranslationProbeModel::kImplName;
  definition.model_type = "llm";
  definition.description = "Test-only translation generation probe";
  definition.required_protocol = ExecutionProtocol::kFixture;
  definition.fixture_backends = {"translation_probe_backend"};
  definition.concurrency = InferenceConcurrency::kSerialized;
  return definition;
}());

}  // namespace

TEST_F(AdapterContractSecurityTest,
       TranslationOperatorGeneratesOnceFromRawQueryAndPacksLiteralOutput) {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("edgeflow-translate-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
  } cleanup{directory};
  const auto pipe_path = (directory / "pipeline.json").string();
  std::ifstream source(GetConfigPath("configs/pipeline_translate_cpu.json"));
  ASSERT_TRUE(source.is_open());
  nlohmann::json pipeline;
  source >> pipeline;
  // 保留生产计算图和端口绑定，只替换模型执行，
  // 使这些断言既不需要模型资源也不需要 Demo。
  ASSERT_EQ(pipeline.at("pipeline").size(), 1U);
  EXPECT_EQ(pipeline["pipeline"][0]["type"], "llm_generate");
  ASSERT_EQ(pipeline.at("models").size(), 1U);
  auto& model_params = pipeline["models"][0];
  model_params["type"] = "llm";
  model_params["backend"] = {{"type", "translation_probe_backend"}};
  model_params["file"] = "translation-probe.fixture";
  model_params["params"] = nlohmann::json::object();
  pipeline["io"]["output"][0]["params"] = {{"entities_json_max_bytes", 2047}};
  std::ofstream(pipe_path) << pipeline.dump();

  nlohmann::json op_cfg = {{"pipe_path", "pipeline.json"}};
  const auto config = (directory / "pipeline.conf").string();
  std::ofstream(config) << op_cfg.dump();

  operator_api::CreateParam create{};
  create.model_path = directory.c_str();
  create.cfg_file_name = "pipeline.conf";
  create.compute_platform = operator_api::ComputePlatform::kCpu;
  void* raw_handle = nullptr;
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(op.Create(&raw_handle, &create), 0);
  std::unique_ptr<void, int (*)(void*)> handle(raw_handle, op.Destroy);
  const auto model = TranslationProbeModel::latest.lock();
  ASSERT_NE(model, nullptr);

  uint64_t out_req_id = 0;
  int out_status_code = 0;
  std::string out_entities_json;

  auto process = [&](const char* payload) {
    if (!payload) {
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }
    std::string s(payload);
    CompanyString cs{static_cast<int32_t>(s.size()),
                     const_cast<char*>(s.data())};
    CompanyOperatorEntityInput input{};
    input.request_id = 987654321;
    input.service_type = kMockServiceTranslate;
    input.sentence_text = &cs;
    operator_api::NamedIoBatch inputs(1);
    inputs[0]["trans.entity_in"] =
        operator_api::MakeBorrowedOperatorInput(&input);
    operator_api::NamedIoBatch outputs(1);
    outputs[0]["trans.entity_out"] = nullptr;

    int ret = op.Process(handle.get(), inputs, outputs);
    if (ret == 0) {
      auto out_sp = outputs[0]["trans.entity_out"];
      if (out_sp) {
        auto* out_dto = static_cast<CompanyOperatorEntityOutput*>(out_sp.get());
        out_req_id = out_dto->request_id;
        out_status_code = out_dto->status_code;
        if (out_dto->entities_json && out_dto->entities_json->data) {
          out_entities_json = std::string(out_dto->entities_json->data,
                                          out_dto->entities_json->length);
        }
      }
      out_sp.reset();
    }
    outputs.clear();
    return ret;
  };
  const std::vector<std::string> queries = {"hello,what is your name", "",
                                            "  #\n\"hi\"\\中文  ",
                                            std::string("before\0after", 12)};
  const std::vector<std::string> translations = {
      "你好，你的名字是什么", "", "  #\n\"你好\"\\中文  ",
      std::string("left\0right", 10)};
  for (size_t i = 0; i < queries.size(); ++i) {
    for (bool extras : {false, true}) {
      nlohmann::json request = {{"query", queries[i]}};
      if (extras) {
        request["version"] = nullptr;
        request["endpoint"] = "not-translate";
        request["src_lan"] = {"not", "a", "language"};
      }
      const auto payload = request.dump(2);
      model->response = translations[i];
      const auto before = model->calls.size();
      ASSERT_EQ(process(payload.c_str()), 0) << payload;
      ASSERT_EQ(model->calls.size(), before + 1);
      ASSERT_EQ(model->calls.back().size(), 1U);
      EXPECT_EQ(model->calls.back()[0].data, queries[i]);
      EXPECT_EQ(model->calls.back()[0].req_id, 0U);
      EXPECT_EQ(model->calls.back()[0].sub_id, 0U);
      EXPECT_EQ(out_req_id, 987654321U);
      EXPECT_EQ(out_status_code, 0);
      EXPECT_EQ(nlohmann::json::parse(out_entities_json),
                nlohmann::json({{"translated", translations[i]}}));
    }
  }
  // 看起来像 JSON 或 Markdown 的文本仍是原始模型结果：
  // 不做 JSON 解析、字段抽取、裁剪或重试。
  for (const std::string text :
       {R"({"translated":"literal","extra":42})",
        "```json\n{\"translated\":\"literal\"}\n```"}) {
    model->response = text;
    const auto before = model->calls.size();
    ASSERT_EQ(process("{\"query\":\"hello\"}"), 0);
    EXPECT_EQ(model->calls.size(), before + 1);
    EXPECT_EQ(nlohmann::json::parse(out_entities_json),
              nlohmann::json({{"translated", text}}));
  }
  const auto before_invalid = model->calls.size();
  for (const char* invalid :
       std::vector<const char*>{nullptr, "invalid JSON", "[]", "{}",
                                "{\"query\":null}", "{\"query\":123}"}) {
    EXPECT_EQ(process(invalid), COMPANY_ALG_ERR_INVALID_INPUT);
  }
  const std::string too_long(64 * 1024 + 1, 'x');
  EXPECT_EQ(process(too_long.c_str()), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(model->calls.size(), before_invalid);

  model->response.assign(2200, 'x');
  EXPECT_EQ(process("{\"query\":\"hello\"}"), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(model->calls.size(), before_invalid + 1);
  // Model 错误码保留在诊断中；宿主看到的是执行阶段类别。
  model->failure = -731;
  EXPECT_EQ(process("{\"query\":\"hello\"}"), COMPANY_ALG_ERR_UNKNOWN);
  const std::string model_error = operator_api::GetOperatorLastError();
  EXPECT_NE(model_error.find("Pipeline execution failed with internal code "
                             "-731: Node '"),
            std::string::npos)
      << model_error;
  EXPECT_EQ(model->calls.size(), before_invalid + 2);
  model->failure = 0;
  model->response = "你好";
  EXPECT_EQ(process("{\"query\":\"hello\"}"), 0);
  EXPECT_EQ(model->calls.size(), before_invalid + 3);
  EXPECT_EQ(nlohmann::json::parse(out_entities_json),
            nlohmann::json({{"translated", "你好"}}));

  // 模型响应中的非法 UTF-8 会使 JSON dump 抛异常，经公开的 Operator Process
  // 屏障映射为 COMPANY_ALG_ERR_EXCEPTION (-99)
  model->response = "prefix\xFF\xFFsuffix";
  EXPECT_EQ(process("{\"query\":\"hello\"}"), COMPANY_ALG_ERR_EXCEPTION);
  model->response = "你好";
}

TEST_F(AdapterContractSecurityTest,
       MultipleEndpointsReachEntityResponseAsRawDocumentFields) {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("edgeflow-endpoints-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
  } cleanup{directory};
  const nlohmann::json pipeline = {
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "entity_extract"}}}},
        {"output",
         {{{"type", "entity_out"},
           {"name", "entity_extract"},
           {"inputs", {{"entities", "generate.document"}}}}}}}},
      {"models",
       {{{"type", "llm"},
         {"name", "probe"},
         {"file", "probe.fixture"},
         {"backend", {{"type", "translation_probe_backend"}}}}}},
      {"pipeline",
       {{{"type", "llm_generate"},
         {"name", "generate"},
         {"params",
          {{"bind_model", "probe"},
           {"endpoints",
            {{"entities", {{"prompt", "entities={{input}}"}}},
             {"summary", {{"prompt", "summary={{input}}"}}}}}}},
         {"inputs", {{"input", "input.sentence_text"}}}}}}};
  std::ofstream(directory / "pipeline.json") << pipeline;
  std::ofstream(directory / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  const auto root = directory.string();
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  operator_api::CreateParam create{};
  create.model_path = root.c_str();
  create.cfg_file_name = "pipeline.conf";
  create.max_frame_depth = 2;
  create.compute_platform = operator_api::ComputePlatform::kCpu;
  void* raw_handle = nullptr;
  ASSERT_EQ(op.Create(&raw_handle, &create), 0)
      << operator_api::GetOperatorLastError();
  std::unique_ptr<void, int (*)(void*)> handle(raw_handle, op.Destroy);
  const auto model = TranslationProbeModel::latest.lock();
  ASSERT_NE(model, nullptr);
  std::vector<std::string> requests{"张三在北京", "hello\n\"world\""};
  const std::vector<uint64_t> request_ids{987654321, 17};
  const std::vector<std::string> entities{"[\"PERSON:张三\"]",
                                          std::string("left\0right", 10)};
  const std::vector<std::string> summaries{"```json\n{\"raw\":true}\n```",
                                           "plain summary"};
  std::vector<CompanyString> sentences;
  std::vector<CompanyOperatorEntityInput> carriers;
  sentences.reserve(requests.size());
  carriers.reserve(requests.size());
  operator_api::NamedIoBatch inputs(requests.size()), outputs(requests.size());
  for (size_t i = 0; i < requests.size(); ++i) {
    model->responses["entities=" + requests[i]] = entities[i];
    model->responses["summary=" + requests[i]] = summaries[i];
    sentences.push_back(
        {static_cast<int32_t>(requests[i].size()), requests[i].data()});
    carriers.push_back(
        {request_ids[i], kMockServiceEntityExtract, &sentences.back()});
    inputs[i]["test.entity_in"] =
        operator_api::MakeBorrowedOperatorInput(&carriers.back());
    outputs[i]["test.entity_out"] = nullptr;
  }
  ASSERT_EQ(op.Process(handle.get(), inputs, outputs), 0)
      << operator_api::GetOperatorLastError();
  ASSERT_EQ(model->calls.size(), 2u);
  for (size_t endpoint = 0; endpoint < model->calls.size(); ++endpoint) {
    ASSERT_EQ(model->calls[endpoint].size(), requests.size());
    const std::string prefix = endpoint == 0 ? "entities=" : "summary=";
    for (size_t i = 0; i < requests.size(); ++i) {
      EXPECT_EQ(model->calls[endpoint][i].data, prefix + requests[i]);
      EXPECT_EQ(model->calls[endpoint][i].req_id, i);
      EXPECT_EQ(model->calls[endpoint][i].sub_id, 0u);
    }
  }
  ASSERT_EQ(outputs.size(), requests.size());
  for (size_t i = 0; i < outputs.size(); ++i) {
    const auto* result = static_cast<CompanyOperatorEntityOutput*>(
        outputs[i].at("test.entity_out").get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->request_id, request_ids[i]);
    EXPECT_EQ(result->service_type, kMockServiceEntityExtract);
    EXPECT_EQ(result->status_code, 0);
    ASSERT_NE(result->entities_json, nullptr);
    const auto response = nlohmann::json::parse(std::string(
        result->entities_json->data, result->entities_json->length));
    EXPECT_EQ(response, nlohmann::json({{"entities", entities[i]},
                                        {"summary", summaries[i]}}));
    EXPECT_TRUE(response["entities"].is_string());
    EXPECT_TRUE(response["summary"].is_string());
  }
}

TEST_F(AdapterContractSecurityTest,
       TranslationLiteralResultPackingAndCarrierSafety) {
  const auto* converter = IoConverterRegistry::Instance().FindOutputConverter(
      "entity_out", "translate");
  ASSERT_NE(converter, nullptr);
  const std::string translation(2200, 'x');
  AlgContext large;
  std::vector<uint64_t> request_ids{123};
  large.Publish(kTranslation, TextBatch{{0, 0, translation}});
  OutputEncodeOptions options;
  const llm_edgeflow::IoPortBindings options_ports = {
      {"translation", "translation"}};
  options.ports = &options_ports;
  options.type = "entity_out";
  options.name = "translate";
  options.request_ids = &request_ids;

  AdapterStatus status;

  char buf_large[2500] = {0};
  CompanyString cs_large{0, buf_large};
  CompanyOperatorEntityOutput fixed{};
  fixed.entities_json = &cs_large;

  TestOutputBatchView fixed_view;
  fixed_view.count = 1;
  fixed_view.leased_slots["entity_out"] = {&fixed};
  fixed_view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";
  fixed_view.SetCapacity("entity_out", "entities_json", 2047);
  size_t written = 0;

  EXPECT_EQ(
      converter->encode_fn(&large, options, &fixed_view, &written, &status),
      COMPANY_ALG_ERR_BUFFER_TOO_SMALL);

  // 重排后的内部结果必须映射回外部请求 ID。
  AlgContext reordered;
  request_ids = {999, 123};
  reordered.Publish(kTranslation,
                    TextBatch{{1, 0, "第二句"}, {0, 0, "第一句"}});
  char buf_first[512] = {0};
  char buf_second[512] = {0};
  CompanyString cs_first{0, buf_first};
  CompanyString cs_second{0, buf_second};
  CompanyOperatorEntityOutput first{}, second{};
  first.entities_json = &cs_first;
  second.entities_json = &cs_second;

  TestOutputBatchView reordered_view;
  reordered_view.count = 2;
  reordered_view.leased_slots["entity_out"] = {&first, &second};
  reordered_view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";
  reordered_view.SetCapacity("entity_out", "entities_json", 511);

  ASSERT_EQ(converter->encode_fn(&reordered, options, &reordered_view, &written,
                                 &status),
            0);
  EXPECT_EQ(written, 2U);
  EXPECT_EQ(first.request_id, 999U);
  EXPECT_EQ(second.request_id, 123U);
  EXPECT_EQ(nlohmann::json::parse(first.entities_json->data),
            nlohmann::json({{"translated", "第一句"}}));
  EXPECT_EQ(nlohmann::json::parse(second.entities_json->data),
            nlohmann::json({{"translated", "第二句"}}));

  reordered_view.count = 1;
  EXPECT_EQ(converter->encode_fn(&reordered, options, &reordered_view, &written,
                                 &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);

  for (const TextBatch& invalid :
       std::vector<TextBatch>{{},
                              {{2, 0, "out-of-range"}},
                              {{0, 1, "invalid-sub-id"}},
                              {{0, 0, "duplicate"}, {0, 0, "duplicate"}},
                              {{0, 0, "missing-second"}}}) {
    AlgContext ctx;
    ctx.Publish(kTranslation, invalid);
    reordered_view.count = 2;
    EXPECT_EQ(
        converter->encode_fn(&ctx, options, &reordered_view, &written, &status),
        COMPANY_ALG_ERR_INVALID_INPUT);
  }
  for (bool publish_ids : {false, true}) {
    AlgContext missing;
    if (publish_ids) {
      options.request_ids = &request_ids;
    } else {
      options.request_ids = nullptr;
      missing.Publish(kTranslation, TextBatch{{0, 0, "你好"}});
    }
    reordered_view.count = 1;
    EXPECT_EQ(converter->encode_fn(&missing, options, &reordered_view, &written,
                                   &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
  }
}

TEST_F(AdapterContractSecurityTest, PipelineFileDirectoryIsSandboxed) {
  const std::string root_input = GetConfigPath("configs");
  const auto root = std::filesystem::absolute(root_input);
  ASSERT_TRUE(std::filesystem::is_directory(root));

  nlohmann::json pipeline_json = {
      {"models",
       {{{"type", "embedding"},
         {"name", "model"},
         {"file", "artifact.onnx"},
         {"backend", {{"type", "test_tensor_backend"}}}}}}};
  nlohmann::json resolved;
  std::string diagnostic;
  ASSERT_TRUE(
      ResolveModelFiles(pipeline_json, root_input, &resolved, &diagnostic))
      << diagnostic;
  EXPECT_EQ(
      std::filesystem::path(resolved["models"][0]["file"].get<std::string>()),
      std::filesystem::weakly_canonical(root / "artifact.onnx"));
  pipeline_json["models"][0]["file"] = "..name/artifact.onnx";
  ASSERT_TRUE(
      ResolveModelFiles(pipeline_json, root_input, &resolved, &diagnostic))
      << diagnostic;
  ASSERT_TRUE(ResolveModelFiles(pipeline_json, "", &resolved, &diagnostic));
  EXPECT_EQ(resolved["models"][0]["file"], "..name/artifact.onnx");
  for (const std::string& invalid :
       {std::string("../escape.onnx"), std::string("nested/../artifact.onnx"),
        (root / "absolute.onnx").string(), std::string("C:\\model.onnx"),
        std::string("\\\\server\\model.onnx")}) {
    pipeline_json["models"][0]["file"] = invalid;
    EXPECT_FALSE(
        ResolveModelFiles(pipeline_json, root_input, &resolved, &diagnostic))
        << invalid;
    EXPECT_TRUE(resolved.is_null());
  }
}

TEST_F(AdapterContractSecurityTest,
       InMemoryEntryResolvesPipelineFilesBeforeCore) {
  const std::string config_path =
      GetConfigPath("demo/fixtures/mock/pipeline_doc_qa.json");
  std::ifstream config_stream(config_path);
  ASSERT_TRUE(config_stream.is_open());
  nlohmann::json pipeline_json;
  config_stream >> pipeline_json;
  ASSERT_EQ(pipeline_json["models"].size(), 2u);
  pipeline_json["models"][0]["file"] = "embedding.fixture";
  pipeline_json["models"][1]["file"] = "llm.fixture";

  const std::filesystem::path model_root =
      std::filesystem::weakly_canonical(GetConfigPath("configs"));
  std::unique_ptr<ValidatedIoPlan> io_plan;
  std::string plan_err;
  ASSERT_EQ(IoPlanResolver::ResolveFromPipelineJson(
                pipeline_json, model_root.string(), &io_plan, &plan_err),
            0)
      << plan_err;
  ASSERT_NE(io_plan, nullptr);

  std::unique_ptr<SharedAlgorithmRuntime> runtime;
  std::string runtime_err;
  RuntimeOptions runtime_opts{};
  runtime_opts.device_id = 0;
  ASSERT_EQ(SharedAlgorithmRuntime::CreateFromIoPlan(
                std::move(io_plan), 0, &runtime_opts, &runtime, &runtime_err),
            0)
      << runtime_err;
  ASSERT_NE(runtime, nullptr);

  const auto embedding_registration = runtime->GetPipeline()
                                          ->GetSessionContext()
                                          .GetModelManager()
                                          .GetModelRegistration("embed_model");
  const auto llm_registration = runtime->GetPipeline()
                                    ->GetSessionContext()
                                    .GetModelManager()
                                    .GetModelRegistration("llm_model");
  ASSERT_TRUE(embedding_registration.has_value());
  ASSERT_TRUE(llm_registration.has_value());
  EXPECT_EQ(embedding_registration->model_file,
            std::filesystem::weakly_canonical(model_root / "embedding.fixture")
                .string());
  EXPECT_EQ(
      llm_registration->model_file,
      std::filesystem::weakly_canonical(model_root / "llm.fixture").string());
}

// ---------------------------------------------------------------------------
// 1. Tagged Union & 模板适配器真实运行与非法枚举拦截 (ADP-001, ADP-010,
// RECHECK-005)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, TaggedUnionAndEnumValidation) {
  using namespace template_examples;
  TemplateTaggedUnionAdapter adapter;

  // 1.1 Helper 级校验
  AdapterStatus status;
  std::vector<int> valid_enums = {1, 2};
  EXPECT_TRUE(AdapterValidationHelper::RequireEnum(
      "inputs[0].payload_type", 1, valid_enums, 0, "MultiModalBiz", &status));
  EXPECT_TRUE(status.IsOk());

  EXPECT_FALSE(AdapterValidationHelper::RequireEnum(
      "inputs[0].payload_type", 99, valid_enums, 0, "MultiModalBiz", &status));
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.SampleIndex(), 0);
  EXPECT_EQ(status.FieldPath(), "inputs[0].payload_type");

  // 1.2 Tagged Union 适配器真实 Unpack 路径测试 (文本分支)
  TemplateTaggedUnionInput text_in;
  text_in.request_id = 1001;
  text_in.payload_type = TEMPLATE_PAYLOAD_TEXT;
  text_in.data.text.text_content = "Hello Tagged Union";

  const void* inputs[1] = {&text_in};
  AlgContext ctx;
  AdapterStatus unpack_status;
  int ret = adapter.Unpack(inputs, 1, &ctx, &unpack_status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  auto* items =
      ctx.Read<std::vector<TemplateUnionItemDto>>("tagged_union_items");
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->size(), 1U);
  EXPECT_EQ((*items)[0].text_content, "Hello Tagged Union");

  // 1.3 非法枚举分支直接在真实 Unpack 中被拦截
  TemplateTaggedUnionInput invalid_in;
  invalid_in.request_id = 1002;
  invalid_in.payload_type = 999;  // 非法枚举
  const void* bad_inputs[1] = {&invalid_in};
  AlgContext bad_ctx;
  AdapterStatus bad_status;
  int bad_ret = adapter.Unpack(bad_inputs, 1, &bad_ctx, &bad_status);
  EXPECT_EQ(bad_ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_FALSE(bad_status.IsOk());
  EXPECT_EQ(bad_status.SampleIndex(), 0);
  EXPECT_EQ(bad_status.FieldPath(), "inputs[i].payload_type");
}

// ---------------------------------------------------------------------------
// 2. 嵌套变长数组与乘法溢出/超限测试 (ADP-001, ADP-010, RECHECK-005)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, NestedArrayAndIntegerOverflowProtection) {
  using namespace template_examples;
  TemplateNestedArrayAdapter adapter;

  // 2.1 Helper 级乘法溢出校验
  AdapterStatus status;
  struct DummyBox {
    float x, y, w, h;
  };
  EXPECT_TRUE(AdapterValidationHelper::CheckedMultiply(
      "inputs[0].boxes", 100, sizeof(DummyBox), 1024 * 1024, 0, "DetectionBiz",
      &status));

  size_t overflow_count = (SIZE_MAX / sizeof(DummyBox)) + 1;
  EXPECT_FALSE(AdapterValidationHelper::CheckedMultiply(
      "inputs[0].boxes", overflow_count, sizeof(DummyBox), 1024 * 1024, 0,
      "DetectionBiz", &status));
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);

  // 2.2 嵌套数组适配器真实 Unpack 路径
  TemplateTagItem tags[2] = {{"tag_a", 0.9f}, {"tag_b", 0.5f}};
  TemplateNestedArrayInput array_in;
  array_in.request_id = 2001;
  array_in.tag_count = 2;
  array_in.tag_array = tags;

  const void* inputs[1] = {&array_in};
  AlgContext ctx;
  AdapterStatus unpack_status;
  int ret = adapter.Unpack(inputs, 1, &ctx, &unpack_status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  auto* array_items =
      ctx.Read<std::vector<TemplateNestedArrayItemDto>>("nested_array_items");
  ASSERT_NE(array_items, nullptr);
  ASSERT_EQ(array_items->size(), 1);
  EXPECT_EQ((*array_items)[0].tags.size(), 2);
  EXPECT_EQ((*array_items)[0].tags[0].tag_name, "tag_a");

  // 2.3 异常 count (<0 或 count > max) 拦截
  TemplateNestedArrayInput bad_array_in;
  bad_array_in.request_id = 2002;
  bad_array_in.tag_count = -5;  // 负数
  bad_array_in.tag_array = nullptr;
  const void* bad_inputs[1] = {&bad_array_in};
  AlgContext bad_ctx;
  AdapterStatus bad_status;
  int bad_ret = adapter.Unpack(bad_inputs, 1, &bad_ctx, &bad_status);
  EXPECT_EQ(bad_ret, COMPANY_ALG_ERR_INVALID_INPUT);
}

// ---------------------------------------------------------------------------
// 3. 多级嵌套指针树与最大深度递归栈保护测试 (ADP-001, ADP-010, RECHECK-005)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, NestedPointerTreeDepthProtection) {
  using namespace template_examples;
  TemplateNestedPointerTreeAdapter adapter;

  // 3.1 正常二层树
  TemplateTreeNode child1{101, "child_node_1", 0, nullptr};
  TemplateTreeNode child2{102, "child_node_2", 0, nullptr};
  const TemplateTreeNode* root_children[2] = {&child1, &child2};
  TemplateTreeNode root{100, "root_node", 2, root_children};

  TemplateNestedTreeInput tree_in{3001, &root};
  const void* inputs[1] = {&tree_in};
  AlgContext ctx;
  AdapterStatus status;
  std::vector<uint64_t> request_ids;
  InputDecodeOptions options;
  const llm_edgeflow::IoPortBindings options_ports = {
      {"tree_root_dtos", "tree_root_dtos"}};
  options.ports = &options_ports;
  options.type = adapter.AdapterName();
  options.name = "common";
  options.request_ids = &request_ids;
  int ret = adapter.Unpack(inputs, 1, &ctx, options, &status);
  EXPECT_EQ(ret, COMPANY_ALG_SUCCESS);
  auto* tree_dtos =
      ctx.Read<std::vector<TemplateTreeNodeDto>>("tree_root_dtos");
  ASSERT_NE(tree_dtos, nullptr);
  ASSERT_EQ(tree_dtos->size(), 1);
  EXPECT_EQ((*tree_dtos)[0].children.size(), 2);

  // 3.2 空子节点指针拦截
  const TemplateTreeNode* bad_children[2] = {&child1, nullptr};
  TemplateTreeNode bad_root{100, "root_node", 2, bad_children};
  TemplateNestedTreeInput bad_tree_in{3002, &bad_root};
  const void* bad_inputs[1] = {&bad_tree_in};
  AlgContext bad_ctx;
  AdapterStatus bad_status;
  int bad_ret = adapter.Unpack(bad_inputs, 1, &bad_ctx, options, &bad_status);
  EXPECT_EQ(bad_ret, COMPANY_ALG_ERR_INVALID_INPUT);
}

// ---------------------------------------------------------------------------
// 4. COPY_IN 内存所有权深度隔离测试 (ADP-002, RECHECK-006)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, DirectUnpackMemoryIsolation) {
  const auto* input_conv = IoConverterRegistry::Instance().FindInputConverter(
      "keyword_in", "keyword_match");
  ASSERT_NE(input_conv, nullptr);

  // 创建动态可修改的原始缓冲区
  char caller_buf[256];
  snprintf(caller_buf, sizeof(caller_buf), "设备系统初始化自检正常");

  CompanyString cs{static_cast<int32_t>(std::strlen(caller_buf)), caller_buf};
  CompanyOperatorKeywordInput in_struct;
  in_struct.request_id = 9999;
  in_struct.service_type = kMockServiceKeywordMatch;
  in_struct.sentence_text = &cs;

  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["keyword_in"] = llm_edgeflow::BorrowInputForTest({&in_struct});
  in_view.slot_types["keyword_in"] = "CompanyOperatorKeywordInput";
  InputDecodeOptions in_options;
  const llm_edgeflow::IoPortBindings in_options_ports = {
      {"sentence_text", "sentence_text"}};
  in_options.ports = &in_options_ports;
  in_options.type = "keyword_in";
  in_options.name = "keyword_match";
  std::vector<uint64_t> request_ids;
  in_options.request_ids = &request_ids;

  AlgContext ctx;
  AdapterStatus status;
  int unpack_ret = input_conv->decode_fn(in_view, in_options, &ctx, &status);
  ASSERT_EQ(unpack_ret, COMPANY_ALG_SUCCESS);

  // 立即篡改调用方内存 Buffer (例如 memset 覆盖为 'X')
  std::memset(caller_buf, 'X', sizeof(caller_buf) - 1);
  caller_buf[sizeof(caller_buf) - 1] = '\0';

  // 验证 AlgContext 中的 DTO 保持原有数据完全不受外界内存修改影响 (物理深拷贝)
  auto* sentences = ctx.Read(kSentenceText);
  ASSERT_NE(sentences, nullptr);
  ASSERT_EQ(sentences->size(), 1U);
  EXPECT_EQ((*sentences)[0].data, "设备系统初始化自检正常");
  EXPECT_NE((*sentences)[0].data, std::string(caller_buf));
}

// ---------------------------------------------------------------------------
// 5. 输出字符串截断拒绝测试 (RECHECK-001)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, OutputStringTruncationRejection) {
  using namespace template_examples;
  TemplateFlatStructAdapter adapter;

  AlgContext ctx;
  std::vector<TemplateFlatResultDto> results;
  // 构造长度超过 512 字节的超长 JSON 结果
  std::string huge_json(1024, 'A');
  results.push_back({5001, 0, huge_json});
  ctx.Publish("flat_final_outputs", results);

  TemplateFlatOutput out_slot;
  void* outputs[1] = {&out_slot};
  int num_outputs = 1;
  AdapterStatus status;

  // 预期必须返回 COMPANY_ALG_ERR_BUFFER_TOO_SMALL (-4)，拒绝静默假装成功
  int pack_ret = adapter.Pack(&ctx, outputs, &num_outputs, &status);
  EXPECT_EQ(pack_ret, COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_FALSE(status.IsOk());
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
}

// ---------------------------------------------------------------------------
// 8. 结构化诊断工具与有界字符串扫描测试 (RECHECK-004)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, StructuredStatusAndBoundedStringScan) {
  AdapterStatus status;

  // 正常字符串
  EXPECT_TRUE(AdapterValidationHelper::RequireBoundedString(
      "inputs[0].text", "normal text", 100, 0, "TestBiz", &status));
  EXPECT_TRUE(status.IsOk());

  // 超长字符串拦截
  EXPECT_FALSE(AdapterValidationHelper::RequireBoundedString(
      "inputs[0].text", "a very very long text exceeding limit", 10, 0,
      "TestBiz", &status));
  EXPECT_FALSE(status.IsOk());
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "inputs[0].text");

  // 验证诊断字符串包含丰富定位元数据
  std::string diag = status.ToString();
  EXPECT_NE(diag.find("TestBiz"), std::string::npos);
  EXPECT_NE(diag.find("inputs[0].text"), std::string::npos);
  EXPECT_NE(diag.find("sample [0]"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 9. 多线程共享 Adapter 无状态并发安全性测试 (ADP-003, RECHECK-006)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, ConcurrentStatelessAdapterExecution) {
  operator_api::CreateParam param{};
  param.model_path = ".";
  param.cfg_file_name = "configs/pipeline_keyword_match_rules.conf";
  param.device_id = 0;
  param.compute_platform = operator_api::ComputePlatform::kCpu;
  param.max_frame_depth = 25;

  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();

  constexpr int kNumThreads = 8;
  constexpr int kNumIters = 10;
  std::vector<std::thread> workers;

  for (int t = 0; t < kNumThreads; ++t) {
    workers.emplace_back([&, t]() {
      void* hndl = nullptr;
      int create_ret = op.Create(&hndl, &param);
      ASSERT_EQ(create_ret, 0);
      ASSERT_NE(hndl, nullptr);

      for (int it = 0; it < kNumIters; ++it) {
        std::string query = "系统初始化与设备自检请求 #" + std::to_string(t);
        CompanyString cs{static_cast<int32_t>(query.size()),
                         const_cast<char*>(query.data())};
        CompanyOperatorKeywordInput in_req{static_cast<uint64_t>(t * 1000 + it),
                                           kMockServiceKeywordMatch, &cs};

        operator_api::NamedIoBatch inputs(1);
        inputs[0]["client_channel.keyword_in"] =
            operator_api::MakeBorrowedOperatorInput(&in_req);

        operator_api::NamedIoBatch outputs(1);
        outputs[0]["client_channel.keyword_out"] = nullptr;

        int proc_ret = op.Process(hndl, inputs, outputs);
        EXPECT_EQ(proc_ret, 0);
        ASSERT_EQ(outputs.size(), 1u);
        auto out_sp = outputs[0]["client_channel.keyword_out"];
        ASSERT_NE(out_sp, nullptr);
        auto* out_res =
            static_cast<CompanyOperatorKeywordOutput*>(out_sp.get());
        ASSERT_NE(out_res, nullptr);
        EXPECT_EQ(out_res->request_id, in_req.request_id);
        EXPECT_EQ(out_res->is_hit, 1);
        out_sp.reset();
        outputs.clear();
      }

      op.Destroy(hndl);
    });
  }

  for (auto& w : workers) {
    if (w.joinable()) w.join();
  }
}

// 跨样本的载体错误与 biz 解码错误的优先级
TEST_F(AdapterContractSecurityTest,
       TranslationCrossSampleCarrierVsBizErrorPriority) {
  const auto* converter = IoConverterRegistry::Instance().FindInputConverter(
      "entity_in", "translate");
  ASSERT_NE(converter, nullptr);

  // 样本 0 有载体错误 (字符串超长)
  std::string oversized(64 * 1024 + 1, 'z');
  CompanyString cs_oversized{static_cast<int32_t>(oversized.size()),
                             const_cast<char*>(oversized.data())};
  CompanyOperatorEntityInput in_carrier{101, kMockServiceTranslate,
                                        &cs_oversized};

  ExternalInputBatchView carrier_view;
  carrier_view.count = 1;
  carrier_view.slots["entity_in"] =
      llm_edgeflow::BorrowInputForTest({&in_carrier});
  carrier_view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

  InputDecodeOptions options;
  const llm_edgeflow::IoPortBindings options_ports = {{"query", "query"}};
  options.ports = &options_ports;
  options.type = "entity_in";
  options.name = "translate";

  AlgContext carrier_ctx;
  AdapterStatus carrier_status;
  int ret = converter->decode_fn(carrier_view, options, &carrier_ctx,
                                 &carrier_status);
  EXPECT_EQ(ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(carrier_status.SampleIndex(), 0);
  EXPECT_EQ(carrier_status.FieldPath(), "sentence_text");

  // 业务解码错误使用所选 Converter 的 type/name 标签
  std::string bad_json = "{\"wrong_field\":123}";
  CompanyString cs_biz{static_cast<int32_t>(bad_json.size()),
                       const_cast<char*>(bad_json.data())};
  CompanyOperatorEntityInput in_biz{103, kMockServiceTranslate, &cs_biz};
  ExternalInputBatchView biz_view;
  biz_view.count = 1;
  biz_view.slots["entity_in"] = llm_edgeflow::BorrowInputForTest({&in_biz});
  biz_view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

  AlgContext biz_ctx;
  AdapterStatus biz_status;
  EXPECT_EQ(converter->decode_fn(biz_view, options, &biz_ctx, &biz_status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(biz_status.AdapterName(), "entity_in/translate");
  EXPECT_EQ(biz_status.FieldPath(), "json");
}

// 返回码与 AdapterStatus 相互独立
TEST_F(AdapterContractSecurityTest,
       TranslationReturnCodeAndAdapterStatusIndependence) {
  const auto* converter = IoConverterRegistry::Instance().FindOutputConverter(
      "entity_out", "translate");
  ASSERT_NE(converter, nullptr);

  // 存在请求 ID 表，但缺少答案
  AlgContext ctx;
  const std::vector<uint64_t> request_ids{1001};

  CompanyOperatorEntityOutput out{};
  ExternalOutputBatchView view;
  view.count = 1;
  view.leased_slots["entity_out"] = {&out};
  view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";

  OutputEncodeOptions options;
  const llm_edgeflow::IoPortBindings options_ports = {
      {"translation", "translation"}};
  options.ports = &options_ports;
  options.type = "entity_out";
  options.name = "translate";
  options.request_ids = &request_ids;

  size_t written = 0;
  AdapterStatus status;
  int ret = converter->encode_fn(&ctx, options, &view, &written, &status);

  // 缺少内部数据属于非法输入，与输出容量无关。
  EXPECT_EQ(ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
}

// 翻译序列化失败 (非法 UTF-8) 优先于容量检查
TEST_F(AdapterContractSecurityTest,
       TranslationSerializationFailurePriorityOverCapacity) {
  const auto* converter = IoConverterRegistry::Instance().FindOutputConverter(
      "entity_out", "translate");
  ASSERT_NE(converter, nullptr);

  // 存在请求 ID 表，但答案含非法 UTF-8 字节序列
  AlgContext ctx;
  const std::vector<uint64_t> request_ids{1001};
  std::string invalid_utf8 = "prefix\xFF\xFFsuffix";
  ctx.Publish(kTranslation, TextBatch{{0, 0, invalid_utf8}});

  CompanyOperatorEntityOutput out{};
  ExternalOutputBatchView view;
  view.count = 1;
  view.leased_slots["entity_out"] = {&out};
  view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";

  OutputEncodeOptions options;
  const llm_edgeflow::IoPortBindings options_ports = {
      {"translation", "translation"}};
  options.ports = &options_ports;
  options.type = "entity_out";
  options.name = "translate";
  options.request_ids = &request_ids;

  size_t written = 0;
  AdapterStatus status;

  // Encode 中先序列化再校验容量；
  // 未处理的 dump 异常会从 encode_fn 抛出
  EXPECT_THROW(converter->encode_fn(&ctx, options, &view, &written, &status),
               std::exception);
}

// 翻译在 AlgContext 为空时的诊断
TEST_F(AdapterContractSecurityTest, TranslateNullContextDiagnostics) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "entity_in", "translate");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "entity_out", "translate");
  ASSERT_NE(out_conv, nullptr);

  // 1. context 为空时 Decode 必须返回 INVALID_INPUT (-3)，字段为 "context"
  std::string query_json = "{\"query\":\"test\"}";
  CompanyString cs{static_cast<int32_t>(query_json.size()),
                   const_cast<char*>(query_json.data())};
  CompanyOperatorEntityInput input{100, kMockServiceTranslate, &cs};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.slots["entity_in"] = llm_edgeflow::BorrowInputForTest({&input});
  in_view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

  InputDecodeOptions in_options;
  const llm_edgeflow::IoPortBindings in_options_ports = {{"query", "query"}};
  in_options.ports = &in_options_ports;
  in_options.type = "entity_in";
  in_options.name = "translate";

  AdapterStatus unpack_status;
  int unpack_ret =
      in_conv->decode_fn(in_view, in_options, nullptr, &unpack_status);
  EXPECT_EQ(unpack_ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(unpack_status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(unpack_status.FieldPath(), "context");
  EXPECT_EQ(unpack_status.AdapterName(), "entity_in/translate");

  EXPECT_EQ(in_conv->decode_fn(in_view, in_options, nullptr, nullptr),
            COMPANY_ALG_ERR_INVALID_INPUT);

  // 2. context 为空时 Encode 必须返回 INVALID_INPUT (-3)，字段为 "context"
  CompanyOperatorEntityOutput output{};
  ExternalOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["entity_out"] = {&output};
  out_view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";

  OutputEncodeOptions out_options;
  const llm_edgeflow::IoPortBindings out_options_ports = {
      {"translation", "translation"}};
  out_options.ports = &out_options_ports;
  out_options.type = "entity_out";
  out_options.name = "translate";

  size_t written = 0;
  AdapterStatus pack_status;
  int pack_ret = out_conv->encode_fn(nullptr, out_options, &out_view, &written,
                                     &pack_status);
  EXPECT_EQ(pack_ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(pack_status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(pack_status.FieldPath(), "context");
  EXPECT_EQ(pack_status.AdapterName(), "entity_out/translate");

  EXPECT_EQ(
      out_conv->encode_fn(nullptr, out_options, &out_view, &written, nullptr),
      COMPANY_ALG_ERR_INVALID_INPUT);
}

TEST_F(AdapterContractSecurityTest,
       DecodeAndEncodeRejectMissingRequestIdTable) {
  const auto* input = IoConverterRegistry::Instance().FindInputConverter(
      "keyword_in", "keyword_match");
  const auto* output = IoConverterRegistry::Instance().FindOutputConverter(
      "keyword_out", "keyword_match");
  ASSERT_NE(input, nullptr);
  ASSERT_NE(output, nullptr);
  char bytes[] = "query";
  CompanyString text{5, bytes};
  CompanyOperatorKeywordInput row{900001, kMockServiceKeywordMatch, &text};
  ExternalInputBatchView source;
  source.count = 1;
  source.slots["keyword_in"] = BorrowInputForTest({&row});
  source.slot_types["keyword_in"] = "CompanyOperatorKeywordInput";
  InputDecodeOptions in_options;
  const llm_edgeflow::IoPortBindings in_options_ports =
      llm_edgeflow::test::ConverterPortsForTest(*input);
  in_options.ports = &in_options_ports;
  in_options.type = input->type;
  in_options.name = input->name;
  AlgContext context;
  AdapterStatus status;
  EXPECT_EQ(input->decode_fn(source, in_options, &context, &status),
            COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "request_ids");
  EXPECT_EQ(status.AdapterName(), input->Label());
  EXPECT_EQ(status.Message(), "Missing request id table in decode options");
  EXPECT_FALSE(context.Has("sentence_text"));

  ASSERT_TRUE(context.Publish(kMatches, RuleMatchBatch{{0, 0, {}}}));
  OutputEncodeOptions out_options;
  const llm_edgeflow::IoPortBindings out_options_ports =
      llm_edgeflow::test::ConverterPortsForTest(*output);
  out_options.ports = &out_options_ports;
  out_options.type = output->type;
  out_options.name = output->name;
  CompanyOperatorKeywordOutput result{};
  result.request_id = 123;
  ExternalOutputBatchView destination;
  destination.count = 1;
  destination.leased_slots["keyword_out"] = {&result};
  destination.slot_types["keyword_out"] = "CompanyOperatorKeywordOutput";
  size_t written = 99;
  EXPECT_EQ(
      output->encode_fn(&context, out_options, &destination, &written, &status),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(status.FieldPath(), "request_ids");
  EXPECT_EQ(status.AdapterName(), output->Label());
  EXPECT_EQ(status.Message(), "Missing request id table in encode options");
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(result.request_id, 123U);
}

TEST_F(AdapterContractSecurityTest, InputLengthLimitsStayUnchanged) {
  struct Boundary {
    const char* type;
    const char* name;
    int32_t service_type;
    const char* field;
    size_t limit;
    const char* message;
  };
  // 数值期望与实现中的常量保持独立。
  const Boundary boundaries[] = {
      {"keyword_in", "keyword_match", kMockServiceKeywordMatch, "sentence_text",
       65536, "sentence_text length exceeds 64 KiB limit"},
      {"entity_in", "entity_extract", kMockServiceEntityExtract,
       "sentence_text", 65536, "sentence_text length exceeds 64 KiB limit"},
      {"entity_in", "translate", kMockServiceTranslate, "sentence_text", 65536,
       "sentence_text length exceeds 64 KiB limit"},
      {"audit_in", "dialogue_audit", kMockServiceDialogueAudit,
       "audit_in.user_text", 65536, "user_text length exceeds limit"},
      {"doc_in", "doc_qa", kMockServiceDocQa, "doc_in.query_text", 65536,
       "query_text length exceeds limit"},
      {"doc_in", "doc_qa", kMockServiceDocQa, "doc_in.doc_text", 10485760,
       "doc_text length exceeds limit"},
      {"frame", "ocr_invoice_qa", kMockServiceOcrInvoiceQa, "frame.image_uri",
       4096, "image_uri length exceeds limit"},
      {"string", "ocr_invoice_qa", 0, "string", 65536,
       "query length exceeds limit"},
      {"rerank_in", "cross_rerank", kMockServiceCrossRerank,
       "rerank_in.query_text", 65536, "query_text length exceeds limit"},
      {"rerank_in", "cross_rerank", kMockServiceCrossRerank,
       "rerank_in.candidate_passages", 65536,
       "candidate passage length exceeds limit"},
      {"rerank_in", "cross_rerank", kMockServiceCrossRerank,
       "rerank_in.candidate_count", 8,
       "candidate_count out of valid range [1, 8]"},
  };
  for (const auto& boundary : boundaries) {
    const auto* converter = IoConverterRegistry::Instance().FindInputConverter(
        boundary.type, boundary.name);
    ASSERT_NE(converter, nullptr) << boundary.type << "/" << boundary.name;
    const std::string label = converter->Label();
    const std::string type = boundary.type;
    const std::string field = boundary.field;
    for (size_t extra : {0U, 1U}) {
      SCOPED_TRACE(label + " " + field + " +" + std::to_string(extra));
      std::string text(boundary.limit + extra, 'x');
      if (converter->name == "translate") {
        text = "{\"query\":\"" + std::string(text.size() - 12, 'x') + "\"}";
      }
      CompanyString large{static_cast<int32_t>(text.size()), text.data()};
      char short_text[] = "query";
      CompanyString small{5, short_text};
      const int32_t service_type = boundary.service_type;
      CompanyOperatorKeywordInput keyword{101, service_type, &large};
      CompanyOperatorEntityInput entity{101, service_type, &large};
      CompanyOperatorAuditInput audit{101, service_type, &large, nullptr};
      CompanyOperatorDocInput doc{
          101, service_type, field == "doc_in.doc_text" ? &large : &small,
          field == "doc_in.query_text" ? &large : &small};
      CompanyFrame frame{101, service_type,
                         field == "frame.image_uri" ? &large : &small, nullptr};
      CompanyOperatorRerankInput rerank{};
      rerank.request_id = 101;
      rerank.service_type = service_type;
      rerank.query_text = field == "rerank_in.query_text" ? &large : &small;
      rerank.candidate_count =
          field == "rerank_in.candidate_count"
              ? static_cast<int32_t>(boundary.limit + extra)
              : 1;
      for (auto& passage : rerank.candidate_passages)
        passage = field == "rerank_in.candidate_passages" ? &large : &small;

      ExternalInputBatchView view;
      view.count = 1;
      view.slot_types[converter->type] = converter->slot.type_id;
      if (type == "keyword_in")
        view.slots[type] = BorrowInputForTest({&keyword});
      else if (type == "entity_in")
        view.slots[type] = BorrowInputForTest({&entity});
      else if (type == "audit_in")
        view.slots[type] = BorrowInputForTest({&audit});
      else if (type == "doc_in")
        view.slots[type] = BorrowInputForTest({&doc});
      else if (type == "frame")
        view.slots[type] = BorrowInputForTest({&frame});
      else if (type == "string")
        view.slots[type] = BorrowInputForTest({&large});
      else
        view.slots[type] = BorrowInputForTest({&rerank});
      AlgContext context;
      AdapterStatus status;
      InputDecodeOptions options;
      const llm_edgeflow::IoPortBindings options_ports =
          llm_edgeflow::test::ConverterPortsForTest(*converter);
      options.ports = &options_ports;
      options.type = converter->type;
      options.name = converter->name;
      std::vector<uint64_t> request_ids;
      options.request_ids = &request_ids;
      const int result = converter->decode_fn(view, options, &context, &status);
      EXPECT_EQ(result, extra == 0 ? 0 : COMPANY_ALG_ERR_INVALID_INPUT);
      EXPECT_EQ(status.ToString(),
                extra == 0 ? "OK"
                           : "[AdapterStatus] Error -3 in Adapter [" + label +
                                 "] at sample [0] field `" + field +
                                 "`: " + boundary.message);
    }
  }
}

TEST_F(AdapterContractSecurityTest, OperatorInputLimitsStayUnchanged) {
  using namespace operator_api;
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("edgeflow-input-boundaries-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{directory};
  const auto root = std::filesystem::absolute(GetConfigPath("configs"))
                        .parent_path()
                        .string();
  const auto ops = Get_LLM_EDGEFLOW_OperatorTable();
  const auto check = [&](const char* conf, const NamedIoBatch& inputs,
                         NamedIoBatch outputs, int expected,
                         const std::string& diagnostic) {
    CreateParam param{};
    const std::filesystem::path conf_path(conf);
    const auto model_root =
        conf_path.is_absolute() ? conf_path.parent_path().string() : root;
    const auto relative_conf = conf_path.is_absolute()
                                   ? conf_path.filename().string()
                                   : std::string(conf);
    param.model_path = model_root.c_str();
    param.cfg_file_name = relative_conf.c_str();
    param.compute_platform = ComputePlatform::kCpu;
    void* raw_handle = nullptr;
    ASSERT_EQ(ops.Create(&raw_handle, &param), 0) << GetOperatorLastError();
    const auto destroy = [ops](void* handle) { ops.Destroy(handle); };
    std::unique_ptr<void, decltype(destroy)> handle(raw_handle, destroy);
    // 成功的调用保留现有的线程局部 last error。
    const std::string previous_error = GetOperatorLastError();
    EXPECT_EQ(ops.Process(handle.get(), inputs, outputs), expected)
        << GetOperatorLastError();
    EXPECT_EQ(GetOperatorLastError(),
              expected == 0 ? previous_error : diagnostic);
    if (expected == 0) {
      for (const auto& [key, value] : outputs[0])
        EXPECT_NE(value, nullptr) << key;
    } else {
      for (const auto& [key, value] : outputs[0])
        EXPECT_EQ(value, nullptr) << key;
    }
  };

  for (size_t length : {65536U, 65537U}) {
    std::string text(length, 'x');
    CompanyString cs{static_cast<int32_t>(length), text.data()};
    CompanyOperatorKeywordInput keyword{101, kMockServiceKeywordMatch, &cs};
    NamedIoBatch inputs(1), outputs(1);
    inputs[0]["test.keyword_in"] = MakeBorrowedOperatorInput(&keyword);
    outputs[0]["test.keyword_out"] = nullptr;
    check("configs/pipeline_keyword_match_rules.conf", inputs, outputs,
          length == 65536 ? 0 : -3,
          length == 65536
              ? ""
              : "Validation failed for input key test.keyword_in: "
                "sentence_text length 65537 exceeds max limit 65536");
  }

  // 使用生产计算图和测试模型执行；两条拒绝路径都不应到达模型推理，
  // 也不需要外部模型资源。
  std::ifstream source(GetConfigPath("configs/pipeline_cross_rerank_cpu.json"));
  ASSERT_TRUE(source.is_open());
  nlohmann::json pipeline;
  source >> pipeline;
  pipeline["models"][0]["type"] = "rerank";
  pipeline["models"][0]["backend"] = {{"type", "test_tensor_backend"}};
  pipeline["models"][0]["file"] = "boundary.fixture";
  pipeline["models"][0]["params"] = nlohmann::json::object();
  std::ofstream(directory / "pipeline.json") << pipeline.dump();
  std::ofstream(directory / "pipeline.conf")
      << "{\"pipe_path\":\"pipeline.json\"}";
  const auto conf = (directory / "pipeline.conf").string();
  for (size_t length : {65537U, 10485761U}) {
    std::string text(length, 'x');
    CompanyString passage{static_cast<int32_t>(length), text.data()};
    char query_text[] = "query";
    CompanyString query{5, query_text};
    CompanyOperatorRerankInput rerank{};
    rerank.request_id = 101;
    rerank.service_type = kMockServiceCrossRerank;
    rerank.query_text = &query;
    rerank.candidate_count = 1;
    rerank.candidate_passages[0] = &passage;
    NamedIoBatch inputs(1), outputs(1);
    inputs[0]["test.rerank_in"] = MakeBorrowedOperatorInput(&rerank);
    outputs[0]["test.rerank_out"] = nullptr;
    check(conf.c_str(), inputs, outputs, -3,
          length == 65537
              ? "DecodeInput failed for rerank_in/cross_rerank: "
                "[AdapterStatus] Error -3 in Adapter [rerank_in/cross_rerank] "
                "at sample [0] field `rerank_in.candidate_passages`: candidate "
                "passage length exceeds limit"
              : "Validation failed for input key test.rerank_in: "
                "candidate_passages[0] length 10485761 exceeds max limit "
                "10485760");
  }

  for (size_t length : {4096U, 4097U}) {
    std::string path(length, 'x');
    CompanyString uri{static_cast<int32_t>(length), path.data()};
    char query_text[] = "query";
    CompanyString query{5, query_text};
    CompanyFrame frame{101, kMockServiceOcrInvoiceQa, &uri, nullptr};
    NamedIoBatch inputs(1), outputs(1);
    inputs[0]["test.frame"] = MakeBorrowedOperatorInput(&frame);
    inputs[0]["test.string"] = MakeBorrowedOperatorInput(&query);
    outputs[0]["test.od_out"] = nullptr;
    check("demo/fixtures/mock/pipeline_ocr_invoice_qa.conf", inputs, outputs,
          length == 4096 ? 0 : -3,
          length == 4096
              ? ""
              : "Validation failed for input key test.frame: "
                "CompanyFrame.image_uri length 4097 exceeds max limit 4096");
  }
}

TEST_F(AdapterContractSecurityTest,
       KeywordOperatorRuleMatchResponseIsByteStable) {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("edgeflow-keyword-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
  } cleanup{directory};
  // 关键词和规则命中、类型化规则常量、正则捕获以及默认命中
  // 都会进入外部 JSON 响应。
  const nlohmann::json pipeline = nlohmann::json::parse(R"j({
  "io": {
    "input": [
      {
        "type": "keyword_in",
        "name": "keyword_match"
      }
    ],
    "output": [
      {
        "type": "keyword_out",
        "name": "keyword_match",
        "inputs": {
          "matches": "rules.matches"
        }
      }
    ]
  },
  "models": [],
  "pipeline": [
    {
      "inputs": {
        "text": "input.sentence_text"
      },
      "name": "rules",
      "type": "text_rule_match",
      "params": {
        "categories": {
          "A": [
            "x"
          ]
        },
        "rules": [
          {
            "id": "r_nav",
            "strategy": "regex",
            "pattern": "导航到(?<city>北京)",
            "category": "NAV",
            "score": 0.75,
            "constants": {
              "flag": true,
              "n": 3,
              "s": "v"
            }
          },
          {
            "id": "r_y",
            "strategy": "contains",
            "pattern": "y",
            "category": "B",
            "score": 0.5
          }
        ],
        "default_category": "FALLBACK",
        "default_score": 0.25
      }
    }
  ]
})j");
  std::ofstream(directory / "pipeline.json") << pipeline.dump();
  std::ofstream(directory / "pipeline.conf")
      << "{\"pipe_path\":\"pipeline.json\"}";

  operator_api::CreateParam create{};
  create.model_path = directory.c_str();
  create.cfg_file_name = "pipeline.conf";
  create.compute_platform = operator_api::ComputePlatform::kCpu;
  void* raw_handle = nullptr;
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  ASSERT_EQ(op.Create(&raw_handle, &create), 0);
  std::unique_ptr<void, int (*)(void*)> handle(raw_handle, op.Destroy);

  std::string hit_text = "x 导航到北京y";
  std::string fallback_text = "nothing";
  CompanyString hit{static_cast<int32_t>(hit_text.size()), hit_text.data()};
  CompanyString fallback{static_cast<int32_t>(fallback_text.size()),
                         fallback_text.data()};
  CompanyOperatorKeywordInput first{301, kMockServiceKeywordMatch, &hit};
  CompanyOperatorKeywordInput second{302, kMockServiceKeywordMatch, &fallback};
  operator_api::NamedIoBatch inputs(2), outputs(2);
  inputs[0]["kw.keyword_in"] = operator_api::MakeBorrowedOperatorInput(&first);
  inputs[1]["kw.keyword_in"] = operator_api::MakeBorrowedOperatorInput(&second);
  outputs[0]["kw.keyword_out"] = nullptr;
  outputs[1]["kw.keyword_out"] = nullptr;
  ASSERT_EQ(op.Process(handle.get(), inputs, outputs), 0)
      << operator_api::GetOperatorLastError();

  auto response = [&](size_t index) {
    const auto* out = static_cast<CompanyOperatorKeywordOutput*>(
        outputs[index]["kw.keyword_out"].get());
    EXPECT_NE(out, nullptr);
    EXPECT_EQ(out->is_hit, 1);
    EXPECT_EQ(out->status_code, 0);
    return std::string(out->match_result_json->data,
                       out->match_result_json->length);
  };
  EXPECT_EQ(response(0),
            R"j({"confidence":1.0,"intent":"A","matched_word":"x","matches":[)j"
            R"j({"category":"A","matched_word":"x"},)j"
            R"j({"category":"NAV","pattern":"导航到(?<city>北京)",)j"
            R"j("rule_id":"r_nav","score":0.75},)j"
            R"j({"category":"B","pattern":"y","rule_id":"r_y","score":0.5}],)j"
            R"j("slots":{"city":"北京","flag":true,"n":3,"s":"v"}})j");
  EXPECT_EQ(response(1),
            R"j({"confidence":0.25,"intent":"FALLBACK","matched_word":"",)j"
            R"j("matches":[],"slots":{"raw_query":"nothing"}})j");
}

TEST_F(AdapterContractSecurityTest, CreateAndExecutionFailuresUseStageCodes) {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("edgeflow-stage-codes-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
  } cleanup{directory};
  nlohmann::json pipeline = nlohmann::json::parse(R"j({
  "io": {
    "input": [
      {
        "type": "entity_in",
        "name": "entity_extract"
      }
    ],
    "output": [
      {
        "type": "entity_out",
        "name": "entity_extract",
        "inputs": {
          "entities": "parse.document"
        }
      }
    ]
  },
  "models": [
    {
      "name": "llm",
      "type": "llm",
      "backend": {
        "type": "test_causal_lm_backend"
      },
      "file": "neutral-llm.fixture"
    }
  ],
  "pipeline": [
    {
      "inputs": {
        "input": "input.sentence_text"
      },
      "name": "gen",
      "type": "llm_generate",
      "params": {
        "bind_model": "llm",
        "endpoints": {
          "answer": {}
        }
      }
    },
    {
      "inputs": {
        "text": "gen.text"
      },
      "name": "parse",
      "type": "structured_json_parse",
      "params": {
        "failure_policy": "fail"
      }
    }
  ]
})j");
  std::ofstream(directory / "pipeline.conf")
      << "{\"pipe_path\":\"pipeline.json\"}";
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  auto create = [&](const nlohmann::json& document, void** handle) {
    std::ofstream(directory / "pipeline.json") << document.dump();
    operator_api::CreateParam param{};
    param.model_path = directory.c_str();
    param.cfg_file_name = "pipeline.conf";
    param.compute_platform = operator_api::ComputePlatform::kCpu;
    return op.Create(handle, &param);
  };

  // 校验失败和模型加载失败都属于创建参数错误。
  void* handle = nullptr;
  auto invalid = pipeline;
  invalid["pipeline"][1]["type"] = "missing_node_type";
  EXPECT_EQ(create(invalid, &handle), COMPANY_ALG_ERR_INVALID_PARAM);
  EXPECT_NE(std::string(operator_api::GetOperatorLastError())
                .find("Create preparation failed with internal code -3: "),
            std::string::npos)
      << operator_api::GetOperatorLastError();
  auto unloadable = pipeline;
  unloadable["models"][0]["backend"] = {{"type", "failing_create_backend"}};
  EXPECT_EQ(create(unloadable, &handle), COMPANY_ALG_ERR_INVALID_PARAM);
  const std::string load_error = operator_api::GetOperatorLastError();
  EXPECT_NE(load_error.find("Create preparation failed with internal code -3"),
            std::string::npos)
      << load_error;
  EXPECT_NE(load_error.find("MODEL_MATERIALIZATION_FAILED"), std::string::npos)
      << load_error;
  EXPECT_EQ(handle, nullptr);

  ASSERT_EQ(create(pipeline, &handle), 0)
      << operator_api::GetOperatorLastError();
  std::unique_ptr<void, int (*)(void*)> owner(handle, op.Destroy);

  // 测试 Backend 返回纯文本，因此 fail 策略会拒绝它。
  std::string text = "张三在北京";
  CompanyString sentence{static_cast<int32_t>(text.size()), text.data()};
  CompanyOperatorEntityInput input{11, kMockServiceEntityExtract, &sentence};
  operator_api::NamedIoBatch inputs(1), outputs(1);
  inputs[0]["e.entity_in"] = operator_api::MakeBorrowedOperatorInput(&input);
  outputs[0]["e.entity_out"] = nullptr;
  EXPECT_EQ(op.Process(handle, inputs, outputs), COMPANY_ALG_ERR_UNKNOWN);
  const std::string error = operator_api::GetOperatorLastError();
  EXPECT_NE(error.find("Pipeline execution failed with internal code -6102: "
                       "Node 'parse' (structured_json_parse)"),
            std::string::npos)
      << error;
  EXPECT_EQ(outputs[0]["e.entity_out"], nullptr);
}

}  // namespace llm_edgeflow
