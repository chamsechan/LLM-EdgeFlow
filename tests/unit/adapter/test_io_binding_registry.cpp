#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "adapter/converter_authoring.h"
#include "adapter/deployment_io_config.h"
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

}  // namespace

class IoBindingRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IoConverterRegistry::Instance().ClearForTesting();
    IoBindingRegistry::Instance().ClearForTesting();

    // 注册基础转换器供测试
    InputConverterDefinition in_def;
    in_def.converter_id = "test.in.operator";
    in_def.transport = "operator";
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
    out_def.transport = "operator";
    out_def.schema_id = "out_schema";
    out_def.schema_version = 1;
    out_def.external_type = "CompanyOperatorEntityOutput";
    out_def.external_slots = {ExternalSlotDefinition(
        "entity_out", "CompanyOperatorEntityOutput", PortDirection::kOutput,
        true, "CompanyOperatorEntityOutput", "entity_out")};
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
};

TEST_F(IoBindingRegistryTest, RegisterAndAuditValidBinding) {
  auto& reg = IoBindingRegistry::Instance();

  IoBindingDefinition binding;
  binding.binding_id = "test_biz.operator.v1";
  binding.biz_name = "test_biz_v1";
  binding.transport = "operator";
  binding.input_converter_id = "test.in.operator";
  binding.output_converter_id = "test.out.operator";
  binding.input_ports = {{"texts", "input_sentences"}};
  binding.output_ports = {{"answers", "llm_answers"}};

  EXPECT_TRUE(reg.RegisterBinding(binding));

  BizExposureDefinition exposure;
  exposure.biz_name = "test_biz_v1";
  exposure.max_batch_size = 32;
  exposure.required_transports = {"operator"};
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
  bad_biz.transport = "operator";
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
  bad_conv.transport = "operator";
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
  exposure.required_transports = {"operator"};
  reg.RegisterExposure(exposure);

  std::vector<std::string> errors;
  EXPECT_FALSE(reg.Audit(&errors));
  bool found_missing_exp = false;
  for (const auto& e : errors) {
    if (e.find("lacks valid binding for required transport: operator") !=
        std::string::npos) {
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
  valid_binding.transport = "operator";
  valid_binding.input_converter_id = "test.in.operator";
  valid_binding.output_converter_id = "test.out.operator";
  valid_binding.input_ports = {{"texts", "input_sentences"}};
  valid_binding.output_ports = {{"answers", "llm_answers"}};
  EXPECT_TRUE(reg.RegisterBinding(valid_binding));

  BizExposureDefinition exposure;
  exposure.biz_name = "test_biz_v1";
  exposure.max_batch_size = 32;
  exposure.required_transports = {"operator"};
  EXPECT_TRUE(reg.RegisterExposure(exposure));

  // 单独 audit 合法绑定应当通过
  std::vector<std::string> errors;
  EXPECT_TRUE(reg.Audit(&errors));
  EXPECT_TRUE(errors.empty());

  // 2. 注册未被选择使用的非法绑定 (缺失必需输入映射)
  IoBindingDefinition illegal_binding;
  illegal_binding.binding_id = "unselected_bad.operator.v1";
  illegal_binding.biz_name = "test_biz_v1";
  illegal_binding.transport = "operator";
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
  EXPECT_TRUE(
      DeploymentIoConfig::Parse(valid_cfg, tmp_dir, "operator", &parsed, &err));
  EXPECT_EQ(parsed.pipe_path, "test.json");

  // 2. 拒绝旧 Schema 1 包装 (schema_version + data)
  nlohmann::json old_schema1 = {
      {"schema_version", 1},
      {"data",
       {{"pipe_path", "test.json"}, {"io_binding", "test_biz.operator.v1"}}}};
  EXPECT_FALSE(DeploymentIoConfig::Parse(old_schema1, tmp_dir, "operator",
                                         &parsed, &err));
  EXPECT_NE(err.find("Deprecated"), std::string::npos);

  // 3. 拒绝顶层未知字段
  nlohmann::json bad_field = valid_cfg;
  bad_field["extra_field"] = "foo";
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(bad_field, tmp_dir, "operator", &parsed, &err));

  // 4. 拒绝 cabi transport
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(valid_cfg, tmp_dir, "cabi", &parsed, &err));

  // 5. 路径逃逸拒绝
  nlohmann::json escape_cfg = {{"pipe_path", "../../../etc/passwd"}};
  EXPECT_FALSE(DeploymentIoConfig::Parse(escape_cfg, tmp_dir, "operator",
                                         &parsed, &err));

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
                                        base_dir.string(), "operator", &parsed,
                                        &err));
  EXPECT_EQ(parsed.resolved_pipe_path,
            fs::canonical(base_dir / "pipeline.json").string());

  // 2. 子目录文件 -> 成功
  EXPECT_TRUE(DeploymentIoConfig::Parse(make_conf("subdir/sub_pipeline.json"),
                                        base_dir.string(), "operator", &parsed,
                                        &err));
  EXPECT_EQ(parsed.resolved_pipe_path,
            fs::canonical(sub_dir / "sub_pipeline.json").string());

  // 3. 父目录逃逸 (../outside/outside_pipeline.json) -> 严格拒绝
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(make_conf("../outside/outside_pipeline.json"),
                                base_dir.string(), "operator", &parsed, &err));
  EXPECT_NE(err.find("escapes config directory"), std::string::npos);

  // 4. 兄弟目录逃逸 (../sibling/sibling_pipeline.json) -> 严格拒绝
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(make_conf("../sibling/sibling_pipeline.json"),
                                base_dir.string(), "operator", &parsed, &err));
  EXPECT_NE(err.find("escapes config directory"), std::string::npos);

  // 5. 符号链接逃逸 (位于 base_dir 内但指向根外) -> 严格拒绝
  EXPECT_FALSE(DeploymentIoConfig::Parse(make_conf("symlink_escape.json"),
                                         base_dir.string(), "operator", &parsed,
                                         &err));
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
  bool read_ok = DeploymentIoConfig::ReadFromFile(
      conf_file.string(), "operator", &cwd_parsed, &cwd_err);

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
  valid_binding.transport = "operator";
  valid_binding.input_converter_id = "test.in.operator";
  valid_binding.output_converter_id = "test.out.operator";
  valid_binding.input_ports = {{"texts", "input_sentences"}};
  valid_binding.output_ports = {{"answers", "llm_answers"}};
  EXPECT_TRUE(reg.RegisterBinding(valid_binding));

  BizExposureDefinition exposure;
  exposure.biz_name = "test_biz_v1";
  exposure.max_batch_size = 32;
  exposure.required_transports = {"operator"};
  EXPECT_TRUE(reg.RegisterExposure(exposure));

  // 注册一个未被任何曝光引用的非法绑定 (输入端口缺少必需端口)
  IoBindingDefinition unselected_bad_binding;
  unselected_bad_binding.binding_id = "unselected_bad.operator.v1";
  unselected_bad_binding.biz_name = "test_biz_v1";
  unselected_bad_binding.transport = "operator";
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
         {"ports",
          {{"inputs", {{"text", "in"}}}, {"outputs", {{"matches", "out"}}}}},
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
         {"ports",
          {{"inputs", {{"text", "in"}}}, {"outputs", {{"matches", "out"}}}}},
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

}  // namespace llm_edgeflow
