#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "adapter/converter_authoring.h"
#include "adapter/deployment_io_config.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_binding_resolver.h"
#include "adapter/io_converter_registry.h"
#include "adapter/shared_algorithm_runtime.h"
#include "core/pipeline_catalog.h"

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
    in_def.converter_id = "test.in.cabi";
    in_def.transport = "cabi";
    in_def.schema_id = "in_schema";
    in_def.schema_version = 1;
    in_def.external_type = "int";
    in_def.external_slots = {
        ExternalSlotDefinition("inputs", "int", PortDirection::kInput, true)};
    in_def.max_batch_size = 64;
    in_def.logical_ports = {
        NodePortDefinition("texts", "TextBatch", true, "1:1")};
    in_def.decode_fn = &DummyDecode;
    IoConverterRegistry::Instance().RegisterInputConverter(in_def);

    OutputConverterDefinition out_def;
    out_def.converter_id = "test.out.cabi";
    out_def.transport = "cabi";
    out_def.schema_id = "out_schema";
    out_def.schema_version = 1;
    out_def.external_type = "int";
    out_def.external_slots = {
        ExternalSlotDefinition("answers", "int", PortDirection::kOutput, true)};
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
  binding.binding_id = "test_biz.cabi.v1";
  binding.biz_name = "test_biz_v1";
  binding.transport = "cabi";
  binding.input_converter_id = "test.in.cabi";
  binding.output_converter_id = "test.out.cabi";
  binding.input_ports = {{"texts", "input_sentences"}};
  binding.output_ports = {{"answers", "llm_answers"}};

  EXPECT_TRUE(reg.RegisterBinding(binding));

  BizExposureDefinition exposure;
  exposure.biz_name = "test_biz_v1";
  exposure.max_batch_size = 32;
  exposure.required_transports = {"cabi"};
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
  bad_biz.transport = "cabi";
  bad_biz.input_converter_id = "test.in.cabi";
  bad_biz.output_converter_id = "test.out.cabi";
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
  bad_conv.transport = "cabi";
  bad_conv.input_converter_id = "non_existent_input";
  bad_conv.output_converter_id = "test.out.cabi";
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
  valid_binding.binding_id = "test_biz.cabi.v1";
  valid_binding.biz_name = "test_biz_v1";
  valid_binding.transport = "cabi";
  valid_binding.input_converter_id = "test.in.cabi";
  valid_binding.output_converter_id = "test.out.cabi";
  valid_binding.input_ports = {{"texts", "input_sentences"}};
  valid_binding.output_ports = {{"answers", "llm_answers"}};
  EXPECT_TRUE(reg.RegisterBinding(valid_binding));

  BizExposureDefinition exposure;
  exposure.biz_name = "test_biz_v1";
  exposure.max_batch_size = 32;
  exposure.required_transports = {"cabi"};
  EXPECT_TRUE(reg.RegisterExposure(exposure));

  // 单独 audit 合法绑定应当通过
  std::vector<std::string> errors;
  EXPECT_TRUE(reg.Audit(&errors));
  EXPECT_TRUE(errors.empty());

  // 2. 注册未被选择使用的非法绑定 (缺失必需输入映射)
  IoBindingDefinition illegal_binding;
  illegal_binding.binding_id = "unselected_bad.cabi.v1";
  illegal_binding.biz_name = "test_biz_v1";
  illegal_binding.transport = "cabi";
  illegal_binding.input_converter_id = "test.in.cabi";
  illegal_binding.output_converter_id = "test.out.cabi";
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
  // 1. 合法 schema 1 C ABI 配置
  nlohmann::json valid_cfg = {
      {"schema_version", 1},
      {"data",
       {{"pipe_path", "test.json"}, {"io_binding", "test_biz.cabi.v1"}}}};

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
      DeploymentIoConfig::Parse(valid_cfg, tmp_dir, "cabi", &parsed, &err));
  EXPECT_EQ(parsed.pipe_path, "test.json");
  EXPECT_EQ(parsed.io_binding, "test_biz.cabi.v1");

  // 2. 拒绝未知 schema_version
  nlohmann::json bad_ver = valid_cfg;
  bad_ver["schema_version"] = 2;
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(bad_ver, tmp_dir, "cabi", &parsed, &err));

  // 3. 拒绝顶层未知字段
  nlohmann::json bad_field = valid_cfg;
  bad_field["extra_field"] = "foo";
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(bad_field, tmp_dir, "cabi", &parsed, &err));

  // 4. C ABI 拒绝 outputs
  nlohmann::json cabi_with_outputs = valid_cfg;
  cabi_with_outputs["data"]["outputs"] = nlohmann::json::object();
  EXPECT_FALSE(DeploymentIoConfig::Parse(cabi_with_outputs, tmp_dir, "cabi",
                                         &parsed, &err));

  // 5. 路径逃逸拒绝
  nlohmann::json escape_cfg = valid_cfg;
  escape_cfg["data"]["pipe_path"] = "../../../etc/passwd";
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(escape_cfg, tmp_dir, "cabi", &parsed, &err));

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
    nlohmann::json cfg = {
        {"schema_version", 1},
        {"data", {{"pipe_path", pipe}, {"io_binding", "test_biz.cabi.v1"}}}};
    return cfg;
  };

  // 1. 同级文件 -> 成功
  EXPECT_TRUE(DeploymentIoConfig::Parse(
      make_conf("pipeline.json"), base_dir.string(), "cabi", &parsed, &err));
  EXPECT_EQ(parsed.resolved_pipe_path,
            fs::canonical(base_dir / "pipeline.json").string());

  // 2. 子目录文件 -> 成功
  EXPECT_TRUE(DeploymentIoConfig::Parse(make_conf("subdir/sub_pipeline.json"),
                                        base_dir.string(), "cabi", &parsed,
                                        &err));
  EXPECT_EQ(parsed.resolved_pipe_path,
            fs::canonical(sub_dir / "sub_pipeline.json").string());

  // 3. 父目录逃逸 (../outside/outside_pipeline.json) -> 严格拒绝
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(make_conf("../outside/outside_pipeline.json"),
                                base_dir.string(), "cabi", &parsed, &err));
  EXPECT_NE(err.find("escapes config directory"), std::string::npos);

  // 4. 兄弟目录逃逸 (../sibling/sibling_pipeline.json) -> 严格拒绝
  EXPECT_FALSE(
      DeploymentIoConfig::Parse(make_conf("../sibling/sibling_pipeline.json"),
                                base_dir.string(), "cabi", &parsed, &err));
  EXPECT_NE(err.find("escapes config directory"), std::string::npos);

  // 5. 符号链接逃逸 (位于 base_dir 内但指向根外) -> 严格拒绝
  EXPECT_FALSE(DeploymentIoConfig::Parse(make_conf("symlink_escape.json"),
                                         base_dir.string(), "cabi", &parsed,
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
  bool read_ok = DeploymentIoConfig::ReadFromFile(conf_file.string(), "cabi",
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
  valid_binding.binding_id = "test_biz.cabi.v1";
  valid_binding.biz_name = "test_biz_v1";
  valid_binding.transport = "cabi";
  valid_binding.input_converter_id = "test.in.cabi";
  valid_binding.output_converter_id = "test.out.cabi";
  valid_binding.input_ports = {{"texts", "input_sentences"}};
  valid_binding.output_ports = {{"answers", "llm_answers"}};
  EXPECT_TRUE(reg.RegisterBinding(valid_binding));

  BizExposureDefinition exposure;
  exposure.biz_name = "test_biz_v1";
  exposure.max_batch_size = 32;
  exposure.required_transports = {"cabi"};
  EXPECT_TRUE(reg.RegisterExposure(exposure));

  // 注册一个未被任何曝光引用的非法绑定 (输入端口缺少必需端口)
  IoBindingDefinition unselected_bad_binding;
  unselected_bad_binding.binding_id = "unselected_bad.cabi.v1";
  unselected_bad_binding.biz_name = "test_biz_v1";
  unselected_bad_binding.transport = "cabi";
  unselected_bad_binding.input_converter_id = "test.in.cabi";
  unselected_bad_binding.output_converter_id = "test.out.cabi";
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

}  // namespace llm_edgeflow
