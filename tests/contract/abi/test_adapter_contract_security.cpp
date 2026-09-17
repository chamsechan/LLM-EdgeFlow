#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "adapter/adapter_status.h"
#include "adapter/adapter_validation_helper.h"
#include "adapter/biz_blackboard_keys.h"
#include "adapter/biz_results.h"
#include "adapter/deployment_model_resolver.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_binding_resolver.h"
#include "adapter/io_converter_registry.h"
#include "adapter/shared_algorithm_runtime.h"
#include "edgeflow/operator/interface.h"
#include "edgeflow/operator/types.h"
#include "engine/model_registry.h"
#include "platform_mock/error_codes.h"
#include "tests/support/adapter_examples/flat_struct_adapter.h"
#include "tests/support/adapter_examples/nested_array_adapter.h"
#include "tests/support/adapter_examples/nested_pointer_tree_adapter.h"
#include "tests/support/adapter_examples/tagged_union_adapter.h"

namespace llm_edgeflow {

static std::string GetConfigPath(const std::string& rel_path) {
  if (std::filesystem::exists(rel_path)) return rel_path;
  if (std::filesystem::exists("../" + rel_path)) return "../" + rel_path;
  return rel_path;
}

class AdapterContractSecurityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IoBindingRegistry::Instance().ResetConflictForTesting();
    IoConverterRegistry::Instance().ResetConflictForTesting();
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Init();
  }
  void TearDown() override {
    operator_api::Get_LLM_EDGEFLOW_OperatorTable().Deinit();
    IoBindingRegistry::Instance().ResetConflictForTesting();
    IoConverterRegistry::Instance().ResetConflictForTesting();
  }
};

namespace {

// This fixture records actual generation calls while the test executes the
// shipped translation Pipeline through Operator API. It does no JSON handling.
class TranslationProbeModel final : public ILlmModel {
 public:
  inline static constexpr char kModelType[] = "test_translation_probe";
  inline static std::weak_ptr<TranslationProbeModel> latest;

  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string* error) {
    auto model = std::make_shared<TranslationProbeModel>();
    latest = model;
    if (error) error->clear();
    return model;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string type = kModelType;
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "llm";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  size_t GetMaxBatchSize() const noexcept override { return 1; }
  int Generate(const TextBatch& prompts, const GenerateOptions&,
               TextBatch* outputs) noexcept override {
    try {
      calls.push_back(prompts);
      if (failure != 0) return failure;
      if (!outputs) return -1;
      outputs->clear();
      for (const auto& prompt : prompts) {
        outputs->emplace_back(prompt.req_id, prompt.sub_id, response);
      }
      return 0;
    } catch (...) {
      return -1;
    }
  }

  std::vector<TextBatch> calls;
  std::string response;
  int failure = 0;
};

REGISTER_MODEL_WITH_DEFINITION(TranslationProbeModel, [] {
  ModelDefinition definition;
  definition.model_type = TranslationProbeModel::kModelType;
  definition.capability = "llm";
  definition.description = "Test-only translation generation probe";
  definition.required_protocol = ExecutionProtocol::kTextGeneration;
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
  // Keep the production graph and port bindings. Substitute only model
  // execution so these assertions require neither model assets nor a Demo.
  ASSERT_EQ(pipeline.at("pipeline").size(), 1U);
  EXPECT_EQ(pipeline["pipeline"][0]["node_type"], "LlmGenerateNode");
  ASSERT_EQ(pipeline.at("models").size(), 1U);
  auto& model_config = pipeline["models"][0];
  model_config["model_type"] = TranslationProbeModel::kModelType;
  model_config["backend"] = "test_causal_lm_backend";
  model_config["model_path"] = "translation-probe.fixture";
  model_config["model_config"] = nlohmann::json::object();
  model_config["backend_config"] = nlohmann::json::object();
  std::ofstream(pipe_path) << pipeline.dump();

  nlohmann::json op_cfg = {
      {"schema_version", 1},
      {"data",
       {{"pipe_path", "pipeline.json"},
        {"io_binding", "translate.operator.v1"},
        {"outputs",
         {{"entity_out",
           {{"type", "entity_out"},
            {"meta_num", 0},
            {"metadata_type_id", 0},
            {"capacities", {{"entities_json", 2047}}}}}}}}}};
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
  // Text that happens to resemble JSON or Markdown is still the original
  // model result: no JSON parsing, field extraction, stripping, or retries.
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
  model->failure = -731;
  EXPECT_EQ(process("{\"query\":\"hello\"}"), -731);
  EXPECT_EQ(model->calls.size(), before_invalid + 2);
  model->failure = 0;
  model->response = "你好";
  EXPECT_EQ(process("{\"query\":\"hello\"}"), 0);
  EXPECT_EQ(model->calls.size(), before_invalid + 3);
  EXPECT_EQ(nlohmann::json::parse(out_entities_json),
            nlohmann::json({{"translated", "你好"}}));

  // Invalid UTF-8 in model response causes JSON dump to throw, mapping to
  // COMPANY_ALG_ERR_EXCEPTION (-99) through the public Operator Process barrier
  model->response = "prefix\xFF\xFFsuffix";
  EXPECT_EQ(process("{\"query\":\"hello\"}"), COMPANY_ALG_ERR_EXCEPTION);
  model->response = "你好";
}

TEST_F(AdapterContractSecurityTest,
       TranslationLiteralResultPackingAndCarrierSafety) {
  const auto* converter = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(converter, nullptr);
  const std::string translation(2200, 'x');
  AlgContext large;
  large.Publish(kRawRequestIds, std::vector<uint64_t>{123});
  large.Publish(kLlmAnswers, TextBatch{{0, 0, translation}});
  OutputPortBindings bindings(
      {{"raw_request_ids", "raw_request_ids"}, {"llm_answers", "llm_answers"}});
  OutputEncodeOptions options;
  options.converter_id = "translate.json.operator.v1";
  options.transport = "operator";
  AdapterStatus status;

  char buf_large[2500] = {0};
  CompanyString cs_large{0, buf_large};
  CompanyOperatorEntityOutput fixed{};
  fixed.entities_json = &cs_large;

  ExternalOutputBatchView fixed_view;
  fixed_view.count = 1;
  fixed_view.leased_slots["entity_out"] = {&fixed};
  fixed_view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";
  fixed_view.slot_capacities["entity_out"]["entities_json"] = 2047;
  size_t written = 0;

  EXPECT_EQ(converter->encode_fn(&large, bindings, options, &fixed_view,
                                 &written, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);

  // Reordered internal results must map back to external request IDs.
  AlgContext reordered;
  reordered.Publish(kRawRequestIds, std::vector<uint64_t>{999, 123});
  reordered.Publish(kLlmAnswers, TextBatch{{1, 0, "第二句"}, {0, 0, "第一句"}});
  char buf_first[512] = {0};
  char buf_second[512] = {0};
  CompanyString cs_first{0, buf_first};
  CompanyString cs_second{0, buf_second};
  CompanyOperatorEntityOutput first{}, second{};
  first.entities_json = &cs_first;
  second.entities_json = &cs_second;

  ExternalOutputBatchView reordered_view;
  reordered_view.count = 2;
  reordered_view.leased_slots["entity_out"] = {&first, &second};
  reordered_view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";
  reordered_view.slot_capacities["entity_out"]["entities_json"] = 511;

  ASSERT_EQ(converter->encode_fn(&reordered, bindings, options, &reordered_view,
                                 &written, &status),
            0);
  EXPECT_EQ(written, 2U);
  EXPECT_EQ(first.request_id, 999U);
  EXPECT_EQ(second.request_id, 123U);
  EXPECT_EQ(nlohmann::json::parse(first.entities_json->data),
            nlohmann::json({{"translated", "第一句"}}));
  EXPECT_EQ(nlohmann::json::parse(second.entities_json->data),
            nlohmann::json({{"translated", "第二句"}}));

  reordered_view.count = 1;
  EXPECT_EQ(converter->encode_fn(&reordered, bindings, options, &reordered_view,
                                 &written, &status),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);

  for (const TextBatch& invalid :
       std::vector<TextBatch>{{},
                              {{2, 0, "out-of-range"}},
                              {{0, 1, "invalid-sub-id"}},
                              {{0, 0, "duplicate"}, {0, 0, "duplicate"}},
                              {{0, 0, "missing-second"}}}) {
    AlgContext ctx;
    ctx.Publish(kRawRequestIds, std::vector<uint64_t>{999, 123});
    ctx.Publish(kLlmAnswers, invalid);
    reordered_view.count = 2;
    EXPECT_EQ(converter->encode_fn(&ctx, bindings, options, &reordered_view,
                                   &written, &status),
              COMPANY_ALG_ERR_INVALID_INPUT);
  }
  for (bool publish_ids : {false, true}) {
    AlgContext missing;
    if (publish_ids) {
      missing.Publish(kRawRequestIds, std::vector<uint64_t>{123});
    } else {
      missing.Publish(kLlmAnswers, TextBatch{{0, 0, "你好"}});
    }
    reordered_view.count = 1;
    EXPECT_EQ(converter->encode_fn(&missing, bindings, options, &reordered_view,
                                   &written, &status),
              COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  }
}

TEST_F(AdapterContractSecurityTest, DeploymentModelRootContractIsSandboxed) {
  const std::string root_input = GetConfigPath("models");
  const auto root = std::filesystem::absolute(root_input);
  ASSERT_TRUE(std::filesystem::is_directory(root));

  nlohmann::json pipeline_json = {
      {"models", nlohmann::json::array({{{"model_path", "artifact.onnx"}}})}};
  nlohmann::json resolved;
  std::string diagnostic;

  ASSERT_TRUE(ResolveDeploymentModelPaths(pipeline_json, root_input, &resolved,
                                          &diagnostic))
      << diagnostic;
  EXPECT_EQ(std::filesystem::path(
                resolved["models"][0]["model_path"].get<std::string>()),
            std::filesystem::weakly_canonical(root / "artifact.onnx"));

  for (const char* safe_path :
       {"..name/artifact.onnx", "nested/../artifact.onnx"}) {
    pipeline_json["models"][0]["model_path"] = safe_path;
    ASSERT_TRUE(ResolveDeploymentModelPaths(pipeline_json, root_input,
                                            &resolved, &diagnostic))
        << safe_path << ": " << diagnostic;
    EXPECT_EQ(std::filesystem::path(
                  resolved["models"][0]["model_path"].get<std::string>()),
              std::filesystem::weakly_canonical(root / safe_path));
  }

  diagnostic.clear();
  EXPECT_FALSE(
      ResolveDeploymentModelPaths(pipeline_json, "", &resolved, &diagnostic));
  EXPECT_NE(diagnostic.find("requires non-empty model_root_dir"),
            std::string::npos);

  pipeline_json["models"][0]["model_path"] = "../escape.onnx";
  diagnostic.clear();
  EXPECT_FALSE(ResolveDeploymentModelPaths(pipeline_json, root.string(),
                                           &resolved, &diagnostic));
  EXPECT_NE(diagnostic.find("cannot traverse"), std::string::npos);

  const auto unique_suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto temp_base = std::filesystem::temp_directory_path() /
                         ("edgeflow_resolver_" + unique_suffix);
  const auto sandbox = temp_base / "sandbox";
  const auto outside = temp_base / "outside";
  std::filesystem::create_directories(sandbox);
  std::filesystem::create_directories(outside);
  std::filesystem::create_directory_symlink(outside, sandbox / "escape_link");
  pipeline_json["models"][0]["model_path"] = "escape_link/artifact.onnx";
  diagnostic.clear();
  EXPECT_FALSE(ResolveDeploymentModelPaths(pipeline_json, sandbox.string(),
                                           &resolved, &diagnostic));
  EXPECT_NE(diagnostic.find("escapes model_root_dir"), std::string::npos);

  pipeline_json["models"][0]["model_path"] =
      (temp_base / "sandbox_extra/artifact.onnx").string();
  diagnostic.clear();
  EXPECT_FALSE(ResolveDeploymentModelPaths(pipeline_json, sandbox.string(),
                                           &resolved, &diagnostic));
  EXPECT_NE(diagnostic.find("escapes model_root_dir"), std::string::npos);

  pipeline_json["models"][0]["model_path"] =
      (sandbox / "absolute.onnx").string();
  EXPECT_TRUE(ResolveDeploymentModelPaths(pipeline_json, sandbox.string(),
                                          &resolved, &diagnostic))
      << diagnostic;
  std::filesystem::remove_all(temp_base);

  pipeline_json["models"][0]["model_path"] =
      std::filesystem::weakly_canonical(root / "absolute.onnx").string();
  diagnostic.clear();
  EXPECT_TRUE(
      ResolveDeploymentModelPaths(pipeline_json, "", &resolved, &diagnostic))
      << diagnostic;

  diagnostic.clear();
  const nlohmann::json model_less_pipeline = {
      {"biz_name", "model_less"}, {"models", nlohmann::json::array()}};
  EXPECT_TRUE(ResolveDeploymentModelPaths(model_less_pipeline,
                                          "/path/unused/by/model-less-pipeline",
                                          &resolved, &diagnostic))
      << diagnostic;
}

TEST_F(AdapterContractSecurityTest,
       InMemoryEntryResolvesArtifactRootBeforeCore) {
  const std::string config_path =
      GetConfigPath("demo/fixtures/mock/pipeline_doc_qa.json");
  std::ifstream config_stream(config_path);
  ASSERT_TRUE(config_stream.is_open());
  nlohmann::json pipeline_json;
  config_stream >> pipeline_json;
  ASSERT_EQ(pipeline_json["models"].size(), 2u);
  pipeline_json["models"][0]["model_path"] = "embedding.fixture";
  pipeline_json["models"][1]["model_path"] = "llm.fixture";

  const std::filesystem::path model_root =
      std::filesystem::weakly_canonical(GetConfigPath("models"));
  std::unique_ptr<ValidatedIoPlan> io_plan;
  std::string plan_err;
  ASSERT_EQ(IoBindingResolver::ResolveFromPipelineJson(
                pipeline_json, "doc_qa.operator.v1", "operator",
                model_root.string(), &io_plan, &plan_err),
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

  const auto embedding_registration =
      runtime->GetPipeline()
          ->GetSessionContext()
          .GetModelManager()
          .GetModelRegistration("embed_model_v1");
  const auto llm_registration = runtime->GetPipeline()
                                    ->GetSessionContext()
                                    .GetModelManager()
                                    .GetModelRegistration("llm_model_v1");
  ASSERT_TRUE(embedding_registration.has_value());
  ASSERT_TRUE(llm_registration.has_value());
  EXPECT_EQ(embedding_registration->resolved_model_path,
            std::filesystem::weakly_canonical(model_root / "embedding.fixture")
                .string());
  EXPECT_EQ(
      llm_registration->resolved_model_path,
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
  int ret = adapter.Unpack(inputs, 1, &ctx, &status);
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
  int bad_ret = adapter.Unpack(bad_inputs, 1, &bad_ctx, &bad_status);
  EXPECT_EQ(bad_ret, COMPANY_ALG_ERR_INVALID_INPUT);
}

// ---------------------------------------------------------------------------
// 4. COPY_IN 内存所有权深度隔离测试 (ADP-002, RECHECK-006)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, DirectUnpackMemoryIsolation) {
  const auto* input_conv = IoConverterRegistry::Instance().FindInputConverter(
      "keyword.plain.operator.v1");
  ASSERT_NE(input_conv, nullptr);

  // 创建动态可修改的原始缓冲区
  char caller_buf[256];
  snprintf(caller_buf, sizeof(caller_buf), "设备系统初始化自检正常");

  CompanyString cs{static_cast<int32_t>(std::strlen(caller_buf)), caller_buf};
  CompanyOperatorKeywordInput in_struct;
  in_struct.request_id = 9999;
  in_struct.sentence_text = &cs;

  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.leased_slots["keyword_in"] = {&in_struct};
  in_view.slot_types["keyword_in"] = "CompanyOperatorKeywordInput";
  InputPortBindings in_bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"input_sentences", "input_sentences"}});
  InputDecodeOptions in_options;
  in_options.converter_id = "keyword.plain.operator.v1";
  in_options.transport = "operator";

  AlgContext ctx;
  AdapterStatus status;
  int unpack_ret =
      input_conv->decode_fn(in_view, in_options, in_bindings, &ctx, &status);
  ASSERT_EQ(unpack_ret, COMPANY_ALG_SUCCESS);

  // 立即篡改调用方内存 Buffer (例如 memset 覆盖为 'X')
  std::memset(caller_buf, 'X', sizeof(caller_buf) - 1);
  caller_buf[sizeof(caller_buf) - 1] = '\0';

  // 验证 AlgContext 中的 DTO 保持原有数据完全不受外界内存修改影响 (物理深拷贝)
  auto* sentences = ctx.Read(kInputSentences);
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
// 6. Pipeline 绑定精确白名单与 Fail-Closed 校验 (RECHECK-002)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, PipelineBindingFailClosedAndExactMatch) {
  const auto* binding =
      IoBindingRegistry::Instance().FindBinding("keyword_match.operator.v1");
  ASSERT_NE(binding, nullptr);

  // 6.1 精确匹配成功
  EXPECT_EQ(binding->biz_name, "keyword_match_v1");
  EXPECT_EQ(binding->transport, "operator");

  // 6.2 旧 cabi / 包含子串的伪造名称 / 大小写不匹配 / 空白名称均严格拒绝
  // (Fail-Closed)
  EXPECT_EQ(IoBindingRegistry::Instance().FindBinding("keyword_match.cabi.v1"),
            nullptr);
  EXPECT_EQ(IoBindingRegistry::Instance().FindBinding("keyword_match_v1_fake"),
            nullptr);
  EXPECT_EQ(IoBindingRegistry::Instance().FindBinding("my_keyword_match_v1"),
            nullptr);
  EXPECT_EQ(IoBindingRegistry::Instance().FindBinding("KEYWORD_MATCH_V1"),
            nullptr);
  EXPECT_EQ(IoBindingRegistry::Instance().FindBinding(""), nullptr);

  // 6.3 Operator Create 阶段使用非法配置创建句柄立即失败 (-2)
  operator_api::CreateParam param{};
  param.model_path = "./models";
  param.cfg_file_name = "pipeline_dialogue_audit.json";
  param.device_id = 0;
  param.compute_platform = operator_api::ComputePlatform::kCpu;

  void* handle = nullptr;
  auto op = operator_api::Get_LLM_EDGEFLOW_OperatorTable();
  int create_ret = op.Create(&handle, &param);
  EXPECT_NE(create_ret, 0);
  EXPECT_EQ(handle, nullptr);
}

// ---------------------------------------------------------------------------
// 7. Registry 拒绝不支持的 Descriptor 策略组合 (RECHECK-003)
// ---------------------------------------------------------------------------
TEST_F(AdapterContractSecurityTest, RegistryRejectsUnsupportedPolicies) {
  InputConverterDefinition bad_def;
  bad_def.converter_id = "bad.converter.v1";
  bad_def.transport = "unsupported_transport";
  bad_def.decode_fn = [](const ExternalInputBatchView&,
                         const InputDecodeOptions&, const InputPortBindings&,
                         AlgContext*, AdapterStatus*) { return 0; };

  bool reg_ret =
      IoConverterRegistry::Instance().RegisterInputConverter(bad_def);
  EXPECT_FALSE(reg_ret);
  EXPECT_TRUE(IoConverterRegistry::Instance().HasConflict());
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
                                           &cs};

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

// RFC-0053 / RFC-0059: Cross-sample carrier error vs biz decode error priority
TEST_F(AdapterContractSecurityTest,
       TranslationCrossSampleCarrierVsBizErrorPriority) {
  const auto* converter = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(converter, nullptr);

  // Sample 0 has carrier error (oversized string)
  std::string oversized(64 * 1024 + 1, 'z');
  CompanyString cs_oversized{static_cast<int32_t>(oversized.size()),
                             const_cast<char*>(oversized.data())};
  CompanyOperatorEntityInput in_carrier{101, &cs_oversized};

  ExternalInputBatchView carrier_view;
  carrier_view.count = 1;
  carrier_view.leased_slots["entity_in"] = {&in_carrier};
  carrier_view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

  InputPortBindings bindings({{"raw_request_ids", "raw_request_ids"},
                              {"input_sentences", "input_sentences"}});
  InputDecodeOptions options;
  options.converter_id = "translate.json.operator.v1";
  options.transport = "operator";

  AlgContext carrier_ctx;
  AdapterStatus carrier_status;
  int ret = converter->decode_fn(carrier_view, options, bindings, &carrier_ctx,
                                 &carrier_status);
  EXPECT_EQ(ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(carrier_status.SampleIndex(), 0);
  EXPECT_EQ(carrier_status.FieldPath(), "sentence_text");

  // Pure biz decode error retains translate.json.operator.v1 converter name
  std::string bad_json = "{\"wrong_field\":123}";
  CompanyString cs_biz{static_cast<int32_t>(bad_json.size()),
                       const_cast<char*>(bad_json.data())};
  CompanyOperatorEntityInput in_biz{103, &cs_biz};
  ExternalInputBatchView biz_view;
  biz_view.count = 1;
  biz_view.leased_slots["entity_in"] = {&in_biz};
  biz_view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

  AlgContext biz_ctx;
  AdapterStatus biz_status;
  EXPECT_EQ(
      converter->decode_fn(biz_view, options, bindings, &biz_ctx, &biz_status),
      COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(biz_status.AdapterName(), "translate.json.operator.v1");
  EXPECT_EQ(biz_status.FieldPath(), "json");
}

// RFC-0053 / RFC-0059: Return code and AdapterStatus independence
TEST_F(AdapterContractSecurityTest,
       TranslationReturnCodeAndAdapterStatusIndependence) {
  const auto* converter = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(converter, nullptr);

  // AlgContext with raw_request_ids but missing answers
  AlgContext ctx;
  ctx.Publish(kRawRequestIds, std::vector<uint64_t>{1001});

  CompanyOperatorEntityOutput out{};
  ExternalOutputBatchView view;
  view.count = 1;
  view.leased_slots["entity_out"] = {&out};
  view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";

  OutputPortBindings bindings(
      {{"raw_request_ids", "raw_request_ids"}, {"llm_answers", "llm_answers"}});
  OutputEncodeOptions options;
  options.converter_id = "translate.json.operator.v1";
  options.transport = "operator";

  size_t written = 0;
  AdapterStatus status;
  int ret =
      converter->encode_fn(&ctx, bindings, options, &view, &written, &status);

  // Underlying reader wrote BUFFER_TOO_SMALL (-4) into AdapterStatus
  EXPECT_EQ(ret, COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
}

// RFC-0053 / RFC-0059: Translate serialization failure (invalid UTF-8) priority
// over capacity check
TEST_F(AdapterContractSecurityTest,
       TranslationSerializationFailurePriorityOverCapacity) {
  const auto* converter = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(converter, nullptr);

  // AlgContext with valid raw_req_ids, but answer has invalid UTF-8 byte
  // sequence
  AlgContext ctx;
  ctx.Publish(kRawRequestIds, std::vector<uint64_t>{1001});
  std::string invalid_utf8 = "prefix\xFF\xFFsuffix";
  ctx.Publish(kLlmAnswers, TextBatch{{0, 0, invalid_utf8}});

  CompanyOperatorEntityOutput out{};
  ExternalOutputBatchView view;
  view.count = 1;
  view.leased_slots["entity_out"] = {&out};
  view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";

  OutputPortBindings bindings(
      {{"raw_request_ids", "raw_request_ids"}, {"llm_answers", "llm_answers"}});
  OutputEncodeOptions options;
  options.converter_id = "translate.json.operator.v1";
  options.transport = "operator";

  size_t written = 0;
  AdapterStatus status;

  // Serialization in Encode precedes capacity validation;
  // unhandled dump exception propagates out of encode_fn
  EXPECT_THROW(
      converter->encode_fn(&ctx, bindings, options, &view, &written, &status),
      std::exception);
}

// RFC-0053 / RFC-0059: Translate null AlgContext diagnostics
TEST_F(AdapterContractSecurityTest, TranslateNullContextDiagnostics) {
  const auto* in_conv = IoConverterRegistry::Instance().FindInputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(in_conv, nullptr);
  const auto* out_conv = IoConverterRegistry::Instance().FindOutputConverter(
      "translate.json.operator.v1");
  ASSERT_NE(out_conv, nullptr);

  // 1. Decode with null context: must return INVALID_INPUT (-3) with field
  // "context"
  std::string query_json = "{\"query\":\"test\"}";
  CompanyString cs{static_cast<int32_t>(query_json.size()),
                   const_cast<char*>(query_json.data())};
  CompanyOperatorEntityInput input{100, &cs};
  ExternalInputBatchView in_view;
  in_view.count = 1;
  in_view.leased_slots["entity_in"] = {&input};
  in_view.slot_types["entity_in"] = "CompanyOperatorEntityInput";

  InputPortBindings in_bindings({{"raw_request_ids", "raw_request_ids"},
                                 {"input_sentences", "input_sentences"}});
  InputDecodeOptions in_options;
  in_options.converter_id = "translate.json.operator.v1";
  in_options.transport = "operator";

  AdapterStatus unpack_status;
  int unpack_ret = in_conv->decode_fn(in_view, in_options, in_bindings, nullptr,
                                      &unpack_status);
  EXPECT_EQ(unpack_ret, COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(unpack_status.Code(), COMPANY_ALG_ERR_INVALID_INPUT);
  EXPECT_EQ(unpack_status.FieldPath(), "context");
  EXPECT_EQ(unpack_status.AdapterName(), "translate.json.operator.v1");

  EXPECT_EQ(
      in_conv->decode_fn(in_view, in_options, in_bindings, nullptr, nullptr),
      COMPANY_ALG_ERR_INVALID_INPUT);

  // 2. Encode with null context: must return BUFFER_TOO_SMALL (-4) with field
  // "context"
  CompanyOperatorEntityOutput output{};
  ExternalOutputBatchView out_view;
  out_view.count = 1;
  out_view.leased_slots["entity_out"] = {&output};
  out_view.slot_types["entity_out"] = "CompanyOperatorEntityOutput";

  OutputPortBindings out_bindings(
      {{"raw_request_ids", "raw_request_ids"}, {"llm_answers", "llm_answers"}});
  OutputEncodeOptions out_options;
  out_options.converter_id = "translate.json.operator.v1";
  out_options.transport = "operator";

  size_t written = 0;
  AdapterStatus pack_status;
  int pack_ret = out_conv->encode_fn(nullptr, out_bindings, out_options,
                                     &out_view, &written, &pack_status);
  EXPECT_EQ(pack_ret, COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(pack_status.Code(), COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
  EXPECT_EQ(pack_status.FieldPath(), "context");
  EXPECT_EQ(pack_status.AdapterName(), "translate.json.operator.v1");

  EXPECT_EQ(out_conv->encode_fn(nullptr, out_bindings, out_options, &out_view,
                                &written, nullptr),
            COMPANY_ALG_ERR_BUFFER_TOO_SMALL);
}

}  // namespace llm_edgeflow
