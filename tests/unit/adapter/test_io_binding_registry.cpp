#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "adapter/converter_authoring.h"
#include "adapter/deployment_diagnostic.h"
#include "adapter/deployment_io_config.h"
#include "adapter/deployment_preparation.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_binding_resolver.h"
#include "adapter/io_converter_registry.h"
#include "adapter/pipeline_document.h"
#include "adapter/shared_algorithm_runtime.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_config.h"

namespace llm_edgeflow {
namespace {

namespace fs = std::filesystem;

int DummyDecode(const ExternalInputBatchView&, const InputDecodeOptions&,
                const InputPortBindings&, AlgContext*, AdapterStatus*) {
  return 0;
}

int DummyEncode(AlgContext*, const OutputPortBindings&,
                const OutputEncodeOptions&, ExternalOutputBatchView*,
                size_t* written_count, AdapterStatus*) {
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
    saved_exposures_ = IoBindingRegistry::Instance().AllExposures();
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
    for (const auto& exp : saved_exposures_) {
      IoBindingRegistry::Instance().RegisterExposure(exp);
    }
  }

  void SetUp() override {
    IoConverterRegistry::Instance().ClearForTesting();
    IoBindingRegistry::Instance().ClearForTesting();

    // 注册基础转换器供测试
    InputConverterDefinition in_def;
    in_def.converter_id = "test.in.operator";

    in_def.schema_id = "in_schema";
    in_def.schema_version = 1;
    in_def.external_type = "CompanyOperatorEntityInput";
    in_def.external_slots = {ExternalSlotDefinition(
        "entity_in", "CompanyOperatorEntityInput", PortDirection::kInput, true,
        "CompanyOperatorEntityInput", "entity_in")};
    in_def.max_batch_size = 64;
    in_def.logical_ports = {
        NodePortDefinition("texts", "TextBatch", true, "1:1")};
    in_def.decode_fn = &DummyDecode;
    IoConverterRegistry::Instance().RegisterInputConverter(in_def);

    OutputConverterDefinition out_def;
    out_def.converter_id = "test.out.operator";

    out_def.schema_id = "out_schema";
    out_def.schema_version = 1;
    out_def.external_type = "CompanyOperatorEntityOutput";
    out_def.external_slots = {ExternalSlotDefinition(
        "entity_out", "CompanyOperatorEntityOutput", PortDirection::kOutput,
        true, "CompanyOperatorEntityOutput", "entity_out", {"entities_json"})};
    out_def.max_batch_size = 64;
    out_def.logical_ports = {
        NodePortDefinition("answers", "TextBatch", true, "1:1")};
    out_def.encode_fn = &DummyEncode;
    IoConverterRegistry::Instance().RegisterOutputConverter(out_def);

    // 注册业务契约
    BizDefinition biz;
    biz.biz_name = "test_biz_v1";
    biz.ingress = {BizPortDefinition("input_sentences", "TextBatch", true)};
    biz.egress = {BizPortDefinition("llm_answers", "TextBatch", true)};
    PipelineCatalog::RegisterBizDefinition(biz);
  }

  void TearDown() override {
    IoConverterRegistry::Instance().ClearForTesting();
    IoBindingRegistry::Instance().ClearForTesting();
  }

  void RegisterTestBizBinding() {
    IoBindingDefinition binding;
    binding.binding_id = "test_biz.operator.v1";
    binding.biz_name = "test_biz_v1";

    binding.input_converter_id = "test.in.operator";
    binding.output_converter_id = "test.out.operator";
    binding.input_ports = {{"texts", "input_sentences"}};
    binding.output_ports = {{"answers", "llm_answers"}};
    ASSERT_TRUE(IoBindingRegistry::Instance().RegisterBinding(binding));
  }

  static inline std::vector<IoBindingDefinition> saved_bindings_;
  static inline std::vector<BizExposureDefinition> saved_exposures_;
  static inline std::vector<InputConverterDefinition> saved_inputs_;
  static inline std::vector<OutputConverterDefinition> saved_outputs_;
};

TEST_F(IoBindingRegistryTest, RegisterAndAuditValidBinding) {
  auto& reg = IoBindingRegistry::Instance();

  IoBindingDefinition binding;
  binding.binding_id = "test_biz.operator.v1";
  binding.biz_name = "test_biz_v1";

  binding.input_converter_id = "test.in.operator";
  binding.output_converter_id = "test.out.operator";
  binding.input_ports = {{"texts", "input_sentences"}};
  binding.output_ports = {{"answers", "llm_answers"}};

  EXPECT_TRUE(reg.RegisterBinding(binding));

  BizExposureDefinition exposure;
  exposure.biz_name = "test_biz_v1";
  exposure.max_batch_size = 32;

  EXPECT_TRUE(reg.RegisterExposure(exposure));

  std::vector<std::string> audit_errors;
  EXPECT_TRUE(reg.Audit(&audit_errors));
  EXPECT_TRUE(audit_errors.empty());
}

TEST_F(IoBindingRegistryTest, AuditRejectsUnregisteredConvertersAndBiz) {
  auto& reg = IoBindingRegistry::Instance();

  // 1. 引用不存在的 biz_name
  IoBindingDefinition bad_biz;
  bad_biz.binding_id = "bad_biz.binding";
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
  bad_conv.binding_id = "bad_conv.binding";
  bad_conv.biz_name = "test_biz_v1";

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

TEST_F(IoBindingRegistryTest, AuditRejectsMissingProductionExposure) {
  auto& reg = IoBindingRegistry::Instance();

  // 暴露要求 operator，但未注册 operator 绑定
  BizExposureDefinition exposure;
  exposure.biz_name = "test_biz_v1";

  reg.RegisterExposure(exposure);

  std::vector<std::string> errors;
  EXPECT_FALSE(reg.Audit(&errors));
  bool found_missing_exp = false;
  for (const auto& e : errors) {
    if (e.find("lacks a valid Operator binding") != std::string::npos) {
      found_missing_exp = true;
    }
  }
  EXPECT_TRUE(found_missing_exp);
}

TEST_F(IoBindingRegistryTest, UnselectedIllegalBindingFailsAudit) {
  auto& reg = IoBindingRegistry::Instance();

  // 1. 注册合法绑定与曝光
  IoBindingDefinition valid_binding;
  valid_binding.binding_id = "test_biz.operator.v1";
  valid_binding.biz_name = "test_biz_v1";

  valid_binding.input_converter_id = "test.in.operator";
  valid_binding.output_converter_id = "test.out.operator";
  valid_binding.input_ports = {{"texts", "input_sentences"}};
  valid_binding.output_ports = {{"answers", "llm_answers"}};
  EXPECT_TRUE(reg.RegisterBinding(valid_binding));

  BizExposureDefinition exposure;
  exposure.biz_name = "test_biz_v1";
  exposure.max_batch_size = 32;

  EXPECT_TRUE(reg.RegisterExposure(exposure));

  // 单独 audit 合法绑定应当通过
  std::vector<std::string> errors;
  EXPECT_TRUE(reg.Audit(&errors));
  EXPECT_TRUE(errors.empty());

  // 2. 注册未被选择使用的非法绑定 (缺失必需输入映射)
  IoBindingDefinition illegal_binding;
  illegal_binding.binding_id = "unselected_bad.operator.v1";
  illegal_binding.biz_name = "test_biz_v1";

  illegal_binding.input_converter_id = "test.in.operator";
  illegal_binding.output_converter_id = "test.out.operator";
  illegal_binding.input_ports = {};  // 缺失必需 logical port texts
  illegal_binding.output_ports = {{"answers", "llm_answers"}};
  EXPECT_TRUE(reg.RegisterBinding(illegal_binding));

  // 全量审计必须被这个未被选中的非法绑定阻断
  errors.clear();
  EXPECT_FALSE(reg.Audit(&errors));
  bool found_missing_port_mapping = false;
  for (const auto& e : errors) {
    if (e.find(
            "missing required input converter logical port mapping: texts") !=
        std::string::npos) {
      found_missing_port_mapping = true;
    }
  }
  EXPECT_TRUE(found_missing_port_mapping);

  // 验证 SharedAlgorithmRuntime::GlobalInit() 也会因为 Audit 失败而返回冲突错误
  // (-6)
  EXPECT_EQ(SharedAlgorithmRuntime::GlobalInit(),
            COMPANY_ALG_ERR_REGISTRY_CONFLICT);
}

TEST_F(IoBindingRegistryTest, DeploymentIoConfigValidation) {
  // 1. 合法 RFC-0061 Operator 定位配置 (仅包含 pipe_path)
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
      {"data",
       {{"pipe_path", "test.json"}, {"io_binding", "test_biz.operator.v1"}}}};
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

TEST_F(IoBindingRegistryTest, FailClosedAuditRejectsInvalidUnselectedBinding) {
  auto& reg = IoBindingRegistry::Instance();

  // 注册合法暴露与绑定
  IoBindingDefinition valid_binding;
  valid_binding.binding_id = "test_biz.operator.v1";
  valid_binding.biz_name = "test_biz_v1";

  valid_binding.input_converter_id = "test.in.operator";
  valid_binding.output_converter_id = "test.out.operator";
  valid_binding.input_ports = {{"texts", "input_sentences"}};
  valid_binding.output_ports = {{"answers", "llm_answers"}};
  EXPECT_TRUE(reg.RegisterBinding(valid_binding));

  BizExposureDefinition exposure;
  exposure.biz_name = "test_biz_v1";
  exposure.max_batch_size = 32;

  EXPECT_TRUE(reg.RegisterExposure(exposure));

  // 注册一个未被任何曝光引用的非法绑定 (输入端口缺少必需端口)
  IoBindingDefinition unselected_bad_binding;
  unselected_bad_binding.binding_id = "unselected_bad.operator.v1";
  unselected_bad_binding.biz_name = "test_biz_v1";

  unselected_bad_binding.input_converter_id = "test.in.operator";
  unselected_bad_binding.output_converter_id = "test.out.operator";
  // 故意遗漏必需输入映射 texts
  unselected_bad_binding.input_ports = {};
  unselected_bad_binding.output_ports = {{"answers", "llm_answers"}};
  EXPECT_TRUE(reg.RegisterBinding(unselected_bad_binding));

  // 全量 Audit 必须对所有已注册绑定实行 Fail-Closed 检查
  std::vector<std::string> audit_errors;
  EXPECT_FALSE(reg.Audit(&audit_errors));
  EXPECT_FALSE(audit_errors.empty());

  // GlobalInit 必须失败并返回 -6 (COMPANY_ALG_ERR_REGISTRY_CONFLICT)
  EXPECT_EQ(SharedAlgorithmRuntime::GlobalInit(), -6);
}

TEST_F(IoBindingRegistryTest, SplitPipelineDocumentAndCoreBoundary) {
  // 1. 合法完整文档拆分
  nlohmann::json valid_doc = {
      {"biz_name", "keyword_match_v1"},
      {"deployment",
       {{"model_paths", {{"m1", "path/to/m1"}}},
        {"io",
         {{"io_binding", "test.binding.v1"},
          {"output_allocations", {{"out1", nlohmann::json::object()}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"id", "n0"},
         {"node_type", "TextRuleMatchNode"},
         {"depends_on", nlohmann::json::array()},
         {"inputs", {{"text", "in"}}},
         {"outputs", {{"matches", "out"}}},
         {"config", {{"categories", {{"CAT", {"word"}}}}}}}}}};

  PipelineDocumentSplit split;
  std::string err;
  EXPECT_TRUE(SplitPipelineDocument(valid_doc, &split, &err));
  EXPECT_TRUE(split.has_deployment);
  EXPECT_TRUE(split.deployment.has_model_paths);
  EXPECT_EQ(split.deployment.model_paths["m1"], "path/to/m1");
  EXPECT_TRUE(split.deployment.has_io);
  EXPECT_EQ(split.deployment.io.io_binding, "test.binding.v1");
  EXPECT_FALSE(split.neutral_pipeline_json.contains("deployment"));
  EXPECT_EQ(split.neutral_pipeline_json["biz_name"], "keyword_match_v1");

  // 2. 中性文档 (无 deployment)
  nlohmann::json neutral_doc = {
      {"biz_name", "keyword_match_v1"},
      {"models", nlohmann::json::array()},
      {"pipeline",
       {{{"id", "n0"},
         {"node_type", "TextRuleMatchNode"},
         {"depends_on", nlohmann::json::array()},
         {"inputs", {{"text", "in"}}},
         {"outputs", {{"matches", "out"}}},
         {"config", {{"categories", {{"CAT", {"word"}}}}}}}}}};
  EXPECT_TRUE(SplitPipelineDocument(neutral_doc, &split, &err));
  EXPECT_FALSE(split.has_deployment);
  EXPECT_EQ(split.neutral_pipeline_json, neutral_doc);

  // 3. 拒绝 deployment 内部未知字段
  nlohmann::json bad_dep = valid_doc;
  bad_dep["deployment"]["unknown_key"] = 123;
  EXPECT_FALSE(SplitPipelineDocument(bad_dep, &split, &err));
  EXPECT_NE(err.find("Unknown field at /deployment"), std::string::npos);

  // 4. 拒绝 deployment.io 缺失
  nlohmann::json missing_io = valid_doc;
  missing_io["deployment"].erase("io");
  EXPECT_FALSE(SplitPipelineDocument(missing_io, &split, &err));
  EXPECT_NE(err.find("Missing required field '/deployment/io'"),
            std::string::npos);

  // 5. 拒绝 deployment.io.io_binding 缺失或空
  nlohmann::json bad_io = valid_doc;
  bad_io["deployment"]["io"]["io_binding"] = "";
  EXPECT_FALSE(SplitPipelineDocument(bad_io, &split, &err));

  // 6. 拒绝 deployment.io 内部未知字段
  bad_io["deployment"]["io"]["io_binding"] = "test.binding.v1";
  bad_io["deployment"]["io"]["extra_field"] = "bad";
  EXPECT_FALSE(SplitPipelineDocument(bad_io, &split, &err));
  EXPECT_NE(err.find("Unknown field at /deployment/io"), std::string::npos);

  // 7. Core 边界检查: 带 deployment 的文档直接提交给 Core 严格解析必须被拒绝
  // (Unknown root field)
  ParsedPipelineConfig parsed_core;
  PipelineDiagnostic diag;
  EXPECT_FALSE(ParsePipelineConfig(valid_doc, &parsed_core, &diag));
  EXPECT_EQ(diag.code, DiagnosticCode::kUnknownField);
  EXPECT_EQ(diag.path, "/deployment");

  // 8. 拆分出的中性文档提交给 Core 可以解析成功
  EXPECT_TRUE(SplitPipelineDocument(valid_doc, &split, &err));
  EXPECT_TRUE(
      ParsePipelineConfig(split.neutral_pipeline_json, &parsed_core, &diag))
      << diag.message;

  // 9. 如果源文档含有拼写错误的根字段 (例如 deploymen)，拆分时不被过滤，Core
  // 解析必须报错
  nlohmann::json typo_doc = neutral_doc;
  typo_doc["deploymen"] = nlohmann::json::object();
  EXPECT_TRUE(SplitPipelineDocument(typo_doc, &split, &err));
  EXPECT_FALSE(
      ParsePipelineConfig(split.neutral_pipeline_json, &parsed_core, &diag));
  EXPECT_EQ(diag.code, DiagnosticCode::kUnknownField);
  EXPECT_EQ(diag.path, "/deploymen");
}

TEST_F(IoBindingRegistryTest, BizMismatchFailsClosedWithExactPointer) {
  RegisterTestBizBinding();

  // A pipeline with registered biz smart_doc_qa_v1 but binding
  // test_biz.operator.v1 (biz test_biz_v1)
  nlohmann::json doc = {
      {"biz_name", "smart_doc_qa_v1"},
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", DefaultPipelineNodes()}};

  std::unique_ptr<ValidatedIoPlan> plan;
  std::string err;
  int ret =
      IoBindingResolver::ResolveFromPipelineJson(doc, "./models", &plan, &err);
  EXPECT_EQ(ret, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_NE(err.find("Pipeline biz_name 'smart_doc_qa_v1' does not match "
                     "binding biz_name 'test_biz_v1'"),
            std::string::npos)
      << "ACTUAL ERR: " << err;
  EXPECT_NE(err.find("(at /deployment/io/io_binding)"), std::string::npos)
      << "ACTUAL ERR: " << err;
}

TEST_F(IoBindingRegistryTest, EscapedJsonPointerInModelPathsAndSlots) {
  RegisterTestBizBinding();

  // 1. Slot name with special characters ~ and /
  nlohmann::json slot_doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"slot~0/bad", {{"type", "entity_out"}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", DefaultPipelineNodes()}};

  std::unique_ptr<ValidatedIoPlan> plan;
  std::string err;
  int ret = IoBindingResolver::ResolveFromPipelineJson(slot_doc, "./models",
                                                       &plan, &err);
  EXPECT_EQ(ret, -2);
  // slot~0/bad escaped: ~ -> ~0, / -> ~1 => slot~00~1bad
  EXPECT_NE(err.find("/deployment/io/output_allocations/slot~00~1bad"),
            std::string::npos)
      << "ACTUAL ERR: " << err;

  // 2. Unknown model ID with special characters ~ and /
  nlohmann::json model_doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"model_paths", {{"model~1/test", "path/to/model"}}},
        {"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", DefaultPipelineNodes()}};

  ret = IoBindingResolver::ResolveFromPipelineJson(model_doc, "./models", &plan,
                                                   &err);
  EXPECT_EQ(ret, -2);
  // model~1/test escaped: ~ -> ~0, / -> ~1 => model~01~1test
  EXPECT_NE(err.find("/deployment/model_paths/model~01~1test"),
            std::string::npos)
      << "ACTUAL ERR: " << err;
}

TEST_F(IoBindingRegistryTest,
       OverriddenModelPathResolutionFailurePointsToDeployment) {
  RegisterTestBizBinding();

  // Case 1: When model path override escapes model_root_dir, error points to
  // /deployment/model_paths/<mid>
  nlohmann::json doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"model_paths", {{"mid~test", "../../escaped_model.bin"}}},
        {"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models",
       {{{"model_id", "mid~test"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()},
         {"model_path", "models/legal.bin"}}}},
      {"pipeline", DefaultPipelineNodes()}};

  std::unique_ptr<ValidatedIoPlan> plan;
  std::string err;
  int ret =
      IoBindingResolver::ResolveFromPipelineJson(doc, "./models", &plan, &err);
  EXPECT_EQ(ret, -2);
  // mid~test escaped: mid~0test
  EXPECT_NE(err.find("/deployment/model_paths/mid~0test"), std::string::npos)
      << "ACTUAL ERR: " << err;
  EXPECT_EQ(err.find("/models/0/model_path"), std::string::npos)
      << "ACTUAL ERR: " << err;

  // Case 2: When an un-overridden model path escapes model_root_dir, error
  // points to /models/0/model_path
  nlohmann::json unoverridden_doc = doc;
  unoverridden_doc["deployment"].erase("model_paths");
  unoverridden_doc["models"][0]["model_path"] = "../../escaped_model.bin";
  ret = IoBindingResolver::ResolveFromPipelineJson(unoverridden_doc, "./models",
                                                   &plan, &err);
  EXPECT_EQ(ret, -2);
  EXPECT_NE(err.find("/models/0/model_path"), std::string::npos)
      << "ACTUAL ERR: " << err;
  EXPECT_EQ(err.find("/deployment/model_paths/"), std::string::npos)
      << "ACTUAL ERR: " << err;
}

TEST_F(IoBindingRegistryTest,
       PrepareDeploymentSuccessAndBoundaryExtraction_T01) {
  RegisterTestBizBinding();

  nlohmann::json doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"model_paths", {{"mid_1", "models/override.bin"}}},
        {"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
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

  // 1. With override
  EXPECT_TRUE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
  EXPECT_FALSE(prepared.neutral_pipeline_json.contains("deployment"));
  EXPECT_EQ(prepared.neutral_pipeline_json["biz_name"], "test_biz_v1");
  EXPECT_EQ(prepared.neutral_pipeline_json["models"][0]["model_path"],
            "models/override.bin");
  EXPECT_EQ(prepared.overridden_model_ids.count("mid_1"), 1u);
  ASSERT_EQ(prepared.model_path_source_pointers.size(), 1u);
  EXPECT_EQ(prepared.model_path_source_pointers[0],
            "/deployment/model_paths/mid_1");
  EXPECT_EQ(prepared.binding.binding_id, "test_biz.operator.v1");
  ASSERT_NE(prepared.input_converter, nullptr);
  ASSERT_NE(prepared.output_converter, nullptr);
  EXPECT_EQ(prepared.output_specs.count("entity_out"), 1u);
  EXPECT_EQ(prepared.io_boundary.input_published_ports.size(), 1u);
  EXPECT_EQ(prepared.io_boundary.input_published_ports[0].Name(),
            "input_sentences");
  EXPECT_EQ(prepared.io_boundary.output_consumed_ports.size(), 1u);
  EXPECT_EQ(prepared.io_boundary.output_consumed_ports[0].Name(),
            "llm_answers");

  // 2. Without override
  nlohmann::json doc_no_override = doc;
  doc_no_override["deployment"].erase("model_paths");
  prepared.Clear();
  EXPECT_TRUE(
      PrepareDeploymentDocument(doc_no_override, options, &prepared, &diag));
  EXPECT_EQ(prepared.neutral_pipeline_json["models"][0]["model_path"],
            "models/original.bin");
  EXPECT_EQ(prepared.overridden_model_ids.count("mid_1"), 0u);
  ASSERT_EQ(prepared.model_path_source_pointers.size(), 1u);
  EXPECT_EQ(prepared.model_path_source_pointers[0], "/models/0/model_path");
}

TEST_F(IoBindingRegistryTest, MissingDeploymentFails_T02) {
  RegisterTestBizBinding();

  nlohmann::json neutral_doc = {
      {"biz_name", "test_biz_v1"},
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

  // 1. Missing deployment fails in PrepareDeploymentDocument
  EXPECT_FALSE(
      PrepareDeploymentDocument(neutral_doc, options, &prepared, &diag));
  EXPECT_EQ(diag.code, "MISSING_DEPLOYMENT_IO");
  EXPECT_EQ(diag.path, "/deployment/io");

  // 2. Core direct validation of a document containing deployment fails with
  // unknown field
  nlohmann::json doc_with_deployment = neutral_doc;
  doc_with_deployment["deployment"] = nlohmann::json::object();
  ParsedPipelineConfig parsed_config;
  PipelineDiagnostic core_diag;
  EXPECT_FALSE(
      ParsePipelineConfig(doc_with_deployment, &parsed_config, &core_diag));
  EXPECT_EQ(core_diag.code, DiagnosticCode::kUnknownField);
  EXPECT_EQ(core_diag.path, "/deployment");

  // 3. Core direct validation of neutral doc succeeds
  EXPECT_TRUE(ParsePipelineConfig(neutral_doc, &parsed_config, &core_diag));
}

TEST_F(IoBindingRegistryTest,
       OriginalModelPathInvalidRejectedEvenWithOverride_T03) {
  RegisterTestBizBinding();

  nlohmann::json base_doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"model_paths", {{"mid_1", "models/valid_override.bin"}}},
        {"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
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

  // Subcase 1: model_path missing completely
  {
    nlohmann::json doc = base_doc;
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "MISSING_FIELD");
    EXPECT_EQ(diag.path, "/models/0/model_path");

    // Paired check without override: same result
    doc["deployment"].erase("model_paths");
    DeploymentDiagnostic diag_no_override;
    EXPECT_FALSE(
        PrepareDeploymentDocument(doc, options, &prepared, &diag_no_override));
    EXPECT_EQ(diag_no_override.code, "MISSING_FIELD");
    EXPECT_EQ(diag_no_override.path, "/models/0/model_path");
  }

  // Subcase 2: model_path is null
  {
    nlohmann::json doc = base_doc;
    doc["models"][0]["model_path"] = nullptr;
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "FIELD_TYPE");
    EXPECT_EQ(diag.path, "/models/0/model_path");

    doc["deployment"].erase("model_paths");
    DeploymentDiagnostic diag_no_override;
    EXPECT_FALSE(
        PrepareDeploymentDocument(doc, options, &prepared, &diag_no_override));
    EXPECT_EQ(diag_no_override.code, "FIELD_TYPE");
    EXPECT_EQ(diag_no_override.path, "/models/0/model_path");
  }

  // Subcase 3: model_path is integer
  {
    nlohmann::json doc = base_doc;
    doc["models"][0]["model_path"] = 12345;
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "FIELD_TYPE");
    EXPECT_EQ(diag.path, "/models/0/model_path");

    doc["deployment"].erase("model_paths");
    DeploymentDiagnostic diag_no_override;
    EXPECT_FALSE(
        PrepareDeploymentDocument(doc, options, &prepared, &diag_no_override));
    EXPECT_EQ(diag_no_override.code, "FIELD_TYPE");
    EXPECT_EQ(diag_no_override.path, "/models/0/model_path");
  }

  // Subcase 4: model_path is empty string
  {
    nlohmann::json doc = base_doc;
    doc["models"][0]["model_path"] = "";
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "FIELD_RANGE");
    EXPECT_EQ(diag.path, "/models/0/model_path");

    doc["deployment"].erase("model_paths");
    DeploymentDiagnostic diag_no_override;
    EXPECT_FALSE(
        PrepareDeploymentDocument(doc, options, &prepared, &diag_no_override));
    EXPECT_EQ(diag_no_override.code, "FIELD_RANGE");
    EXPECT_EQ(diag_no_override.path, "/models/0/model_path");
  }
}

TEST_F(IoBindingRegistryTest,
       OriginalModelStructureInvalidRejectedEvenWithOverride_T04) {
  RegisterTestBizBinding();

  DeploymentPrepareOptions options;

  options.path_mode = DeploymentPathMode::kLexicalOnly;
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  // Case 1: models is not an array (e.g. object)
  {
    nlohmann::json doc = {{"biz_name", "test_biz_v1"},
                          {"deployment",
                           {{"model_paths", {{"mid_1", "models/override.bin"}}},
                            {"io",
                             {{"io_binding", "test_biz.operator.v1"},
                              {"output_allocations",
                               {{"entity_out", {{"type", "entity_out"}}}}}}}}},
                          {"models", {{"mid_1", "not_an_array"}}},
                          {"pipeline", DefaultPipelineNodes()}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "FIELD_TYPE");
    EXPECT_EQ(diag.path, "/models");
  }

  // Case 2: an override cannot supply a missing required model_path.
  {
    nlohmann::json doc = {{"biz_name", "test_biz_v1"},
                          {"deployment",
                           {{"model_paths", {{"mid_1", "models/override.bin"}}},
                            {"io",
                             {{"io_binding", "test_biz.operator.v1"},
                              {"output_allocations",
                               {{"entity_out", {{"type", "entity_out"}}}}}}}}},
                          {"models",
                           {{{"model_id", "mid_1"},
                             {"model_type", "test_biz_embedding"},
                             {"backend", "test_tensor_backend"}}}},
                          {"pipeline", DefaultPipelineNodes()}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "MISSING_FIELD");
    EXPECT_EQ(diag.path, "/models/0/model_path");
  }

  // Case 3: Duplicate model_id
  {
    nlohmann::json doc = {{"biz_name", "test_biz_v1"},
                          {"deployment",
                           {{"model_paths", {{"mid_1", "models/override.bin"}}},
                            {"io",
                             {{"io_binding", "test_biz.operator.v1"},
                              {"output_allocations",
                               {{"entity_out", {{"type", "entity_out"}}}}}}}}},
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

TEST_F(IoBindingRegistryTest,
       OriginalModelPathNonexistentOnDiskWithValidOverride_T05) {
  RegisterTestBizBinding();

  // Create temporary directory with only the override model file
  fs::path temp_dir = fs::temp_directory_path() / "edgeflow_test_t05";
  fs::create_directories(temp_dir);
  fs::path override_file = temp_dir / "override_model.bin";
  {
    std::ofstream ofs(override_file);
    ofs << "dummy model data";
  }

  nlohmann::json doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"model_paths", {{"mid_1", "override_model.bin"}}},
        {"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models",
       {{{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()},
         {"model_path", "nonexistent_dir/completely_missing_original.bin"}}}},
      {"pipeline", DefaultPipelineNodes()}};

  DeploymentPrepareOptions options;

  options.path_mode = DeploymentPathMode::kUnderRoot;
  options.model_root_dir = temp_dir.string();

  PreparedDeployment prepared;
  DeploymentDiagnostic diag;
  // S2 checks original syntax only; S6 checks override path under root which
  // exists. Missing original model on disk must NOT cause failure!
  EXPECT_TRUE(PrepareDeploymentDocument(doc, options, &prepared, &diag))
      << diag.message;
  EXPECT_EQ(prepared.neutral_pipeline_json["models"][0]["model_path"],
            override_file.string());

  fs::remove_all(temp_dir);
}

TEST_F(IoBindingRegistryTest, OverrideUnknownModelIdOrInvalidSyntax_T06) {
  RegisterTestBizBinding();

  nlohmann::json base_doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
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

  // Case 1: Override unknown model_id
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["model_paths"] = {
        {"nonexistent_model", "models/foo.bin"}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "UNKNOWN_MODEL_ID");
    EXPECT_EQ(diag.path, "/deployment/model_paths/nonexistent_model");
  }

  // Case 2: Override unknown model_id with special chars (escaped pointer)
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["model_paths"] = {{"non/exist~id", "models/foo.bin"}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "UNKNOWN_MODEL_ID");
    EXPECT_EQ(diag.path, "/deployment/model_paths/non~1exist~0id");
  }

  // Case 3: Override value is non-string (e.g. integer)
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["model_paths"] = {{"mid_1", 999}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diag.path, "/deployment/model_paths/mid_1");
  }

  // Case 4: Override value is empty string
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["model_paths"] = {{"mid_1", ""}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diag.path, "/deployment/model_paths/mid_1");
  }
}

TEST_F(IoBindingRegistryTest, DeploymentIoUnknownBindingOrMismatch_T07) {
  RegisterTestBizBinding();

  nlohmann::json base_doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", DefaultPipelineNodes()}};

  DeploymentPrepareOptions options;

  options.path_mode = DeploymentPathMode::kLexicalOnly;
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  // Case 1: Unknown binding
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["io"]["io_binding"] = "completely_unknown_binding";
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "UNKNOWN_IO_BINDING");
    EXPECT_EQ(diag.path, "/deployment/io/io_binding");
  }

  // Case 2: Biz name mismatch
  {
    nlohmann::json doc = base_doc;
    doc["biz_name"] = "other_biz_v1";
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "BIZ_MISMATCH");
    EXPECT_EQ(diag.path, "/deployment/io/io_binding");
  }
}

TEST_F(IoBindingRegistryTest, DeploymentIoSlotValidation_T08) {
  RegisterTestBizBinding();

  nlohmann::json base_doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
              {"metadata_type_id", 0},
              {"capacities", {{"entities_json", 2047}}}}}}}}}}},
      {"models", nlohmann::json::array()},
      {"pipeline", DefaultPipelineNodes()}};

  DeploymentPrepareOptions options;

  options.path_mode = DeploymentPathMode::kLexicalOnly;
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  // Case 1: Missing required slot
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["io"]["output_allocations"].erase("entity_out");
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "MISSING_OUTPUT_SLOT");
    EXPECT_EQ(diag.path, "/deployment/io/output_allocations/entity_out");
  }

  // Case 2: Unknown slot in output_allocations
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["io"]["output_allocations"]["unexpected_extra_slot"] = {
        {"type", "entity_out"}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "UNKNOWN_OUTPUT_SLOT");
    EXPECT_EQ(diag.path,
              "/deployment/io/output_allocations/unexpected_extra_slot");
  }

  // Case 3: Slot allocation missing type field
  {
    nlohmann::json doc = base_doc;
    doc["deployment"]["io"]["output_allocations"]["entity_out"].erase("type");
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "INVALID_OUTPUT_ALLOCATION");
    EXPECT_EQ(diag.path, "/deployment/io/output_allocations/entity_out");
  }
}

TEST_F(IoBindingRegistryTest, PrepareFailureResetsPreparedStateAtomically_T18) {
  RegisterTestBizBinding();

  nlohmann::json valid_doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"model_paths", {{"mid_1", "models/override.bin"}}},
        {"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
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

  // 1. Initial successful preparation
  ASSERT_TRUE(PrepareDeploymentDocument(valid_doc, options, &prepared, &diag));
  EXPECT_FALSE(prepared.binding.binding_id.empty());
  EXPECT_NE(prepared.input_converter, nullptr);
  EXPECT_NE(prepared.output_converter, nullptr);
  EXPECT_FALSE(prepared.neutral_pipeline_json.is_null());
  EXPECT_FALSE(prepared.output_specs.empty());

  // 2. Reusing the SAME prepared instance on a failing document
  nlohmann::json invalid_doc = valid_doc;
  invalid_doc.erase("deployment");
  const nlohmann::json invalid_doc_copy = invalid_doc;

  EXPECT_FALSE(
      PrepareDeploymentDocument(invalid_doc, options, &prepared, &diag));

  // Verify all fields of prepared are completely reset
  EXPECT_TRUE(prepared.binding.binding_id.empty());
  EXPECT_EQ(prepared.input_converter, nullptr);
  EXPECT_EQ(prepared.output_converter, nullptr);
  EXPECT_TRUE(prepared.input_port_bindings.All().empty());
  EXPECT_TRUE(prepared.output_port_bindings.All().empty());
  EXPECT_EQ(prepared.effective_max_batch_size, 0u);
  EXPECT_TRUE(prepared.output_specs.empty());
  EXPECT_TRUE(prepared.output_parameter_texts.empty());
  EXPECT_TRUE(prepared.overridden_model_ids.empty());
  EXPECT_TRUE(prepared.model_path_source_pointers.empty());
  EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
  EXPECT_TRUE(prepared.io_boundary.input_published_ports.empty());
  EXPECT_TRUE(prepared.io_boundary.output_consumed_ports.empty());

  // Verify the input document was NOT mutated
  EXPECT_EQ(invalid_doc, invalid_doc_copy);
}

TEST_F(IoBindingRegistryTest,
       ProjectModelPathDiagnosticsProjectionAndRemediation) {
  PreparedDeployment prepared;
  prepared.overridden_model_ids = {"mid_0"};
  prepared.model_path_source_pointers = {
      "/deployment/model_paths/mid_0",
      "/models/1/model_path",
  };

  ValidationReport report;
  report.ok = false;

  // Diagnostic 1: Points to model 0 (overridden) with a fix patching model_path
  ValidationDiagnostic diag1;
  diag1.code = DiagnosticCode::kFieldRange;
  diag1.path = "/models/0/model_path";
  diag1.message = "File not found";
  ValidationRemediation rem1;
  ValidationFix fix1;
  fix1.id = "fix_model_0";
  fix1.patch = nlohmann::json::array({{{"op", "replace"},
                                       {"path", "/models/0/model_path"},
                                       {"value", "foo.bin"}}});
  rem1.fixes.push_back(fix1);
  diag1.remediation = rem1;
  report.diagnostics.push_back(diag1);

  // Diagnostic 2: Points to model 1 (un-overridden) with a fix patching
  // model_path
  ValidationDiagnostic diag2;
  diag2.code = DiagnosticCode::kFieldRange;
  diag2.path = "/models/1/model_path";
  diag2.message = "File not found 1";
  ValidationRemediation rem2;
  ValidationFix fix2;
  fix2.id = "fix_model_1";
  fix2.patch = nlohmann::json::array({{{"op", "replace"},
                                       {"path", "/models/1/model_path"},
                                       {"value", "bar.bin"}}});
  rem2.fixes.push_back(fix2);
  diag2.remediation = rem2;
  report.diagnostics.push_back(diag2);

  // Diagnostic 3: Non-model diagnostic
  ValidationDiagnostic diag3;
  diag3.code = DiagnosticCode::kMissingField;
  diag3.path = "/pipeline/0/inputs/text";
  diag3.message = "Port unbound";
  report.diagnostics.push_back(diag3);

  // Diagnostic 4: Special characters ~ and / in model ID
  ValidationDiagnostic diag4;
  diag4.code = DiagnosticCode::kFieldRange;
  diag4.path = "/models/2/model_path";
  diag4.message = "File not found 2";
  report.diagnostics.push_back(diag4);
  prepared.overridden_model_ids.insert("mid~special/0");
  prepared.model_path_source_pointers.push_back(
      "/deployment/model_paths/mid~0special~10");

  // Project diagnostics
  ProjectModelPathDiagnostics(prepared, &report);

  ASSERT_EQ(report.diagnostics.size(), 4u);

  // Diagnostic 1: Path projected to /deployment/model_paths/mid_0, and fix
  // targeting model_path removed!
  EXPECT_EQ(report.diagnostics[0].path, "/deployment/model_paths/mid_0");
  ASSERT_TRUE(report.diagnostics[0].remediation.has_value());
  EXPECT_TRUE(report.diagnostics[0].remediation->fixes.empty());

  // Diagnostic 2: Path stays /models/1/model_path, fix preserved!
  EXPECT_EQ(report.diagnostics[1].path, "/models/1/model_path");
  ASSERT_TRUE(report.diagnostics[1].remediation.has_value());
  ASSERT_EQ(report.diagnostics[1].remediation->fixes.size(), 1u);
  EXPECT_EQ(report.diagnostics[1].remediation->fixes[0].id, "fix_model_1");

  // Diagnostic 3: Unchanged
  EXPECT_EQ(report.diagnostics[2].path, "/pipeline/0/inputs/text");

  // Diagnostic 4: Escaped pointer with ~ and /
  EXPECT_EQ(report.diagnostics[3].path,
            "/deployment/model_paths/mid~0special~10");
}

TEST_F(IoBindingRegistryTest,
       ResolveFromPipelineJsonDiagnosticCarrier_T03_T06_T07_T08) {
  RegisterTestBizBinding();

  // T03: Invalid original model path with override through IoBindingResolver
  nlohmann::json t03_doc = {
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"model_paths", {{"mid_1", "models/override.bin"}}},
        {"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
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

  // T06: Unknown override model ID
  nlohmann::json t06_doc = t03_doc;
  t06_doc["models"][0]["model_path"] = "models/original.bin";
  t06_doc["deployment"]["model_paths"] = {{"unknown_mid", "models/foo.bin"}};
  rc = IoBindingResolver::ResolveFromPipelineJson(t06_doc, "./models", &plan,
                                                  &err, &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "UNKNOWN_MODEL_ID");
  EXPECT_EQ(diag.path, "/deployment/model_paths/unknown_mid");

  // T07: Unknown binding through IoBindingResolver
  nlohmann::json t07_doc = t03_doc;
  t07_doc["models"][0]["model_path"] = "models/original.bin";
  t07_doc["deployment"]["io"]["io_binding"] = "nonexistent.binding";
  rc = IoBindingResolver::ResolveFromPipelineJson(t07_doc, "./models", &plan,
                                                  &err, &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "UNKNOWN_IO_BINDING");
  EXPECT_EQ(diag.path, "/deployment/io/io_binding");

  // T08: Missing required output slot
  nlohmann::json t08_doc = t03_doc;
  t08_doc["models"][0]["model_path"] = "models/original.bin";
  t08_doc["deployment"]["io"]["output_allocations"].clear();
  rc = IoBindingResolver::ResolveFromPipelineJson(t08_doc, "./models", &plan,
                                                  &err, &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "MISSING_OUTPUT_SLOT");
  EXPECT_EQ(diag.path, "/deployment/io/output_allocations/entity_out");
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
      {"biz_name", "test_biz_v1"},
      {"deployment",
       {{"io",
         {{"io_binding", "test_biz.operator.v1"},
          {"output_allocations",
           {{"entity_out",
             {{"type", "entity_out"},
              {"meta_num", 0},
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

  // 1. Success case
  int rc = IoBindingResolver::ResolveFromFile(conf_path.string(), "", &plan,
                                              &err, &diag);
  EXPECT_EQ(rc, 0);
  EXPECT_NE(plan, nullptr);
  EXPECT_TRUE(diag.code.empty());

  // 2. T03 via file: missing raw model_path with override
  {
    nlohmann::json t03_pipe = base_pipeline;
    t03_pipe["models"] = {
        {{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()}}};
    t03_pipe["deployment"]["model_paths"] = {{"mid_1", "models/override.bin"}};
    write_file(pipe_path, t03_pipe);

    rc = IoBindingResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                            &diag);
    EXPECT_EQ(rc, -3);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "MISSING_FIELD");
    EXPECT_EQ(diag.path, "/models/0/model_path");
  }

  // 3. T06 via file: unknown override model ID
  {
    nlohmann::json t06_pipe = base_pipeline;
    t06_pipe["deployment"]["model_paths"] = {{"unknown_mid", "models/foo.bin"}};
    write_file(pipe_path, t06_pipe);

    rc = IoBindingResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                            &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "UNKNOWN_MODEL_ID");
    EXPECT_EQ(diag.path, "/deployment/model_paths/unknown_mid");
  }

  // 4. T07 via file: unknown io_binding
  {
    nlohmann::json t07_pipe = base_pipeline;
    t07_pipe["deployment"]["io"]["io_binding"] = "unregistered.binding";
    write_file(pipe_path, t07_pipe);

    rc = IoBindingResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                            &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "UNKNOWN_IO_BINDING");
    EXPECT_EQ(diag.path, "/deployment/io/io_binding");
  }

  // 5. T08 via file: missing required output slot
  {
    nlohmann::json t08_pipe = base_pipeline;
    t08_pipe["deployment"]["io"]["output_allocations"].clear();
    write_file(pipe_path, t08_pipe);

    rc = IoBindingResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                            &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "MISSING_OUTPUT_SLOT");
    EXPECT_EQ(diag.path, "/deployment/io/output_allocations/entity_out");
  }

  // Restore valid pipeline file
  write_file(pipe_path, base_pipeline);

  // 6. Non-existent conf file -> CONFIG_FILE_OPEN
  rc = IoBindingResolver::ResolveFromFile(
      (temp_dir / "nonexistent.conf").string(), "", &plan, &err, &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "CONFIG_FILE_OPEN");
  EXPECT_EQ(diag.path, "/");

  // 7. Malformed JSON in conf file -> JSON_PARSE
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

  // 8. Missing pipeline file referenced by conf -> DEPLOYMENT_ERROR at
  // /pipe_path
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

  // 9. Malformed JSON in pipeline file -> JSON_PARSE
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
