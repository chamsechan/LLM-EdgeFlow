#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "adapter/converter_authoring.h"
#include "adapter/deployment_io_config.h"
#include "adapter/io_binding_registry.h"
#include "adapter/io_binding_resolver.h"
#include "adapter/io_converter_registry.h"
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
    in_def.logical_ports = {
        NodePortDefinition("texts", "TextBatch", true, "1:1")};
    in_def.decode_fn = &DummyDecode;
    IoConverterRegistry::Instance().RegisterInputConverter(in_def);

    OutputConverterDefinition out_def;
    out_def.converter_id = "test.out.cabi";
    out_def.transport = "cabi";
    out_def.schema_id = "out_schema";
    out_def.schema_version = 1;
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

}  // namespace llm_edgeflow
