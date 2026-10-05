#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "adapter/converter_authoring.h"
#include "adapter/deployment_diagnostic.h"
#include "adapter/deployment_io_config.h"
#include "adapter/deployment_preparation.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_binding_resolver.h"
#include "adapter/io_catalog.h"
#include "adapter/io_converter_registry.h"
#include "adapter/pipeline_document.h"
#include "adapter/shared_algorithm_runtime.h"
#include "contracts/registry_conflicts.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_config.h"
#include "edgeflow/operator/interface.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {
namespace {

namespace fs = std::filesystem;

int dummy_decode_calls = 0;
int dummy_encode_calls = 0;

int DummyDecode(const ExternalInputBatchView&, const InputDecodeOptions&,
                AlgContext*, AdapterStatus*) {
  ++dummy_decode_calls;
  return 0;
}

int DummyEncode(AlgContext*, const OutputEncodeOptions&,
                ExternalOutputBatchView*, size_t* written_count,
                AdapterStatus*) {
  ++dummy_encode_calls;
  if (written_count) *written_count = 1;
  return 0;
}

nlohmann::json DefaultPipelineNodes() {
  nlohmann::json node;
  node["id"] = "node_0";
  node["node_type"] = "TextRuleMatchNode";
  node["depends_on"] = nlohmann::json::array();
  node["inputs"]["text"] = "input_sentences";
  node["outputs"]["matches"] = "llm_answers";
  node["config"]["categories"]["CAT"] = nlohmann::json::array({"word"});
  return nlohmann::json::array({node});
}

}  // namespace

class IoBindingRegistryTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    saved_bindings_ = IoBindingRegistry::Instance().AllBindings();
    saved_inputs_ = IoConverterRegistry::Instance().AllInputConverters();
    saved_outputs_ = IoConverterRegistry::Instance().AllOutputConverters();
  }

  static void TearDownTestSuite() {
    IoConverterRegistry::Instance().ClearForTesting();
    IoBindingRegistry::Instance().ClearForTesting();
    for (const auto& in_def : saved_inputs_) {
      IoConverterRegistry::Instance().RegisterInputConverter(in_def);
    }
    for (const auto& out_def : saved_outputs_) {
      IoConverterRegistry::Instance().RegisterOutputConverter(out_def);
    }
    for (const auto& binding : saved_bindings_) {
      IoBindingRegistry::Instance().RegisterBinding(binding);
    }
  }

  void SetUp() override {
    IoConverterRegistry::Instance().ClearForTesting();
    IoBindingRegistry::Instance().ClearForTesting();

    // 注册基础转换器供测试
    InputConverterDefinition in_def;
    in_def.converter_id = "test.in.operator";

    in_def.schema_id = "in_schema";
    in_def.external_slots = {
        ExternalSlotDefinition("entity_in", "CompanyOperatorEntityInput",
                               PortDirection::kInput, true, "entity_in")};
    in_def.max_batch_size = 64;
    in_def.logical_ports = {
        NodePortDefinition("input_sentences", "TextBatch", true, "1:1")};
    in_def.decode_fn = &DummyDecode;
    IoConverterRegistry::Instance().RegisterInputConverter(in_def);

    OutputConverterDefinition out_def;
    out_def.converter_id = "test.out.operator";

    out_def.schema_id = "out_schema";
    out_def.external_slots = {
        ExternalSlotDefinition("entity_out", "CompanyOperatorEntityOutput",
                               PortDirection::kOutput, true, "entity_out")};
    out_def.max_batch_size = 64;
    out_def.logical_ports = {
        NodePortDefinition("llm_answers", "TextBatch", true, "1:1")};
    out_def.encode_fn = &DummyEncode;
    IoConverterRegistry::Instance().RegisterOutputConverter(out_def);

    // 注册业务契约
    BizDefinition biz;
    biz.biz_name = "test_biz";
    biz.ingress = {BizPortDefinition("input_sentences", "TextBatch", true)};
    biz.egress = {BizPortDefinition("llm_answers", "TextBatch", true)};
    PipelineCatalog::RegisterBizDefinition(biz);
  }

  void TearDown() override {
    IoConverterRegistry::Instance().ClearForTesting();
    IoBindingRegistry::Instance().ClearForTesting();
  }

  void RegisterTestBizBinding(size_t max_batch_size = 0) {
    IoBindingDefinition binding;
    binding.biz_name = "test_biz";

    binding.input_converter_id = "test.in.operator";
    binding.output_converter_id = "test.out.operator";
    binding.max_batch_size = max_batch_size;
    ASSERT_TRUE(IoBindingRegistry::Instance().RegisterBinding(binding));
  }

  // 端口 texts 不在业务契约中，必需的 input_sentences 也无人覆盖。
  std::string RegisterUnmatchedInputConverter() {
    auto converter =
        *IoConverterRegistry::Instance().FindInputConverter("test.in.operator");
    converter.converter_id = "unmatched.in.operator";
    converter.logical_ports = {
        NodePortDefinition("texts", "TextBatch", true, "1:1")};
    EXPECT_TRUE(
        IoConverterRegistry::Instance().RegisterInputConverter(converter));
    return converter.converter_id;
  }

  static inline std::vector<IoBindingDefinition> saved_bindings_;
  static inline std::vector<InputConverterDefinition> saved_inputs_;
  static inline std::vector<OutputConverterDefinition> saved_outputs_;
};

TEST_F(IoBindingRegistryTest, RegistryConflictsStayRecordedWithoutMessage) {
  RegistryConflicts conflicts;
  EXPECT_FALSE(conflicts.HasConflict());
  EXPECT_TRUE(conflicts.Messages().empty());
  conflicts.Record("duplicate");
  EXPECT_TRUE(conflicts.HasConflict());
  EXPECT_EQ(conflicts.Messages(), std::vector<std::string>{"duplicate"});
  conflicts.Clear();
  EXPECT_FALSE(conflicts.HasConflict());

  // 因分配失败丢失消息时，注册表不得重新开放。
  RegistryConflicts lossy;
  std::string message = "lost";
  bool allocation_failed = false;
  {
    test_support::ScopedAllocationFailure failure(0);
    lossy.Record(std::move(message));
    allocation_failed = failure.Triggered();
  }
  EXPECT_TRUE(allocation_failed);
  EXPECT_TRUE(lossy.HasConflict());
  ASSERT_EQ(lossy.Messages().size(), 1u);
  EXPECT_NE(lossy.Messages()[0].find("without a stored message"),
            std::string::npos);
}

TEST_F(IoBindingRegistryTest, RegisterAndAuditValidBinding) {
  auto& reg = IoBindingRegistry::Instance();

  IoBindingDefinition binding;
  binding.biz_name = "test_biz";

  binding.input_converter_id = "test.in.operator";
  binding.output_converter_id = "test.out.operator";

  EXPECT_TRUE(reg.RegisterBinding(binding));

  std::vector<std::string> audit_errors;
  EXPECT_TRUE(reg.Audit(&audit_errors));
  EXPECT_TRUE(audit_errors.empty());
}

TEST_F(IoBindingRegistryTest, AuditRejectsUnregisteredConvertersAndBiz) {
  auto& reg = IoBindingRegistry::Instance();

  // 1. 引用不存在的 biz_name
  IoBindingDefinition bad_biz;
  bad_biz.biz_name = "non_existent_biz";

  bad_biz.input_converter_id = "test.in.operator";
  bad_biz.output_converter_id = "test.out.operator";
  reg.RegisterBinding(bad_biz);

  std::vector<std::string> errors;
  EXPECT_FALSE(reg.Audit(&errors));
  bool found_unregistered_biz = false;
  for (const auto& e : errors) {
    if (e.find("unregistered biz_name") != std::string::npos) {
      found_unregistered_biz = true;
    }
  }
  EXPECT_TRUE(found_unregistered_biz);

  reg.ClearForTesting();

  // 2. 引用不存在的转换器
  IoBindingDefinition bad_conv;
  bad_conv.biz_name = "test_biz";

  bad_conv.input_converter_id = "non_existent_input";
  bad_conv.output_converter_id = "test.out.operator";
  reg.RegisterBinding(bad_conv);

  errors.clear();
  EXPECT_FALSE(reg.Audit(&errors));
  bool found_unregistered_conv = false;
  for (const auto& e : errors) {
    if (e.find("unregistered input_converter") != std::string::npos) {
      found_unregistered_conv = true;
    }
  }
  EXPECT_TRUE(found_unregistered_conv);
}

TEST_F(IoBindingRegistryTest,
       AuditAndPreparationRejectBindingWithoutBatchLimit) {
  auto input =
      *IoConverterRegistry::Instance().FindInputConverter("test.in.operator");
  auto output =
      *IoConverterRegistry::Instance().FindOutputConverter("test.out.operator");
  IoConverterRegistry::Instance().ClearForTesting();
  input.max_batch_size = 0;
  output.max_batch_size = 0;
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(input));
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterOutputConverter(output));
  RegisterTestBizBinding(0);

  std::vector<std::string> errors;
  EXPECT_FALSE(IoBindingRegistry::Instance().Audit(&errors));
  EXPECT_TRUE(std::any_of(errors.begin(), errors.end(), [](const auto& e) {
    return e.find("declares no batch limit") != std::string::npos;
  }));

  const nlohmann::json document = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"pipeline", DefaultPipelineNodes()}};
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diagnostic.path, "/deployment/io/io_binding");
}

TEST_F(IoBindingRegistryTest, BindingWithoutExplicitLimitUsesStandardDefault) {
  auto input =
      *IoConverterRegistry::Instance().FindInputConverter("test.in.operator");
  auto output =
      *IoConverterRegistry::Instance().FindOutputConverter("test.out.operator");
  IoConverterRegistry::Instance().ClearForTesting();
  input.max_batch_size = 0;
  output.max_batch_size = 0;
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(input));
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterOutputConverter(output));
  IoBindingDefinition binding;
  binding.biz_name = "test_biz";
  binding.input_converter_id = input.converter_id;
  binding.output_converter_id = output.converter_id;
  ASSERT_TRUE(IoBindingRegistry::Instance().RegisterBinding(binding));
  std::vector<std::string> errors;
  EXPECT_TRUE(IoBindingRegistry::Instance().Audit(&errors));
  EXPECT_TRUE(errors.empty());
  const nlohmann::json document = {
      {"deployment", {{"io", {{"io_binding", binding.biz_name}}}}},
      {"pipeline", DefaultPipelineNodes()}};
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  EXPECT_EQ(prepared.effective_max_batch_size, 64U);
}

TEST_F(IoBindingRegistryTest, CapacityFieldsDeriveFromValueType) {
  auto input =
      *IoConverterRegistry::Instance().FindInputConverter("test.in.operator");
  auto output =
      *IoConverterRegistry::Instance().FindOutputConverter("test.out.operator");
  IoConverterRegistry::Instance().ClearForTesting();
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(input));
  // 保留夹具的逻辑契约，并使用已注册的 keyword 布局。
  output.external_slots = {
      ExternalOutputSlot<CompanyOperatorKeywordOutput>("keyword_out")};
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterOutputConverter(output));
  const auto catalog = IoCatalog::ToJson();
  ASSERT_EQ(catalog["output_converters"].size(), 1U);
  EXPECT_EQ(
      catalog["output_converters"][0]["external_slots"][0]["capacity_fields"],
      nlohmann::json::array({"match_result_json"}));
  const auto inferred = output.external_slots[0];
  EXPECT_EQ(EffectiveCapacityFields(inferred),
            std::vector<std::string>{"match_result_json"});
  auto input_slot = inferred;
  input_slot.direction = PortDirection::kInput;
  EXPECT_TRUE(EffectiveCapacityFields(input_slot).empty());
  auto unknown = inferred;
  unknown.type_suffix = "unregistered";
  EXPECT_TRUE(EffectiveCapacityFields(unknown).empty());
}

TEST_F(IoBindingRegistryTest, UnselectedIllegalBindingFailsAudit) {
  auto& reg = IoBindingRegistry::Instance();

  // 1. 注册合法绑定
  IoBindingDefinition valid_binding;
  valid_binding.biz_name = "test_biz";

  valid_binding.input_converter_id = "test.in.operator";
  valid_binding.output_converter_id = "test.out.operator";
  EXPECT_TRUE(reg.RegisterBinding(valid_binding));

  // 单独 audit 合法绑定应当通过
  std::vector<std::string> errors;
  EXPECT_TRUE(reg.Audit(&errors));
  EXPECT_TRUE(errors.empty());

  // 2. 为另一业务注册未被选用的非法绑定：输入端口不在业务契约中
  auto unselected_biz = *PipelineCatalog::FindBiz("test_biz");
  unselected_biz.biz_name = "unselected_biz";
  ASSERT_TRUE(PipelineCatalog::RegisterBizDefinition(unselected_biz));
  IoBindingDefinition illegal_binding;
  illegal_binding.biz_name = unselected_biz.biz_name;

  illegal_binding.input_converter_id = RegisterUnmatchedInputConverter();
  illegal_binding.output_converter_id = "test.out.operator";
  EXPECT_TRUE(reg.RegisterBinding(illegal_binding));

  // 全量审计必须被这个未被选中的非法绑定阻断
  errors.clear();
  EXPECT_FALSE(reg.Audit(&errors));
  bool found_missing_ingress = false;
  for (const auto& e : errors) {
    if (e.find("missing required biz ingress port: input_sentences") !=
        std::string::npos) {
      found_missing_ingress = true;
    }
  }
  EXPECT_TRUE(found_missing_ingress);

  // 验证 SharedAlgorithmRuntime::GlobalInit() 也会因为 Audit 失败而返回冲突错误
  // (-6)
  EXPECT_EQ(SharedAlgorithmRuntime::GlobalInit(),
            COMPANY_ALG_ERR_REGISTRY_CONFLICT);
}

TEST_F(IoBindingRegistryTest, DeploymentIoConfigValidation) {
  // 1. 合法 Operator 定位配置 (仅包含 pipe_path)
  nlohmann::json valid_cfg = {{"pipe_path", "test.json"}};

  // 写入临时测试 pipeline 文件
  std::string tmp_dir = "/tmp/edgeflow_test_config_" + std::to_string(getpid());
  fs::create_directories(tmp_dir);
  std::string pipe_path = tmp_dir + "/test.json";
  {
    std::ofstream ofs(pipe_path);
    ofs << "{}";
  }

  DeploymentIoConfig parsed;
  std::string err;
  EXPECT_TRUE(DeploymentIoConfig::Parse(valid_cfg, tmp_dir, &parsed, &err));
  EXPECT_EQ(parsed.pipe_path, "test.json");

  // 2. 拒绝旧 Schema 1 包装 (schema_version + data)
  nlohmann::json old_schema1 = {
      {"schema_version", 1},
      {"data", {{"pipe_path", "test.json"}, {"io_binding", "test_biz"}}}};
  EXPECT_FALSE(DeploymentIoConfig::Parse(old_schema1, tmp_dir, &parsed, &err));
  EXPECT_NE(err.find("Unknown field"), std::string::npos);

  // 3. 拒绝顶层未知字段
  nlohmann::json bad_field = valid_cfg;
  bad_field["extra_field"] = "foo";
  EXPECT_FALSE(DeploymentIoConfig::Parse(bad_field, tmp_dir, &parsed, &err));

  // 5. 路径逃逸拒绝
  nlohmann::json escape_cfg = {{"pipe_path", "../../../etc/passwd"}};
  EXPECT_FALSE(DeploymentIoConfig::Parse(escape_cfg, tmp_dir, &parsed, &err));

  // 6. JSON Pointer 转义未知键 (例如 "bad~/field" -> "/bad~0~1field")
  nlohmann::json escaped_key_cfg = valid_cfg;
  escaped_key_cfg["bad~/field"] = 1;
  DeploymentDiagnostic diag;
  EXPECT_FALSE(DeploymentIoConfig::Parse(escaped_key_cfg, tmp_dir, &parsed,
                                         &err, &diag));
  EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diag.path, "/bad~0~1field");

  // 7. ReadFromFile 在解析错误时携带配置文件路径上下文
  const std::string bad_conf_path = tmp_dir + "/bad_config.conf";
  {
    std::ofstream ofs(bad_conf_path);
    ofs << escaped_key_cfg.dump();
  }
  std::string read_err;
  DeploymentDiagnostic read_diag;
  EXPECT_FALSE(DeploymentIoConfig::ReadFromFile(bad_conf_path, &parsed,
                                                &read_err, &read_diag));
  EXPECT_EQ(read_diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(read_diag.path, "/bad~0~1field");
  EXPECT_EQ(read_err.rfind("Error in config file " + bad_conf_path + ": ", 0),
            0);
  EXPECT_EQ(read_diag.message.rfind(
                "Error in config file " + bad_conf_path + ": ", 0),
            0);

  // 8. ReadFromFile 针对旧 Schema 1 报错同样携带配置文件路径上下文
  const std::string dep_conf_path = tmp_dir + "/deprecated.conf";
  {
    std::ofstream ofs(dep_conf_path);
    ofs << old_schema1.dump();
  }
  EXPECT_FALSE(DeploymentIoConfig::ReadFromFile(dep_conf_path, &parsed,
                                                &read_err, &read_diag));
  EXPECT_EQ(read_diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(read_err.rfind("Error in config file " + dep_conf_path + ": ", 0),
            0);
  EXPECT_EQ(read_diag.message.rfind(
                "Error in config file " + dep_conf_path + ": ", 0),
            0);

  // 9. ReadFromFile 文件路径包含关键保留词 (如 pipe_path.conf)
  // 时，仍必须正确携带文件路径前缀
  const std::string keyword_conf_path = tmp_dir + "/pipe_path.conf";
  {
    std::ofstream ofs(keyword_conf_path);
    ofs << escaped_key_cfg.dump();
  }
  EXPECT_FALSE(DeploymentIoConfig::ReadFromFile(keyword_conf_path, &parsed,
                                                &read_err, &read_diag));
  EXPECT_EQ(read_diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(read_diag.path, "/bad~0~1field");
  EXPECT_EQ(
      read_err.rfind("Error in config file " + keyword_conf_path + ": ", 0), 0);
  EXPECT_EQ(read_diag.message.rfind(
                "Error in config file " + keyword_conf_path + ": ", 0),
            0);

  // 10. 多个 ~ 与 / 字符的转义校验
  nlohmann::json multi_escape_cfg = valid_cfg;
  multi_escape_cfg["a~b/c~0/d~1"] = 42;
  EXPECT_FALSE(DeploymentIoConfig::Parse(multi_escape_cfg, tmp_dir, &parsed,
                                         &err, &diag));
  EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diag.path, "/a~0b~1c~00~1d~01");

  fs::remove_all(tmp_dir);
}

TEST_F(IoBindingRegistryTest, StrictConfigDirectoryIsolationAndCwdInvariance) {
  const std::string root_dir =
      "/tmp/edgeflow_test_isolation_" + std::to_string(getpid());
  fs::remove_all(root_dir);

  const fs::path base_dir = fs::path(root_dir) / "service_configs";
  const fs::path outside_dir = fs::path(root_dir) / "outside";
  const fs::path sibling_dir = fs::path(root_dir) / "sibling";
  const fs::path sub_dir = base_dir / "subdir";

  fs::create_directories(base_dir);
  fs::create_directories(outside_dir);
  fs::create_directories(sibling_dir);
  fs::create_directories(sub_dir);

  // 准备各个目标文件
  {
    std::ofstream(base_dir / "pipeline.json") << "{}";
    std::ofstream(sub_dir / "sub_pipeline.json") << "{}";
    std::ofstream(outside_dir / "outside_pipeline.json") << "{}";
    std::ofstream(sibling_dir / "sibling_pipeline.json") << "{}";
  }

  // 创建指向根外文件的符号链接
  std::error_code ec;
  fs::create_symlink(outside_dir / "outside_pipeline.json",
                     base_dir / "symlink_escape.json", ec);
  ASSERT_FALSE(ec) << ec.message();

  DeploymentIoConfig parsed;
  std::string err;

  auto make_conf = [](const std::string& pipe) {
    nlohmann::json cfg = {{"pipe_path", pipe}};
    return cfg;
  };

  // 1. 同级文件 -> 成功
  EXPECT_TRUE(DeploymentIoConfig::Parse(make_conf("pipeline.json"),
                                        base_dir.string(), &parsed, &err));
  EXPECT_EQ(parsed.resolved_pipe_path,
            fs::canonical(base_dir / "pipeline.json").string());

  // 2. 子目录文件 -> 成功
  EXPECT_TRUE(DeploymentIoConfig::Parse(make_conf("subdir/sub_pipeline.json"),
                                        base_dir.string(), &parsed, &err));
  EXPECT_EQ(parsed.resolved_pipe_path,
            fs::canonical(sub_dir / "sub_pipeline.json").string());

  // 3. 父目录逃逸 (../outside/outside_pipeline.json) -> 严格拒绝
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(make_conf("../outside/outside_pipeline.json"),
                                base_dir.string(), &parsed, &err));
  EXPECT_NE(err.find("escapes config directory"), std::string::npos);

  // 4. 兄弟目录逃逸 (../sibling/sibling_pipeline.json) -> 严格拒绝
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(make_conf("../sibling/sibling_pipeline.json"),
                                base_dir.string(), &parsed, &err));
  EXPECT_NE(err.find("escapes config directory"), std::string::npos);

  // 5. 符号链接逃逸 (位于 base_dir 内但指向根外) -> 严格拒绝
  EXPECT_FALSE(DeploymentIoConfig::Parse(make_conf("symlink_escape.json"),
                                         base_dir.string(), &parsed, &err));
  EXPECT_NE(err.find("escapes config directory"), std::string::npos);

  // 6. 切换工作目录不改变解析结果 (Cwd Invariance)
  const fs::path conf_file = base_dir / "deploy.conf";
  {
    std::ofstream ofs(conf_file);
    ofs << make_conf("pipeline.json").dump();
  }

  const fs::path orig_cwd = fs::current_path();
  // 切换工作目录到 outside_dir
  fs::current_path(outside_dir, ec);
  ASSERT_FALSE(ec);

  DeploymentIoConfig cwd_parsed;
  std::string cwd_err;
  bool read_ok = DeploymentIoConfig::ReadFromFile(conf_file.string(),
                                                  &cwd_parsed, &cwd_err);

  // 恢复原工作目录
  fs::current_path(orig_cwd, ec);
  ASSERT_FALSE(ec);

  EXPECT_TRUE(read_ok) << cwd_err;
  EXPECT_EQ(cwd_parsed.resolved_pipe_path,
            fs::canonical(base_dir / "pipeline.json").string());

  fs::remove_all(root_dir);
}

TEST_F(IoBindingRegistryTest, SplitPipelineDocumentAndCoreBoundary) {
  RegisterTestBizBinding();
  nlohmann::json document = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"models", nlohmann::json::array()},
      {"max_parallel_workers", 2},
      {"pipeline", DefaultPipelineNodes()}};
  PipelineDocumentSplit split;
  std::string error;
  ASSERT_TRUE(SplitPipelineDocument(document, &split, &error)) << error;
  EXPECT_EQ(split.deployment.io.io_binding, "test_biz");
  EXPECT_EQ(split.deployment.io.out_mem, nlohmann::json::object());
  EXPECT_FALSE(split.neutral_pipeline_json.contains("deployment"));
  EXPECT_FALSE(split.neutral_pipeline_json.contains("biz_name"));
  EXPECT_EQ(split.neutral_pipeline_json["models"], document["models"]);
  EXPECT_EQ(split.neutral_pipeline_json["max_parallel_workers"], 2);
  EXPECT_EQ(split.neutral_pipeline_json["pipeline"], document["pipeline"]);

  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  EXPECT_EQ(prepared.neutral_pipeline_json["biz_name"], "test_biz");
  EXPECT_FALSE(document.contains("biz_name"));
  ParsedPipelineConfig parsed;
  PipelineDiagnostic core_diagnostic;
  EXPECT_FALSE(ParsePipelineConfig(document, &parsed, &core_diagnostic));
  EXPECT_EQ(core_diagnostic.code, DiagnosticCode::kUnknownField);
  EXPECT_EQ(core_diagnostic.path, "/deployment");
  EXPECT_TRUE(ParsePipelineConfig(prepared.neutral_pipeline_json, &parsed,
                                  &core_diagnostic));
  EXPECT_EQ(parsed.biz_name, "test_biz");
  EXPECT_EQ(parsed.max_parallel_workers, 2);

  for (const char* field : {"unknown_key", "model_path"}) {
    auto invalid = document;
    invalid["deployment"][field] = "invalid";
    EXPECT_FALSE(SplitPipelineDocument(invalid, &split, &error));
    EXPECT_NE(error.find("Unknown field at /deployment/"), std::string::npos);
  }
  auto invalid = document;
  invalid["deployment"]["io"]["extra_field"] = true;
  EXPECT_FALSE(SplitPipelineDocument(invalid, &split, &error));
  invalid = document;
  invalid["deployment"]["io"]["io_binding"] = "";
  EXPECT_FALSE(SplitPipelineDocument(invalid, &split, &error));

  auto typo = document;
  typo["deploymen"] = nlohmann::json::object();
  EXPECT_FALSE(PrepareDeploymentDocument(typo, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diagnostic.path, "/deploymen");
}

TEST_F(IoBindingRegistryTest,
       UnknownExternalRootFieldsAreRejectedWithEscapedPointers) {
  RegisterTestBizBinding();
  const nlohmann::json valid_document = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"pipeline", DefaultPipelineNodes()}};
  struct UnknownRootField {
    const char* name;
    const char* pointer;
  };
  const UnknownRootField fields[] = {
      {"biz_name", "/biz_name"},
      {"execution_mode", "/execution_mode"},
      {"arbitrary_typo", "/arbitrary_typo"},
      {"unknown~field/name", "/unknown~0field~1name"},
      {"", "/"}};
  for (const auto& field : fields) {
    SCOPED_TRACE(field.name);
    auto document = valid_document;
    document[field.name] = "test_biz";
    const std::string expected_message =
        std::string("Unknown field at ") + field.pointer;
    PipelineDocumentSplit split;
    std::string error;
    std::string error_path;
    EXPECT_FALSE(SplitPipelineDocument(document, &split, &error, &error_path));
    EXPECT_EQ(error_path, field.pointer);
    EXPECT_NE(error.find(expected_message), std::string::npos);

    std::unique_ptr<ValidatedIoPlan> plan;
    DeploymentDiagnostic diagnostic;
    EXPECT_EQ(IoBindingResolver::ResolveFromPipelineJson(
                  document, "./models", &plan, &error, &diagnostic),
              -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diagnostic.path, field.pointer);
    EXPECT_NE(error.find(expected_message), std::string::npos);
    EXPECT_EQ(document[field.name], "test_biz");
  }
}

TEST_F(IoBindingRegistryTest, EscapedJsonPointerInOutputSlots) {
  RegisterTestBizBinding();
  nlohmann::json slot_doc = {
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz"},
          {"out_mem", {{"slot~0/bad", nlohmann::json::object()}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", DefaultPipelineNodes()}};

  std::unique_ptr<ValidatedIoPlan> plan;
  std::string err;
  DeploymentDiagnostic diagnostic;
  EXPECT_EQ(IoBindingResolver::ResolveFromPipelineJson(
                slot_doc, "./models", &plan, &err, &diagnostic),
            -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diagnostic.path, "/deployment/io/out_mem/slot~00~1bad");
  EXPECT_NE(err.find(diagnostic.path), std::string::npos) << err;
}

TEST_F(IoBindingRegistryTest, ModelPathResolutionFailurePointsToModelEntry) {
  RegisterTestBizBinding();
  nlohmann::json doc = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"models",
       {{{"model_id", "mid~test/path"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()},
         {"model_path", "../../escaped_model.bin"}}}},
      {"pipeline", DefaultPipelineNodes()}};
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string err;
  DeploymentDiagnostic diagnostic;
  EXPECT_EQ(IoBindingResolver::ResolveFromPipelineJson(doc, "./models", &plan,
                                                       &err, &diagnostic),
            -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diagnostic.code, "INVALID_MODEL_PATH");
  EXPECT_EQ(diagnostic.path, "/models/0/model_path");
  EXPECT_NE(err.find(diagnostic.path), std::string::npos) << err;
}

TEST_F(IoBindingRegistryTest,
       PrepareDeploymentSuccessAndBoundaryExtraction_T01) {
  RegisterTestBizBinding();
  nlohmann::json doc = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"models",
       {{{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()},
         {"model_path", "models/first.bin"}},
        {{"model_id", "mid~2/path"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()},
         {"model_path", "models/second.bin"}}}},
      {"pipeline", DefaultPipelineNodes()}};
  const auto original = doc;
  DeploymentPrepareOptions options;
  options.path_mode = DeploymentPathMode::kLexicalOnly;
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  ASSERT_TRUE(PrepareDeploymentDocument(doc, options, &prepared, &diag))
      << diag.message;
  EXPECT_FALSE(prepared.neutral_pipeline_json.contains("deployment"));
  EXPECT_EQ(prepared.neutral_pipeline_json["biz_name"], "test_biz");
  EXPECT_EQ(prepared.neutral_pipeline_json["models"], doc["models"]);
  EXPECT_EQ(prepared.binding.biz_name, "test_biz");
  ASSERT_NE(prepared.input_converter, nullptr);
  ASSERT_NE(prepared.output_converter, nullptr);
  EXPECT_EQ(prepared.output_specs.count("entity_out"), 1u);
  ASSERT_EQ(prepared.io_boundary.input_published_ports.size(), 1u);
  EXPECT_EQ(prepared.io_boundary.input_published_ports[0].Name(),
            "input_sentences");
  ASSERT_EQ(prepared.io_boundary.output_consumed_ports.size(), 1u);
  EXPECT_EQ(prepared.io_boundary.output_consumed_ports[0].Name(),
            "llm_answers");
  EXPECT_EQ(doc, original);

  doc["models"][0]["model_id"] = "renamed_first";
  doc["models"][0]["model_path"] = "models/changed.bin";
  ASSERT_TRUE(PrepareDeploymentDocument(doc, options, &prepared, &diag))
      << diag.message;
  EXPECT_EQ(prepared.neutral_pipeline_json["models"], doc["models"]);
}

TEST_F(IoBindingRegistryTest,
       ExternalBindingIsRequiredWhileCoreUsesNeutralBusiness) {
  RegisterTestBizBinding();
  const nlohmann::json neutral = {{"biz_name", "test_biz"},
                                  {"pipeline", DefaultPipelineNodes()}};
  ParsedPipelineConfig parsed;
  PipelineDiagnostic core_diagnostic;
  EXPECT_TRUE(ParsePipelineConfig(neutral, &parsed, &core_diagnostic));
  auto external = neutral;
  external.erase("biz_name");
  for (int variant = 0; variant < 3; ++variant) {
    SCOPED_TRACE(variant);
    auto document = external;
    if (variant == 1) document["deployment"] = nlohmann::json::object();
    if (variant == 2) document["deployment"]["io"] = nlohmann::json::object();
    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(
        PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
    EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diagnostic.path, variant == 0   ? "/deployment"
                               : variant == 1 ? "/deployment/io"
                                              : "/deployment/io/io_binding");
    EXPECT_TRUE(prepared.binding.biz_name.empty());
  }
}

TEST_F(IoBindingRegistryTest, ModelPathMissingEmptyOrWrongTypeRejected_T03) {
  RegisterTestBizBinding();
  const nlohmann::json base_doc = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"models",
       {{{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()}}}},
      {"pipeline", DefaultPipelineNodes()}};
  DeploymentPrepareOptions options;
  options.path_mode = DeploymentPathMode::kLexicalOnly;
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  EXPECT_FALSE(PrepareDeploymentDocument(base_doc, options, &prepared, &diag));
  EXPECT_EQ(diag.code, "MISSING_FIELD");
  EXPECT_EQ(diag.path, "/models/0/model_path");
  for (const auto& value :
       nlohmann::json::array({nullptr, 12345, true, nlohmann::json::object(),
                              nlohmann::json::array(), ""})) {
    SCOPED_TRACE(value.dump());
    auto doc = base_doc;
    doc["models"][0]["model_path"] = value;
    const auto original = doc;
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, value.is_string() ? "FIELD_RANGE" : "FIELD_TYPE");
    EXPECT_EQ(diag.path, "/models/0/model_path");
    EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
    EXPECT_EQ(doc, original);
  }
}

TEST_F(IoBindingRegistryTest, ModelStructureInvalidRejected_T04) {
  RegisterTestBizBinding();

  DeploymentPrepareOptions options;

  options.path_mode = DeploymentPathMode::kLexicalOnly;
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  // 情形 1：models 不是数组 (如对象)
  {
    nlohmann::json doc = {
        {"deployment",
         {{"io",
           {{"io_binding", "test_biz"},
            {"out_mem", {{"entity_out", nlohmann::json::object()}}}}}}},
        {"models", {{"mid_1", "not_an_array"}}},
        {"pipeline", DefaultPipelineNodes()}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "FIELD_TYPE");
    EXPECT_EQ(diag.path, "/models");
  }

  // 情形 2：每个模型条目都必须有 model_path。
  {
    nlohmann::json doc = {
        {"deployment",
         {{"io",
           {{"io_binding", "test_biz"},
            {"out_mem", {{"entity_out", nlohmann::json::object()}}}}}}},
        {"models",
         {{{"model_id", "mid_1"},
           {"model_type", "test_biz_embedding"},
           {"backend", "test_tensor_backend"}}}},
        {"pipeline", DefaultPipelineNodes()}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "MISSING_FIELD");
    EXPECT_EQ(diag.path, "/models/0/model_path");
  }

  // 情形 3：model_id 重复
  {
    nlohmann::json doc = {
        {"deployment",
         {{"io",
           {{"io_binding", "test_biz"},
            {"out_mem", {{"entity_out", nlohmann::json::object()}}}}}}},
        {"models",
         {{{"model_id", "mid_1"},
           {"model_type", "test_biz_embedding"},
           {"backend", "test_tensor_backend"},
           {"model_path", "models/orig1.bin"}},
          {{"model_id", "mid_1"},
           {"model_type", "test_biz_embedding"},
           {"backend", "test_tensor_backend"},
           {"model_path", "models/orig2.bin"}}}},
        {"pipeline", DefaultPipelineNodes()}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "DUPLICATE_MODEL_ID");
    EXPECT_EQ(diag.path, "/models/1/model_id");
  }
}

TEST_F(IoBindingRegistryTest, ModelPathNonexistentOnDiskIsAllowed_T05) {
  RegisterTestBizBinding();
  const auto temp_dir = fs::temp_directory_path() / "edgeflow_test_t05";
  fs::create_directories(temp_dir);
  const auto missing_path = temp_dir / "missing_dir/model.bin";
  ASSERT_FALSE(fs::exists(missing_path));
  const nlohmann::json doc = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"models",
       {{{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()},
         {"model_path", "missing_dir/model.bin"}}}},
      {"pipeline", DefaultPipelineNodes()}};
  const auto original = doc;
  DeploymentPrepareOptions options;
  options.path_mode = DeploymentPathMode::kUnderRoot;
  options.model_root_dir = temp_dir.string();
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;
  ASSERT_TRUE(PrepareDeploymentDocument(doc, options, &prepared, &diag))
      << diag.message;
  EXPECT_EQ(prepared.neutral_pipeline_json["models"][0]["model_path"],
            fs::weakly_canonical(missing_path).string());
  EXPECT_EQ(doc, original);
  EXPECT_FALSE(fs::exists(missing_path));
  fs::remove_all(temp_dir);
}

TEST_F(IoBindingRegistryTest, RemovedModelPathsIsAlwaysUnknownField_T06) {
  RegisterTestBizBinding();
  const nlohmann::json base_doc = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"models",
       {{{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()},
         {"model_path", "models/current.bin"}}}},
      {"pipeline", DefaultPipelineNodes()}};
  DeploymentPrepareOptions options;
  options.path_mode = DeploymentPathMode::kLexicalOnly;
  for (const auto& legacy_value :
       nlohmann::json::array({nlohmann::json::object(),
                              {{"mid_1", "models/current.bin"}},
                              {{"mid_1", "models/conflicting.bin"}},
                              {{"non/exist~id", "models/foo.bin"}},
                              {{"mid_1", 999}},
                              {{"mid_1", ""}},
                              nullptr,
                              "",
                              999})) {
    SCOPED_TRACE(legacy_value.dump());
    auto doc = base_doc;
    doc["deployment"]["model_paths"] = legacy_value;
    const auto original = doc;
    PreparedDeployment prepared;
    DeploymentDiagnostic diag;
    ASSERT_TRUE(PrepareDeploymentDocument(base_doc, options, &prepared, &diag));
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diag.path, "/deployment/model_paths");
    EXPECT_NE(diag.message.find("Unknown field at /deployment/model_paths"),
              std::string::npos);
    EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
    EXPECT_TRUE(prepared.binding.biz_name.empty());
    EXPECT_EQ(doc, original);
  }
}

TEST_F(IoBindingRegistryTest, OutputMemoryOverridesRemainOptional) {
  RegisterTestBizBinding();
  for (int variant = 0; variant < 3; ++variant) {
    nlohmann::json document = {
        {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
        {"pipeline", DefaultPipelineNodes()}};
    if (variant == 1)
      document["deployment"]["io"]["out_mem"] = nlohmann::json::object();
    if (variant == 2)
      document["deployment"]["io"]["out_mem"]["entity_out"] =
          nlohmann::json::object();
    const auto original = document;
    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
        << diagnostic.message;
    EXPECT_EQ(prepared.neutral_pipeline_json["biz_name"], "test_biz");
    ASSERT_EQ(prepared.output_specs.size(), 1U);
    const auto& spec = prepared.output_specs.at("entity_out");
    EXPECT_EQ(spec.type, "entity_out");
    EXPECT_GT(spec.GetCapacity("entities_json"), 0U);
    EXPECT_EQ(spec.meta_num, 0U);
    EXPECT_EQ(spec.metadata_type_id, 0);
    EXPECT_EQ(document, original);
  }
}

TEST_F(IoBindingRegistryTest, ExplicitIoBindingSelectsRegisteredBusiness) {
  RegisterTestBizBinding();
  nlohmann::json document = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"pipeline", DefaultPipelineNodes()}};
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  EXPECT_EQ(prepared.binding.biz_name, "test_biz");
  EXPECT_EQ(prepared.neutral_pipeline_json["biz_name"], "test_biz");
  EXPECT_FALSE(document.contains("biz_name"));
  document["deployment"]["io"].erase("io_binding");
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.path, "/deployment/io/io_binding");
  document["deployment"]["io"]["io_binding"] = "unknown_biz";
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "UNKNOWN_IO_BINDING");
  EXPECT_EQ(diagnostic.path, "/deployment/io/io_binding");
}

TEST_F(IoBindingRegistryTest, DefaultsAndOverridesKeepOptionalOutputOptIn) {
  auto converter =
      *IoConverterRegistry::Instance().FindOutputConverter("test.out.operator");
  converter.converter_id = "multiple.out.operator";
  auto required = converter.external_slots.front();
  required.slot_name = "second";
  required.key_suffix = "second";
  auto optional = required;
  optional.slot_name = "audit";
  optional.key_suffix = "audit";
  optional.required = false;
  converter.external_slots.push_back(required);
  converter.external_slots.push_back(optional);
  ASSERT_TRUE(
      IoConverterRegistry::Instance().RegisterOutputConverter(converter));
  IoBindingDefinition binding;
  binding.biz_name = "test_biz";
  binding.input_converter_id = "test.in.operator";
  binding.output_converter_id = converter.converter_id;
  ASSERT_TRUE(IoBindingRegistry::Instance().RegisterBinding(binding));

  nlohmann::json document = {{"pipeline", DefaultPipelineNodes()}};
  document["deployment"]["io"]["io_binding"] = binding.biz_name;
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  ASSERT_EQ(prepared.output_specs.size(), 2U);
  EXPECT_EQ(prepared.output_specs.count("audit"), 0U);
  const auto default_capacity =
      prepared.output_specs.at("entity_out").GetCapacity("entities_json");
  EXPECT_GT(default_capacity, 0U);
  EXPECT_EQ(prepared.output_specs.at("second").GetCapacity("entities_json"),
            default_capacity);

  document["deployment"]["io"]["out_mem"] = {
      {"entity_out", {{"capacities", {{"entities_json", 17}}}}},
      {"audit", nlohmann::json::object()}};
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  ASSERT_EQ(prepared.output_specs.size(), 3U);
  EXPECT_EQ(prepared.output_specs.at("entity_out").GetCapacity("entities_json"),
            17U);
  EXPECT_EQ(prepared.output_specs.at("second").GetCapacity("entities_json"),
            default_capacity);
  EXPECT_EQ(prepared.output_specs.at("audit").GetCapacity("entities_json"),
            default_capacity);
  EXPECT_EQ(prepared.output_specs.at("audit").type, "entity_out");
  EXPECT_EQ(prepared.output_specs.at("second").type, "entity_out");
}

TEST_F(IoBindingRegistryTest, SecondBindingForSameBizIsRejected) {
  RegisterTestBizBinding();
  auto converter =
      *IoConverterRegistry::Instance().FindOutputConverter("test.out.operator");
  converter.converter_id = "keyword.output.converter";
  auto& slot = converter.external_slots.front();
  slot.type_id = "CompanyOperatorKeywordOutput";
  slot.type_suffix = "keyword_out";
  slot.key_suffix = "entity_out";
  ASSERT_TRUE(
      IoConverterRegistry::Instance().RegisterOutputConverter(converter));
  auto second = *IoBindingRegistry::Instance().FindBinding("test_biz");
  second.output_converter_id = converter.converter_id;

  // 同一业务只有一份外部契约：第二个 binding 不论载体是否兼容都在注册时被拒绝。
  EXPECT_FALSE(IoBindingRegistry::Instance().RegisterBinding(second));
  EXPECT_EQ(IoBindingRegistry::Instance()
                .FindBinding("test_biz")
                ->output_converter_id,
            "test.out.operator");
  std::vector<std::string> errors;
  EXPECT_FALSE(IoBindingRegistry::Instance().Audit(&errors));
  EXPECT_TRUE(std::any_of(errors.begin(), errors.end(), [](const auto& error) {
    return error.find("Duplicate IoBinding for biz_name: test_biz") !=
           std::string::npos;
  }));
  IoBindingRegistry::Instance().ResetConflictForTesting();

  // 另一业务可以使用自己的载体契约。
  auto separate = *PipelineCatalog::FindBiz("test_biz");
  separate.biz_name = "separate_output_biz";
  ASSERT_TRUE(PipelineCatalog::RegisterBizDefinition(separate));
  second.biz_name = separate.biz_name;
  ASSERT_TRUE(IoBindingRegistry::Instance().RegisterBinding(second));
  EXPECT_TRUE(IoBindingRegistry::Instance().Audit(&errors))
      << (errors.empty() ? "" : errors.front());

  nlohmann::json document = {
      {"deployment", {{"io", {{"io_binding", second.biz_name}}}}},
      {"pipeline", DefaultPipelineNodes()}};
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  EXPECT_EQ(prepared.binding.biz_name, separate.biz_name);
  EXPECT_EQ(prepared.output_specs.at("entity_out").type, "keyword_out");
}

TEST_F(IoBindingRegistryTest, EffectiveBatchLimitIncludesBindingBound) {
  // 0 表示该来源不设上限；取最小的正值。
  struct Limits {
    size_t binding;
    size_t input;
    size_t output;
    size_t expected;
  };
  const Limits cases[] = {
      {1, 64, 64, 1},    {0, 64, 64, 64}, {0, 8, 16, 8},
      {128, 64, 64, 64}, {128, 8, 64, 8}, {128, 64, 16, 16},
      {64, 0, 0, 64},    {0, 0, 16, 16},  {32, 0, 64, 32},
  };
  auto input =
      *IoConverterRegistry::Instance().FindInputConverter("test.in.operator");
  auto output =
      *IoConverterRegistry::Instance().FindOutputConverter("test.out.operator");
  const nlohmann::json document = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"pipeline", DefaultPipelineNodes()}};
  for (const auto& limits : cases) {
    SCOPED_TRACE(::testing::Message()
                 << "binding=" << limits.binding << ", input=" << limits.input
                 << ", output=" << limits.output);
    IoBindingRegistry::Instance().ClearForTesting();
    IoConverterRegistry::Instance().ClearForTesting();
    input.max_batch_size = limits.input;
    output.max_batch_size = limits.output;
    ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(input));
    ASSERT_TRUE(
        IoConverterRegistry::Instance().RegisterOutputConverter(output));
    RegisterTestBizBinding(limits.binding);

    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
        << diagnostic.message;
    EXPECT_EQ(prepared.effective_max_batch_size, limits.expected);
  }
}

TEST_F(IoBindingRegistryTest, RemovedAllocationFieldIsRejected) {
  RegisterTestBizBinding();
  const nlohmann::json document = {
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz"},
          {"output_allocations", nlohmann::json::object()}}}}},
      {"pipeline", DefaultPipelineNodes()}};
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diagnostic.path, "/deployment/io/output_allocations");
}

TEST_F(IoBindingRegistryTest,
       PublicBusinessQueryWorksWithoutInitAndClearsFailures) {
  RegisterTestBizBinding();
  const auto directory = fs::temp_directory_path() / "edgeflow_business_query";
  fs::create_directories(directory);
  const std::string root = directory.string();
  nlohmann::json document = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"pipeline",
       {{{"id", "copy"},
         {"node_type", "TextTemplateNode"},
         {"inputs", {{"primary", "input_sentences"}}},
         {"outputs", {{"text", "llm_answers"}}},
         {"config", {{"template", "{{primary}}"}}}}}}};
  std::ofstream(directory / "pipeline.json") << document;
  std::ofstream(directory / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  std::string biz = "stale";
  char error[512]{};
  const int decoded_before = dummy_decode_calls;
  const int encoded_before = dummy_encode_calls;
  ASSERT_EQ(operator_api::ResolveOperatorConfigBiz(
                root.c_str(), "pipeline.conf", &biz, error, sizeof(error)),
            0)
      << error;
  EXPECT_EQ(biz, "test_biz");
  EXPECT_EQ(dummy_decode_calls, decoded_before);
  EXPECT_EQ(dummy_encode_calls, encoded_before);
  document["deployment"]["io"]["io_binding"] = "unknown_biz";
  std::ofstream(directory / "pipeline.json") << document;
  EXPECT_EQ(operator_api::ResolveOperatorConfigBiz(
                root.c_str(), "pipeline.conf", &biz, error, sizeof(error)),
            -2);
  EXPECT_TRUE(biz.empty());
  EXPECT_NE(std::string(error).find("unknown_biz"), std::string::npos);
  EXPECT_EQ(operator_api::ResolveOperatorConfigBiz(
                root.c_str(), "pipeline.conf", nullptr, error, sizeof(error)),
            -2);
  fs::remove_all(directory);
}

TEST_F(IoBindingRegistryTest, DeploymentIoUnknownBindingOrMismatch_T07) {
  RegisterTestBizBinding();

  nlohmann::json base_doc = {
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz"},
          {"out_mem",
           {{"entity_out",
             {{"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", DefaultPipelineNodes()}};

  DeploymentPrepareOptions options;

  options.path_mode = DeploymentPathMode::kLexicalOnly;
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  // 情形 1：未知 binding
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["io"]["io_binding"] = "completely_unknown_binding";
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "UNKNOWN_IO_BINDING");
    EXPECT_EQ(diag.path, "/deployment/io/io_binding");
  }

  // 情形 2：即使 binding 存在，已移除的根业务标识也会被拒绝。
  {
    nlohmann::json doc = base_doc;
    doc["biz_name"] = "other_biz_v1";
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diag.path, "/biz_name");
  }
}

TEST_F(IoBindingRegistryTest, DeploymentIoSlotValidation_T08) {
  RegisterTestBizBinding();

  nlohmann::json base_doc = {
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz"},
          {"out_mem",
           {{"entity_out",
             {{"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", DefaultPipelineNodes()}};

  DeploymentPrepareOptions options;

  options.path_mode = DeploymentPathMode::kLexicalOnly;
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  // 情形 1：缺少的必填槽位取其注册的默认值。
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["io"]["out_mem"].erase("entity_out");
    ASSERT_TRUE(PrepareDeploymentDocument(doc, options, &prepared, &diag))
        << diag.message;
    EXPECT_EQ(prepared.output_specs.count("entity_out"), 1U);
  }

  // 情形 2：out_mem 中有未知槽位
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["io"]["out_mem"]["unexpected_extra_slot"] =
        nlohmann::json::object();
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "UNKNOWN_OUTPUT_SLOT");
    EXPECT_EQ(diag.path, "/deployment/io/out_mem/unexpected_extra_slot");
  }

  // 情形 3：已移除的 type 字段即使与槽位一致也会被拒绝。
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["io"]["out_mem"]["entity_out"]["type"] = "entity_out";
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "INVALID_OUTPUT_ALLOCATION");
    EXPECT_EQ(diag.path, "/deployment/io/out_mem/entity_out");
  }
}

TEST_F(IoBindingRegistryTest, PrepareFailureResetsPreparedStateAtomically_T18) {
  RegisterTestBizBinding();

  nlohmann::json valid_doc = {
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz"},
          {"out_mem",
           {{"entity_out",
             {{"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models",
       {{{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()},
         {"model_path", "models/original.bin"}}}},
      {"pipeline", DefaultPipelineNodes()}};

  DeploymentPrepareOptions options;

  options.path_mode = DeploymentPathMode::kLexicalOnly;

  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  // 1. 首次准备成功
  ASSERT_TRUE(PrepareDeploymentDocument(valid_doc, options, &prepared, &diag));
  EXPECT_FALSE(prepared.binding.biz_name.empty());
  EXPECT_NE(prepared.input_converter, nullptr);
  EXPECT_NE(prepared.output_converter, nullptr);
  EXPECT_FALSE(prepared.neutral_pipeline_json.is_null());
  EXPECT_FALSE(prepared.output_specs.empty());

  // 2. 在失败的文档上复用同一个 prepared 实例
  nlohmann::json invalid_doc = valid_doc;
  invalid_doc["deployment"]["io"]["io_binding"] = "unknown_biz";
  const nlohmann::json invalid_doc_copy = invalid_doc;

  EXPECT_FALSE(
      PrepareDeploymentDocument(invalid_doc, options, &prepared, &diag));

  // 验证 prepared 的所有字段都已完全重置
  EXPECT_TRUE(prepared.binding.biz_name.empty());
  EXPECT_EQ(prepared.input_converter, nullptr);
  EXPECT_EQ(prepared.output_converter, nullptr);
  EXPECT_EQ(prepared.effective_max_batch_size, 0u);
  EXPECT_TRUE(prepared.output_specs.empty());
  EXPECT_TRUE(prepared.output_parameter_texts.empty());
  EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
  EXPECT_TRUE(prepared.io_boundary.input_published_ports.empty());
  EXPECT_TRUE(prepared.io_boundary.output_consumed_ports.empty());

  // 验证输入文档未被修改
  EXPECT_EQ(invalid_doc, invalid_doc_copy);

  // 解析完第一个模型后失败时，不得发布部分状态。
  valid_doc["models"].push_back(valid_doc["models"][0]);
  valid_doc["models"][1]["model_id"] = "mid_2";
  valid_doc["models"][1]["model_path"] = "models/second.bin";
  options.path_mode = DeploymentPathMode::kUnderRoot;
  options.model_root_dir = fs::temp_directory_path().string();
  ASSERT_TRUE(PrepareDeploymentDocument(valid_doc, options, &prepared, &diag))
      << diag.message;
  ASSERT_EQ(prepared.neutral_pipeline_json["models"].size(), 2u);
  invalid_doc = valid_doc;
  invalid_doc["models"][1]["model_path"] = "../../escaped_second.bin";
  const auto invalid_second_copy = invalid_doc;
  EXPECT_FALSE(
      PrepareDeploymentDocument(invalid_doc, options, &prepared, &diag));
  EXPECT_EQ(diag.code, "INVALID_MODEL_PATH");
  EXPECT_EQ(diag.path, "/models/1/model_path");
  EXPECT_TRUE(prepared.binding.biz_name.empty());
  EXPECT_EQ(prepared.input_converter, nullptr);
  EXPECT_EQ(prepared.output_converter, nullptr);
  EXPECT_EQ(prepared.effective_max_batch_size, 0u);
  EXPECT_TRUE(prepared.output_specs.empty());
  EXPECT_TRUE(prepared.output_parameter_texts.empty());
  EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
  EXPECT_TRUE(prepared.io_boundary.input_published_ports.empty());
  EXPECT_TRUE(prepared.io_boundary.output_consumed_ports.empty());
  EXPECT_EQ(invalid_doc, invalid_second_copy);
}

TEST_F(IoBindingRegistryTest, ModelPathDiagnosticsKeepApplicableRemediation) {
  const nlohmann::json document = {
      {"models",
       {{{"model_id", "mid~0/path"}, {"model_path", "first.bin"}},
        {{"model_id", "mid_1"}, {"model_path", "second.bin"}}}}};
  ValidationReport report;
  report.ok = false;
  for (size_t index = 0; index < 2; ++index) {
    ValidationDiagnostic diagnostic;
    diagnostic.code = DiagnosticCode::kFieldRange;
    diagnostic.path = "/models/" + std::to_string(index) + "/model_path";
    diagnostic.message = "Invalid model path";
    ValidationFix fix;
    fix.id = "fix_model_" + std::to_string(index);
    fix.patch = nlohmann::json::array(
        {{{"op", "replace"},
          {"path", diagnostic.path},
          {"value", "corrected_" + std::to_string(index) + ".bin"}}});
    diagnostic.remediation.emplace();
    diagnostic.remediation->fixes.push_back(fix);
    report.diagnostics.push_back(diagnostic);
  }
  ValidationDiagnostic port_diagnostic;
  port_diagnostic.code = DiagnosticCode::kMissingField;
  port_diagnostic.path = "/pipeline/0/inputs/text";
  report.diagnostics.push_back(port_diagnostic);

  ProjectDeploymentDiagnostics(&report);

  ASSERT_EQ(report.diagnostics.size(), 3u);
  auto fixed_document = document;
  for (size_t index = 0; index < 2; ++index) {
    const auto& diagnostic = report.diagnostics[index];
    EXPECT_EQ(diagnostic.path,
              "/models/" + std::to_string(index) + "/model_path");
    ASSERT_TRUE(diagnostic.remediation.has_value());
    ASSERT_EQ(diagnostic.remediation->fixes.size(), 1u);
    const auto& fix = diagnostic.remediation->fixes[0];
    EXPECT_EQ(fix.id, "fix_model_" + std::to_string(index));
    fixed_document = fixed_document.patch(fix.patch);
    EXPECT_EQ(fixed_document["models"][index]["model_path"],
              "corrected_" + std::to_string(index) + ".bin");
  }
  EXPECT_EQ(report.diagnostics[2].path, port_diagnostic.path);
  EXPECT_FALSE(fixed_document.contains("deployment"));
}

TEST_F(IoBindingRegistryTest,
       ProjectDerivedBusinessDiagnosticsKeepsOnlyExternalFixes) {
  RegisterTestBizBinding();
  const nlohmann::json external_document = {
      {"deployment", {{"io", {{"io_binding", "test_biz"}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", DefaultPipelineNodes()}};
  DeploymentPrepareOptions options;
  options.path_mode = DeploymentPathMode::kLexicalOnly;
  PreparedDeployment prepared;
  DeploymentDiagnostic deployment_diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(external_document, options, &prepared,
                                        &deployment_diagnostic));
  ASSERT_EQ(prepared.neutral_pipeline_json["biz_name"], "test_biz");
  ASSERT_FALSE(external_document.contains("biz_name"));

  ValidationFix derived_business_fix;
  derived_business_fix.id = "edit_derived_business";
  derived_business_fix.patch = nlohmann::json::array(
      {{{"op", "add"}, {"path", "/biz_name"}, {"value", "test_biz"}}});

  ValidationFix node_fix;
  node_fix.id = "correct_rule_keyword";
  node_fix.patch =
      nlohmann::json::array({{{"op", "replace"},
                              {"path", "/pipeline/0/config/categories/CAT/0"},
                              {"value", "corrected"}}});

  ValidationFix mixed_fix = node_fix;
  mixed_fix.id = "edit_rule_and_derived_business";
  mixed_fix.patch.push_back(derived_business_fix.patch[0]);

  ValidationDiagnostic business_diagnostic;
  business_diagnostic.code = DiagnosticCode::kUnknownBiz;
  business_diagnostic.path = "/biz_name";
  business_diagnostic.remediation.emplace();
  business_diagnostic.remediation->fixes = {derived_business_fix};

  ValidationDiagnostic node_diagnostic;
  node_diagnostic.code = DiagnosticCode::kFieldRange;
  node_diagnostic.path = "/pipeline/0/config/categories/CAT/0";
  node_diagnostic.remediation.emplace();
  node_diagnostic.remediation->fixes = {mixed_fix, node_fix};

  ValidationReport report;
  report.diagnostics = {business_diagnostic, node_diagnostic};
  ProjectDeploymentDiagnostics(&report);

  ASSERT_EQ(report.diagnostics.size(), 2u);
  EXPECT_EQ(report.diagnostics[0].code, DiagnosticCode::kUnknownBiz);
  EXPECT_EQ(report.diagnostics[0].path, "/deployment/io/io_binding");
  ASSERT_TRUE(report.diagnostics[0].remediation.has_value());
  EXPECT_TRUE(report.diagnostics[0].remediation->fixes.empty());

  EXPECT_EQ(report.diagnostics[1].path, node_diagnostic.path);
  ASSERT_TRUE(report.diagnostics[1].remediation.has_value());
  const auto& retained_fixes = report.diagnostics[1].remediation->fixes;
  ASSERT_EQ(retained_fixes.size(), 1u);
  EXPECT_EQ(retained_fixes[0].id, node_fix.id);
  EXPECT_EQ(retained_fixes[0].patch, node_fix.patch);

  const auto fixed_document = external_document.patch(retained_fixes[0].patch);
  EXPECT_EQ(fixed_document["pipeline"][0]["config"]["categories"]["CAT"][0],
            "corrected");
  EXPECT_FALSE(fixed_document.contains("biz_name"));
  EXPECT_TRUE(PrepareDeploymentDocument(fixed_document, options, &prepared,
                                        &deployment_diagnostic));
}

TEST_F(IoBindingRegistryTest,
       ResolveFromPipelineJsonDiagnosticCarrier_T03_T06_T07_T08) {
  RegisterTestBizBinding();

  // T03：经 IoBindingResolver 缺少直接模型路径
  nlohmann::json t03_doc = {
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz"},
          {"out_mem",
           {{"entity_out",
             {{"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models",
       {{{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()}}}},
      {"pipeline", DefaultPipelineNodes()}};

  std::unique_ptr<ValidatedIoPlan> plan;
  std::string err;
  DeploymentDiagnostic diag;
  int rc = IoBindingResolver::ResolveFromPipelineJson(t03_doc, "./models",
                                                      &plan, &err, &diag);
  EXPECT_EQ(rc, -3);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "MISSING_FIELD");
  EXPECT_EQ(diag.path, "/models/0/model_path");

  // T06：已移除的部署字段 model_paths
  nlohmann::json t06_doc = t03_doc;
  t06_doc["models"][0]["model_path"] = "models/original.bin";
  t06_doc["deployment"]["model_paths"] = {{"unknown_mid", "models/foo.bin"}};
  rc = IoBindingResolver::ResolveFromPipelineJson(t06_doc, "./models", &plan,
                                                  &err, &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diag.path, "/deployment/model_paths");

  // T07：经 IoBindingResolver 的未知 binding
  nlohmann::json t07_doc = t03_doc;
  t07_doc["models"][0]["model_path"] = "models/original.bin";
  t07_doc["deployment"]["io"]["io_binding"] = "nonexistent_biz";
  rc = IoBindingResolver::ResolveFromPipelineJson(t07_doc, "./models", &plan,
                                                  &err, &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "UNKNOWN_IO_BINDING");
  EXPECT_EQ(diag.path, "/deployment/io/io_binding");

  // T08：非法的输出分配保留其精确诊断
  nlohmann::json t08_doc = t03_doc;
  t08_doc["models"][0]["model_path"] = "models/original.bin";
  t08_doc["deployment"]["io"]["out_mem"]["entity_out"] = 42;
  rc = IoBindingResolver::ResolveFromPipelineJson(t08_doc, "./models", &plan,
                                                  &err, &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "INVALID_OUTPUT_ALLOCATION");
  EXPECT_EQ(diag.path, "/deployment/io/out_mem/entity_out");
}

nlohmann::json ValidTextTemplatePipelineNodes() {
  nlohmann::json node;
  node["id"] = "node_0";
  node["node_type"] = "TextTemplateNode";
  node["depends_on"] = nlohmann::json::array();
  node["inputs"]["primary"] = "input_sentences";
  node["outputs"]["text"] = "llm_answers";
  node["config"]["template"] = "{{primary}}";
  return nlohmann::json::array({node});
}

TEST_F(IoBindingRegistryTest,
       ResolveFromFileDiagnosticCarrier_T03_T06_T07_T08_AndFileErrors) {
  RegisterTestBizBinding();

  fs::path temp_dir =
      fs::temp_directory_path() / "edgeflow_test_resolve_from_file";
  fs::create_directories(temp_dir);

  auto write_file = [](const fs::path& p, const nlohmann::json& content) {
    std::ofstream ofs(p);
    ofs << content.dump(2);
    ofs.flush();
    ofs.close();
  };
  auto write_raw = [](const fs::path& p, const std::string& content) {
    std::ofstream ofs(p);
    ofs << content;
    ofs.flush();
    ofs.close();
  };

  nlohmann::json base_pipeline = {
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz"},
          {"out_mem",
           {{"entity_out",
             {{"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", ValidTextTemplatePipelineNodes()}};

  fs::path pipe_path = temp_dir / "pipeline.json";
  fs::path conf_path = temp_dir / "pipeline.conf";
  write_file(pipe_path, base_pipeline);
  write_file(conf_path, {{"pipe_path", "pipeline.json"}});

  std::unique_ptr<ValidatedIoPlan> plan;
  std::string err;
  DeploymentDiagnostic diag;

  // 1. 成功情形
  int rc = IoBindingResolver::ResolveFromFile(conf_path.string(), "", &plan,
                                              &err, &diag);
  EXPECT_EQ(rc, 0);
  EXPECT_NE(plan, nullptr);
  EXPECT_TRUE(diag.code.empty());

  // 2. 经文件的 T03：缺少 model_path
  {
    nlohmann::json t03_pipe = base_pipeline;
    t03_pipe["models"] = {
        {{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()}}};
    write_file(pipe_path, t03_pipe);

    rc = IoBindingResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                            &diag);
    EXPECT_EQ(rc, -3);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "MISSING_FIELD");
    EXPECT_EQ(diag.path, "/models/0/model_path");
  }

  // 3. 经文件的 T06：已移除的部署字段 model_paths
  {
    nlohmann::json t06_pipe = base_pipeline;
    t06_pipe["deployment"]["model_paths"] = {{"unknown_mid", "models/foo.bin"}};
    write_file(pipe_path, t06_pipe);

    rc = IoBindingResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                            &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diag.path, "/deployment/model_paths");
  }

  // 4. 经文件的 T07：未知 io_binding
  {
    nlohmann::json t07_pipe = base_pipeline;
    t07_pipe["deployment"]["io"]["io_binding"] = "unregistered_biz";
    write_file(pipe_path, t07_pipe);

    rc = IoBindingResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                            &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "UNKNOWN_IO_BINDING");
    EXPECT_EQ(diag.path, "/deployment/io/io_binding");
  }

  // 5. 经文件的 T08：非法的输出分配
  {
    nlohmann::json t08_pipe = base_pipeline;
    t08_pipe["deployment"]["io"]["out_mem"]["entity_out"] = 42;
    write_file(pipe_path, t08_pipe);

    rc = IoBindingResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                            &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "INVALID_OUTPUT_ALLOCATION");
    EXPECT_EQ(diag.path, "/deployment/io/out_mem/entity_out");
  }

  // 恢复合法的 Pipeline 文件
  write_file(pipe_path, base_pipeline);

  // 6. conf 文件不存在 -> CONFIG_FILE_OPEN
  rc = IoBindingResolver::ResolveFromFile(
      (temp_dir / "nonexistent.conf").string(), "", &plan, &err, &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "CONFIG_FILE_OPEN");
  EXPECT_EQ(diag.path, "/");

  // 7. conf 文件 JSON 格式错误 -> JSON_PARSE
  {
    fs::path bad_conf = temp_dir / "bad_syntax.conf";
    write_raw(bad_conf, "{ unquoted: invalid JSON ...");
    rc = IoBindingResolver::ResolveFromFile(bad_conf.string(), "", &plan, &err,
                                            &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "JSON_PARSE");
    EXPECT_EQ(diag.path, "/");
  }

  // 8. conf 引用的 Pipeline 文件缺失 -> /pipe_path 处 DEPLOYMENT_ERROR
  {
    fs::path missing_pipe_conf = temp_dir / "missing_pipe.conf";
    write_file(missing_pipe_conf, {{"pipe_path", "missing_pipeline.json"}});
    rc = IoBindingResolver::ResolveFromFile(missing_pipe_conf.string(), "",
                                            &plan, &err, &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diag.path, "/pipe_path");
  }

  // 9. Pipeline 文件 JSON 格式错误 -> JSON_PARSE
  {
    fs::path bad_pipe = temp_dir / "bad_pipe.json";
    write_raw(bad_pipe, "{ bad_pipe_json: invalid");

    fs::path bad_pipe_conf = temp_dir / "bad_pipe.conf";
    write_file(bad_pipe_conf, {{"pipe_path", "bad_pipe.json"}});

    rc = IoBindingResolver::ResolveFromFile(bad_pipe_conf.string(), "", &plan,
                                            &err, &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "JSON_PARSE");
    EXPECT_EQ(diag.path, "/");
  }

  fs::remove_all(temp_dir);
}

}  // namespace llm_edgeflow
