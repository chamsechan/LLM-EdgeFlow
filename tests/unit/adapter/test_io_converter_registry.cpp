#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <thread>

#include "adapter/converter_authoring.h"
#include "adapter/deployment_diagnostic.h"
#include "adapter/deployment_io_config.h"
#include "adapter/deployment_preparation.h"
#include "adapter/io_catalog.h"
#include "adapter/io_converter_registry.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/pipeline_document.h"
#include "adapter/shared_algorithm_runtime.h"
#include "contracts/parameters.h"
#include "contracts/registry_conflicts.h"
#include "core/pipeline_config.h"
#include "edgeflow/operator/interface.h"
#include "tests/support/scoped_allocation_failure.h"

namespace llm_edgeflow {
namespace {

namespace fs = std::filesystem;

// 测试登记使用的占位 service_type；registry
// 在每个用例前清空，不与生产登记冲突。
constexpr int32_t kTestServiceType = 9001;

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

// 输出尺寸参数：默认值 2047，上限由平台结构登记决定。
struct SizeParams {
  int64_t entities_json_max_bytes{};
};

auto SizeSpec() {
  return Parameters<SizeParams>(
      {MaxBytes("entities_json", &SizeParams::entities_json_max_bytes)
           .Default(2047)});
}

// 行为参数：默认值、范围、枚举和 Create 阶段的 Prepare / Validate。
struct BehaviorParams {
  std::string mode;
  int64_t limit{};
  int64_t doubled{};  // 由 Prepare 推导，不对应配置项
};

auto BehaviorSpec() {
  auto spec = Parameters<BehaviorParams>(
      {Field("mode", &BehaviorParams::mode).Default("a").Enum({"a", "b"}),
       Field("limit", &BehaviorParams::limit).Default(3).Range(1, 10)});
  spec.Prepare([](BehaviorParams* p, std::string* error) {
    if (p->limit == 9) {
      *error = "limit 9 is reserved";
      return false;
    }
    p->doubled = p->limit * 2;
    return true;
  });
  spec.Validate([](const BehaviorParams& p, std::string* error) {
    if (p.mode == "b" && p.limit > 5) {
      *error = "mode b requires limit <= 5";
      return false;
    }
    return true;
  });
  return spec;
}

struct ObservedParams {
  std::string mode;
  int64_t limit = 0;
  int64_t doubled = 0;
  int calls = 0;
};
ObservedParams g_observed;

int BehaviorDecode(const ExternalInputBatchView&,
                   const InputDecodeOptions& options, AlgContext*,
                   AdapterStatus*) {
  const auto& p = options.Params<BehaviorParams>();
  g_observed = {p.mode, p.limit, p.doubled, g_observed.calls + 1};
  return 0;
}

// 读取同一份只读参数的输出逐行回调所需的尺寸，Prepare 可以改写它。
struct DerivedSizeParams {
  int64_t base{};
  int64_t entities_json_max_bytes{};
};

auto DerivedSizeSpec() {
  auto spec = Parameters<DerivedSizeParams>(
      {Field("base", &DerivedSizeParams::base).Default(100).Range(1, 5000),
       MaxBytes("entities_json", &DerivedSizeParams::entities_json_max_bytes)
           .Default(10)});
  spec.Prepare([](DerivedSizeParams* p, std::string*) {
    p->entities_json_max_bytes = p->base * 4;
    return true;
  });
  return spec;
}

}  // namespace

class IoConverterRegistryTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    saved_inputs_ = IoConverterRegistry::Instance().AllInputConverters();
    saved_outputs_ = IoConverterRegistry::Instance().AllOutputConverters();
  }

  static void TearDownTestSuite() {
    IoConverterRegistry::Instance().ClearForTesting();
    for (const auto& in_def : saved_inputs_) {
      IoConverterRegistry::Instance().RegisterInputConverter(in_def);
    }
    for (const auto& out_def : saved_outputs_) {
      IoConverterRegistry::Instance().RegisterOutputConverter(out_def);
    }
  }

  void SetUp() override {
    IoConverterRegistry::Instance().ClearForTesting();
    ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(
        MakeInput("test_biz")));
    ASSERT_TRUE(IoConverterRegistry::Instance().RegisterOutputConverter(
        MakeOutput("test_biz")));
  }

  void TearDown() override {
    IoConverterRegistry::Instance().ClearForTesting();
  }

  static InputConverterDefinition MakeInput(const std::string& name) {
    InputConverterDefinition def;
    def.type = "entity_in";
    def.name = name;
    def.service_type = kTestServiceType;
    def.slot = ExternalInputSlot<CompanyOperatorEntityInput>("entity_in");
    def.logical_ports = {
        NodePortDefinition("input_sentences", "TextBatch", true, "1:1")};
    def.decode_fn = &DummyDecode;
    return def;
  }

  static OutputConverterDefinition MakeOutput(const std::string& name) {
    OutputConverterDefinition def;
    def.type = "entity_out";
    def.name = name;
    def.service_type = kTestServiceType;
    def.slot = ExternalOutputSlot<CompanyOperatorEntityOutput>("entity_out");
    def.logical_ports = {
        NodePortDefinition("llm_answers", "TextBatch", true, "1:1")};
    def.params = SizeSpec();
    def.encode_fn = &DummyEncode;
    return def;
  }

  // 一个输入项、一个输出项的方案；extra 合并进对应项（例如 params）。
  static nlohmann::json Document(
      const nlohmann::json& input_extra = nlohmann::json::object(),
      const nlohmann::json& output_extra = nlohmann::json::object()) {
    nlohmann::json input = {{"type", "entity_in"}, {"name", "test_biz"}};
    nlohmann::json output = {{"type", "entity_out"}, {"name", "test_biz"}};
    input.update(input_extra);
    output.update(output_extra);
    return {{"io", {{"input", {input}}, {"output", {output}}}},
            {"pipeline", DefaultPipelineNodes()}};
  }

  // 全量审计的错误里是否出现 text。
  static bool AuditMentions(const std::string& text) {
    std::vector<std::string> errors;
    EXPECT_FALSE(IoConverterRegistry::Instance().Audit(&errors));
    return std::any_of(errors.begin(), errors.end(), [&](const auto& error) {
      return error.find(text) != std::string::npos;
    });
  }

  static inline std::vector<InputConverterDefinition> saved_inputs_;
  static inline std::vector<OutputConverterDefinition> saved_outputs_;
};

TEST_F(IoConverterRegistryTest, RegistryConflictsStayRecordedWithoutMessage) {
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

// ---------------------------------------------------------------------------
// 注册审计
// ---------------------------------------------------------------------------

TEST_F(IoConverterRegistryTest, RegisteredConvertersPassAudit) {
  std::vector<std::string> errors;
  EXPECT_TRUE(IoConverterRegistry::Instance().Audit(&errors));
  EXPECT_TRUE(errors.empty());
}

TEST_F(IoConverterRegistryTest, RegistrationConflictsAreRecordedAndAudited) {
  auto& registry = IoConverterRegistry::Instance();

  EXPECT_FALSE(registry.RegisterInputConverter(MakeInput("test_biz")));
  EXPECT_TRUE(
      AuditMentions("Duplicate InputConverter registration: "
                    "entity_in/test_biz"));
  registry.ResetConflictForTesting();

  // 同一方向、同一 type 下 service_type 取值重复。
  auto same_value = MakeOutput("another_biz");
  EXPECT_FALSE(registry.RegisterOutputConverter(same_value));
  EXPECT_TRUE(
      AuditMentions("Duplicate service_type 9001 for OutputConverter "
                    "type: entity_out"));
  registry.ResetConflictForTesting();

  auto empty_type = MakeInput("x");
  empty_type.type.clear();
  EXPECT_FALSE(registry.RegisterInputConverter(empty_type));
  auto empty_name = MakeInput("");
  EXPECT_FALSE(registry.RegisterInputConverter(empty_name));
  auto no_decode = MakeInput("no_decode");
  no_decode.service_type = 9002;
  no_decode.decode_fn = nullptr;
  EXPECT_FALSE(registry.RegisterInputConverter(no_decode));
  auto no_encode = MakeOutput("no_encode");
  no_encode.service_type = 9002;
  no_encode.encode_fn = nullptr;
  EXPECT_FALSE(registry.RegisterOutputConverter(no_encode));
  EXPECT_TRUE(AuditMentions("Empty type or name"));
  EXPECT_TRUE(AuditMentions("Missing decode_fn"));
  EXPECT_TRUE(AuditMentions("Missing encode_fn"));
}

// 槽声明的宿主结构须与其后缀登记的结构一致，否则会按错误的布局读写内存。
TEST_F(IoConverterRegistryTest, SlotStructMustMatchRegisteredValueType) {
  auto& registry = IoConverterRegistry::Instance();
  auto input = MakeInput("mismatch");
  input.type = "keyword_in";
  input.service_type = 9002;
  input.slot = ExternalInputSlot<CompanyOperatorEntityInput>("keyword_in");
  ASSERT_TRUE(registry.RegisterInputConverter(input));
  auto output = MakeOutput("mismatch");
  output.type = "keyword_out";
  output.service_type = 9002;
  output.slot = ExternalOutputSlot<CompanyOperatorEntityOutput>("keyword_out");
  ASSERT_TRUE(registry.RegisterOutputConverter(output));

  EXPECT_TRUE(AuditMentions(
      "Converter 'keyword_in/mismatch' input slot declares "
      "CompanyOperatorEntityInput but ValueType suffix 'keyword_in' is "
      "registered for CompanyOperatorKeywordInput"));
  EXPECT_TRUE(AuditMentions(
      "Converter 'keyword_out/mismatch' output slot declares "
      "CompanyOperatorEntityOutput but ValueType suffix 'keyword_out' is "
      "registered for CompanyOperatorKeywordOutput"));
}

TEST_F(IoConverterRegistryTest, SlotSuffixMustEqualType) {
  auto input = MakeInput("renamed");
  input.type = "entity_in_renamed";
  input.service_type = 9002;
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(input));
  EXPECT_TRUE(
      AuditMentions("slot type_suffix 'entity_in' differs from its type"));
}

TEST_F(IoConverterRegistryTest, UnregisteredValueTypeFailsAudit) {
  auto input = MakeInput("unknown_struct");
  input.type = "no_such_struct";
  input.service_type = std::nullopt;
  input.slot.type_suffix = "no_such_struct";
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(input));
  EXPECT_TRUE(
      AuditMentions("uses unregistered ValueType suffix: no_such_struct"));
}

TEST_F(IoConverterRegistryTest, ServiceTypeMustMatchStructDeclaration) {
  auto& registry = IoConverterRegistry::Instance();
  // 结构体有 service_type 成员：非 common 登记必须填写。
  auto missing = MakeInput("missing_value");
  missing.service_type = std::nullopt;
  ASSERT_TRUE(registry.RegisterInputConverter(missing));
  EXPECT_TRUE(
      AuditMentions("Converter 'entity_in/missing_value' must declare "
                    "service_type"));
  registry.ClearForTesting();

  // common 表示默认处理，不核对 service_type，因此不能填写。
  auto common = MakeInput(kCommonIoName);
  ASSERT_TRUE(registry.RegisterInputConverter(common));
  EXPECT_TRUE(
      AuditMentions("Converter 'entity_in/common' must not declare "
                    "service_type"));
  registry.ClearForTesting();

  // common 不填 service_type 则合法。
  common.service_type = std::nullopt;
  ASSERT_TRUE(registry.RegisterInputConverter(common));
  std::vector<std::string> errors;
  EXPECT_TRUE(registry.Audit(&errors))
      << (errors.empty() ? "" : errors.front());
  registry.ClearForTesting();

  // 结构体没有 service_type 成员（CompanyString）：不能填写。
  InputConverterDefinition text;
  text.type = "string";
  text.name = "text_only";
  text.service_type = 1;
  text.slot = ExternalInputSlot<CompanyString>("string");
  text.logical_ports = {NodePortDefinition("question", "TextBatch", true)};
  text.decode_fn = &DummyDecode;
  ASSERT_TRUE(registry.RegisterInputConverter(text));
  EXPECT_TRUE(AuditMentions("its host struct has no service_type member"));
  registry.ClearForTesting();
  text.service_type = std::nullopt;
  ASSERT_TRUE(registry.RegisterInputConverter(text));
  EXPECT_TRUE(registry.Audit(&errors))
      << (errors.empty() ? "" : errors.front());
}

TEST_F(IoConverterRegistryTest, SizeParametersMustCoverEveryStringField) {
  auto& registry = IoConverterRegistry::Instance();
  auto no_sizes = MakeOutput("no_sizes");
  no_sizes.service_type = 9002;
  no_sizes.params = ParameterSet();
  ASSERT_TRUE(registry.RegisterOutputConverter(no_sizes));
  EXPECT_TRUE(
      AuditMentions("lacks integer parameter 'entities_json_max_bytes' "
                    "for output string field 'entities_json'"));
  registry.ClearForTesting();

  // 默认值超过平台上限。
  struct Params {
    int64_t entities_json_max_bytes{};
  };
  auto too_big = MakeOutput("too_big");
  too_big.params = Parameters<Params>(
      {Field("entities_json_max_bytes", &Params::entities_json_max_bytes)
           .Default(65537)});
  ASSERT_TRUE(registry.RegisterOutputConverter(too_big));
  EXPECT_TRUE(
      AuditMentions("parameter 'entities_json_max_bytes' default 65537 "
                    "is outside [1, 65536] (platform limit)"));
}

TEST_F(IoConverterRegistryTest, NamedLayoutParametersAndMetadataAreAudited) {
  auto& registry = IoConverterRegistry::Instance();
  // 结构体没有 metadata 成员却声明了 metadata_count。
  auto metadata = MakeOutput("metadata");
  metadata.service_type = 9002;
  metadata.slot.metadata_count = 1;
  metadata.slot.metadata_type_id = 1;
  ASSERT_TRUE(registry.RegisterOutputConverter(metadata));
  EXPECT_TRUE(AuditMentions("output slot declaration is invalid"));
  registry.ClearForTesting();

  // 标准布局不接受布局参数。
  auto params = MakeOutput("layout_params");
  params.slot.allocator_params = R"({"capacity": 4})";
  ASSERT_TRUE(registry.RegisterOutputConverter(params));
  EXPECT_TRUE(AuditMentions("allocator_params rejected by allocator"));
  registry.ClearForTesting();

  // 命名布局不存在。
  auto layout = MakeOutput("missing_layout");
  layout.slot.allocator = "no_such_layout";
  ASSERT_TRUE(registry.RegisterOutputConverter(layout));
  EXPECT_TRUE(
      AuditMentions("has no registered ValueType for suffix "
                    "'entity_out' and allocator 'no_such_layout'"));
}

TEST_F(IoConverterRegistryTest, DuplicateOrEmptyPortsAreAudited) {
  auto& registry = IoConverterRegistry::Instance();
  auto input = MakeInput("dup_ports");
  input.service_type = 9002;
  input.logical_ports = {NodePortDefinition("same", "TextBatch", true),
                         NodePortDefinition("same", "TextBatch", true)};
  ASSERT_TRUE(registry.RegisterInputConverter(input));
  EXPECT_TRUE(AuditMentions("declares logical port twice: same"));
  registry.ClearForTesting();

  auto empty_ports = MakeInput("no_ports");
  empty_ports.logical_ports.clear();
  EXPECT_FALSE(registry.RegisterInputConverter(empty_ports));
  EXPECT_TRUE(AuditMentions("Empty logical_ports"));
}

// 即使没有任何方案使用，登记的槽结构不一致也使 Init 失败，并写明 type/name。
TEST_F(IoConverterRegistryTest, UnselectedIllegalConverterFailsInit) {
  std::string init_diagnostic;
  EXPECT_EQ(SharedAlgorithmRuntime::GlobalInit(&init_diagnostic), 0)
      << init_diagnostic;

  auto unused = MakeInput("unused_but_wrong");
  unused.type = "keyword_in";
  unused.service_type = 9002;
  unused.slot = ExternalInputSlot<CompanyOperatorEntityInput>("keyword_in");
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(unused));

  EXPECT_EQ(SharedAlgorithmRuntime::GlobalInit(&init_diagnostic),
            COMPANY_ALG_ERR_REGISTRY_CONFLICT);
  EXPECT_NE(init_diagnostic.find("Converter audit: Converter "
                                 "'keyword_in/unused_but_wrong'"),
            std::string::npos)
      << init_diagnostic;
}

// ---------------------------------------------------------------------------
// io 文档结构
// ---------------------------------------------------------------------------

TEST_F(IoConverterRegistryTest, SplitPipelineDocumentAndCoreBoundary) {
  nlohmann::json document = Document();
  document["models"] = nlohmann::json::array();
  document["max_parallel_workers"] = 2;
  PipelineDocumentSplit split;
  std::string error;
  ASSERT_TRUE(SplitPipelineDocument(document, &split, &error)) << error;
  ASSERT_EQ(split.io.input.size(), 1U);
  EXPECT_EQ(split.io.input[0].type, "entity_in");
  EXPECT_EQ(split.io.input[0].name, "test_biz");
  EXPECT_TRUE(split.io.input[0].params.empty());
  ASSERT_EQ(split.io.output.size(), 1U);
  EXPECT_EQ(split.io.output[0].type, "entity_out");
  EXPECT_FALSE(split.core_json.contains("io"));
  EXPECT_EQ(split.core_json["models"], document["models"]);
  EXPECT_EQ(split.core_json["max_parallel_workers"], 2);
  EXPECT_EQ(split.core_json["pipeline"], document["pipeline"]);

  // io 由接入层独占解析：Core 看到它按未知根字段处理。
  ParsedPipelineConfig parsed;
  PipelineDiagnostic core_diagnostic;
  EXPECT_FALSE(ParsePipelineConfig(document, &parsed, &core_diagnostic));
  EXPECT_EQ(core_diagnostic.code, DiagnosticCode::kUnknownField);
  EXPECT_EQ(core_diagnostic.path, "/io");
  EXPECT_TRUE(ParsePipelineConfig(split.core_json, &parsed, &core_diagnostic));
  EXPECT_EQ(parsed.max_parallel_workers, 2);

  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  EXPECT_FALSE(prepared.neutral_pipeline_json.contains("io"));
  EXPECT_EQ(prepared.neutral_pipeline_json["pipeline"], document["pipeline"]);

  auto typo = document;
  typo["iO"] = nlohmann::json::object();
  EXPECT_FALSE(PrepareDeploymentDocument(typo, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diagnostic.path, "/iO");
}

TEST_F(IoConverterRegistryTest, IoStructureErrorsPointToTheOffendingPosition) {
  struct Case {
    const char* name;
    std::function<void(nlohmann::json*)> mutate;
    const char* path;
  };
  const std::vector<Case> cases = {
      {"missing io", [](auto* d) { d->erase("io"); }, "/io"},
      {"io not an object", [](auto* d) { (*d)["io"] = 1; }, "/io"},
      {"unknown io key", [](auto* d) { (*d)["io"]["extra"] = 1; }, "/io/extra"},
      {"missing input", [](auto* d) { (*d)["io"].erase("input"); },
       "/io/input"},
      {"missing output", [](auto* d) { (*d)["io"].erase("output"); },
       "/io/output"},
      {"input not an array", [](auto* d) { (*d)["io"]["input"] = "x"; },
       "/io/input"},
      {"empty input",
       [](auto* d) { (*d)["io"]["input"] = nlohmann::json::array(); },
       "/io/input"},
      {"empty output",
       [](auto* d) { (*d)["io"]["output"] = nlohmann::json::array(); },
       "/io/output"},
      {"item not an object", [](auto* d) { (*d)["io"]["input"][0] = "x"; },
       "/io/input/0"},
      {"item extra key", [](auto* d) { (*d)["io"]["output"][0]["extra"] = 1; },
       "/io/output/0/extra"},
      {"item missing type",
       [](auto* d) { (*d)["io"]["input"][0].erase("type"); },
       "/io/input/0/type"},
      {"item missing name",
       [](auto* d) { (*d)["io"]["input"][0].erase("name"); },
       "/io/input/0/name"},
      {"item type not a string",
       [](auto* d) { (*d)["io"]["input"][0]["type"] = 3; }, "/io/input/0/type"},
      {"item empty name", [](auto* d) { (*d)["io"]["output"][0]["name"] = ""; },
       "/io/output/0/name"},
      {"params not an object",
       [](auto* d) { (*d)["io"]["output"][0]["params"] = 1; },
       "/io/output/0/params"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    auto document = Document();
    test.mutate(&document);
    PipelineDocumentSplit split;
    std::string error;
    std::string path;
    EXPECT_FALSE(SplitPipelineDocument(document, &split, &error, &path));
    EXPECT_EQ(path, test.path) << error;

    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(
        PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
    EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diagnostic.path, test.path);
  }
}

TEST_F(IoConverterRegistryTest,
       UnknownExternalRootFieldsAreRejectedWithEscapedPointers) {
  const nlohmann::json valid_document = Document();
  struct UnknownRootField {
    const char* name;
    const char* pointer;
  };
  const UnknownRootField fields[] = {
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
    EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(
                  document, "./models", &plan, &error, &diagnostic),
              -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diagnostic.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diagnostic.path, field.pointer);
    EXPECT_NE(error.find(expected_message), std::string::npos);
    EXPECT_EQ(document[field.name], "test_biz");
  }
}

// ---------------------------------------------------------------------------
// 选择登记
// ---------------------------------------------------------------------------

TEST_F(IoConverterRegistryTest,
       UnknownConverterListsTheNamesOfARegisteredType) {
  auto& registry = IoConverterRegistry::Instance();
  auto other = MakeInput("other_biz");
  other.service_type = 9002;
  ASSERT_TRUE(registry.RegisterInputConverter(other));

  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  auto document = Document({{"name", "no_such_biz"}});
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "UNKNOWN_CONVERTER");
  EXPECT_EQ(diagnostic.path, "/io/input/0");
  EXPECT_NE(diagnostic.message.find("entity_in/no_such_biz"),
            std::string::npos);
  EXPECT_NE(diagnostic.message.find("other_biz, test_biz"), std::string::npos)
      << diagnostic.message;

  // type 未登记：列出该方向已登记的 type。
  document = Document({{"type", "no_such_struct"}});
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "UNKNOWN_CONVERTER");
  EXPECT_NE(diagnostic.message.find("registered input types: entity_in"),
            std::string::npos)
      << diagnostic.message;

  // 方向用错：输出 type 写在输入侧。
  document = Document({{"type", "entity_out"}});
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "UNKNOWN_CONVERTER");
  EXPECT_EQ(diagnostic.path, "/io/input/0");

  // 输出侧同样检查，路径指向输出项。
  document = Document(nlohmann::json::object(), {{"name", "no_such_biz"}});
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "UNKNOWN_CONVERTER");
  EXPECT_EQ(diagnostic.path, "/io/output/0");
}

TEST_F(IoConverterRegistryTest, CommonIsSelectedExplicitlyAndNeverImplied) {
  auto common = MakeInput(kCommonIoName);
  common.service_type = std::nullopt;
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(common));

  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(Document({{"name", "common"}}), {},
                                        &prepared, &diagnostic))
      << diagnostic.message;
  ASSERT_EQ(prepared.inputs.size(), 1U);
  EXPECT_EQ(prepared.inputs[0].converter->name, "common");
  EXPECT_FALSE(prepared.inputs[0].converter->service_type.has_value());

  // 输出侧没有 common 登记：报 UNKNOWN_CONVERTER，不退回其他登记。
  EXPECT_FALSE(PrepareDeploymentDocument(
      Document(nlohmann::json::object(), {{"name", "common"}}), {}, &prepared,
      &diagnostic));
  EXPECT_EQ(diagnostic.code, "UNKNOWN_CONVERTER");
  EXPECT_EQ(diagnostic.path, "/io/output/0");
}

TEST_F(IoConverterRegistryTest,
       DuplicatePairsRejectedAndDistinctBusinessesAllowed) {
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  auto document = Document();
  document["io"]["input"].push_back(document["io"]["input"][0]);
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "DUPLICATE_IO_ENTRY");
  EXPECT_EQ(diagnostic.path, "/io/input/1");

  document = Document();
  document["io"]["output"].push_back(document["io"]["output"][0]);
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "DUPLICATE_IO_ENTRY");
  EXPECT_EQ(diagnostic.path, "/io/output/1");

  // 同一宿主结构体在一侧可以出现多次，只要业务名不同且端口不冲突。
  auto other = MakeInput("other_biz");
  other.service_type = 9002;
  other.logical_ports = {NodePortDefinition("other_port", "TextBatch", true)};
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterInputConverter(other));
  document = Document();
  document["io"]["input"].push_back(
      {{"type", "entity_in"}, {"name", "other_biz"}});
  EXPECT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
}

TEST_F(IoConverterRegistryTest, MultipleInputItemsMustPublishDistinctPorts) {
  auto& registry = IoConverterRegistry::Instance();
  InputConverterDefinition second;
  second.type = "frame";
  second.name = "pair";
  second.service_type = 9002;
  second.slot = ExternalInputSlot<CompanyFrame>("frame");
  second.logical_ports = {
      NodePortDefinition("input_sentences", "TextBatch", true)};
  second.decode_fn = &DummyDecode;
  ASSERT_TRUE(registry.RegisterInputConverter(second));

  auto document = Document();
  document["io"]["input"].push_back({{"type", "frame"}, {"name", "pair"}});
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "DUPLICATE_PORT_PRODUCER");
  EXPECT_EQ(diagnostic.path, "/io/input/1");

  // 端口不重名时两项的端口合并成同一个 IO 边界。
  registry.ClearForTesting();
  ASSERT_TRUE(registry.RegisterInputConverter(MakeInput("test_biz")));
  ASSERT_TRUE(registry.RegisterOutputConverter(MakeOutput("test_biz")));
  second.logical_ports = {NodePortDefinition("image_paths", "TextBatch", true)};
  ASSERT_TRUE(registry.RegisterInputConverter(second));
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  ASSERT_EQ(prepared.inputs.size(), 2U);
  ASSERT_EQ(prepared.io_boundary.input_published_ports.size(), 2U);
  EXPECT_EQ(prepared.io_boundary.input_published_ports[0].Name(),
            "input_sentences");
  EXPECT_EQ(prepared.io_boundary.input_published_ports[1].Name(),
            "image_paths");
}

TEST_F(IoConverterRegistryTest, AtLeastOneInputItemMustCarryRequestId) {
  auto& registry = IoConverterRegistry::Instance();
  registry.ClearForTesting();
  InputConverterDefinition text;
  text.type = "string";
  text.name = "question";
  text.slot = ExternalInputSlot<CompanyString>("string");
  text.logical_ports = {
      NodePortDefinition("input_sentences", "TextBatch", true)};
  text.decode_fn = &DummyDecode;
  ASSERT_TRUE(registry.RegisterInputConverter(text));
  ASSERT_TRUE(registry.RegisterOutputConverter(MakeOutput("test_biz")));

  auto document = Document({{"type", "string"}, {"name", "question"}});
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "INVALID_COMBINATION");
  EXPECT_EQ(diagnostic.path, "/io/input");
}

// ---------------------------------------------------------------------------
// converter 参数
// ---------------------------------------------------------------------------

class IoConverterParamsTest : public IoConverterRegistryTest {
 protected:
  void SetUp() override {
    IoConverterRegistryTest::SetUp();
    auto behavior = MakeInput("behavior");
    behavior.service_type = 9002;
    behavior.params = BehaviorSpec();
    behavior.decode_fn = &BehaviorDecode;
    ASSERT_TRUE(
        IoConverterRegistry::Instance().RegisterInputConverter(behavior));
    g_observed = {};
  }

  static nlohmann::json BehaviorDocument(const nlohmann::json& params) {
    auto document = Document();
    document["io"]["input"][0] = {
        {"type", "entity_in"}, {"name", "behavior"}, {"params", params}};
    return document;
  }
};

TEST_F(IoConverterParamsTest, DefaultsApplyWhenParamsAreOmitted) {
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  auto document = BehaviorDocument(nlohmann::json::object());
  document["io"]["input"][0].erase("params");
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  const auto& values = *prepared.inputs[0].params;
  EXPECT_EQ(values.Get<BehaviorParams>().mode, "a");
  EXPECT_EQ(values.Get<BehaviorParams>().limit, 3);
  EXPECT_EQ(values.Get<BehaviorParams>().doubled, 6);
  EXPECT_EQ(values.Effective(), (nlohmann::json{{"mode", "a"}, {"limit", 3}}));
}

TEST_F(IoConverterParamsTest,
       OverridesTakeEffectAndPrepareDerivedValuesStayInternal) {
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(
      PrepareDeploymentDocument(BehaviorDocument({{"mode", "b"}, {"limit", 4}}),
                                {}, &prepared, &diagnostic))
      << diagnostic.message;
  const auto& params = prepared.inputs[0].params->Get<BehaviorParams>();
  EXPECT_EQ(params.mode, "b");
  EXPECT_EQ(params.limit, 4);
  EXPECT_EQ(params.doubled, 8);
  // 生效值只含已声明的参数，Prepare 推导出的内部成员不进入配置。
  EXPECT_FALSE(prepared.inputs[0].params->Effective().contains("doubled"));
}

TEST_F(IoConverterParamsTest, ParameterErrorsKeepNodeStyleCodesAndPaths) {
  struct Case {
    nlohmann::json params;
    const char* code;
    const char* path;
  };
  const std::vector<Case> cases = {
      {{{"nope", 1}}, "UNKNOWN_CONFIG_FIELD", "/io/input/0/params/nope"},
      {{{"limit", "x"}}, "CONFIG_FIELD_TYPE", "/io/input/0/params/limit"},
      {{{"limit", 11}}, "CONFIG_FIELD_RANGE", "/io/input/0/params/limit"},
      {{{"mode", "c"}}, "CONFIG_FIELD_ENUM", "/io/input/0/params/mode"},
      // Prepare 失败：整项 params 出错。
      {{{"limit", 9}}, "INVALID_COMBINATION", "/io/input/0/params"},
      // Validate 的跨字段检查失败：整项 params 出错。
      {{{"mode", "b"}, {"limit", 8}},
       "INVALID_COMBINATION",
       "/io/input/0/params"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.params.dump());
    PreparedDeployment prepared;
    DeploymentDiagnostic diagnostic;
    EXPECT_FALSE(PrepareDeploymentDocument(BehaviorDocument(test.params), {},
                                           &prepared, &diagnostic));
    EXPECT_EQ(diagnostic.code, test.code) << diagnostic.message;
    EXPECT_EQ(diagnostic.path, test.path);
  }
}

TEST_F(IoConverterParamsTest,
       ParametersWrittenForAConverterWithoutOnesAreUnknown) {
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  auto document = Document({{"params", {{"anything", 1}}}});
  EXPECT_FALSE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "UNKNOWN_CONFIG_FIELD");
  EXPECT_EQ(diagnostic.path, "/io/input/0/params/anything");
}

TEST_F(IoConverterParamsTest,
       SharedParametersAreReadOnlyAcrossConcurrentCalls) {
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(BehaviorDocument({{"limit", 5}}), {},
                                        &prepared, &diagnostic));
  const auto* converter = prepared.inputs[0].converter;
  const ParameterValues* shared = prepared.inputs[0].params.get();
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 200; ++i) {
        const auto& p = shared->Get<BehaviorParams>();
        if (p.limit != 5 || p.doubled != 10 ||
            converter->params.Fields().size() != 2U) {
          ++mismatches;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(mismatches.load(), 0);
}

// ---------------------------------------------------------------------------
// 输出内存
// ---------------------------------------------------------------------------

TEST_F(IoConverterRegistryTest,
       OutputSizeComesFromConverterDefaultsAndOverrides) {
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(Document(), {}, &prepared, &diagnostic))
      << diagnostic.message;
  ASSERT_EQ(prepared.outputs.size(), 1U);
  const auto& spec = prepared.outputs[0].pool_spec;
  EXPECT_EQ(spec.type, "entity_out");
  EXPECT_EQ(spec.GetCapacity("entities_json"), 2047U);
  EXPECT_EQ(spec.meta_num, 0U);
  EXPECT_EQ(spec.metadata_type_id, 0);
  EXPECT_TRUE(spec.allocator.empty());

  ASSERT_TRUE(PrepareDeploymentDocument(
      Document(nlohmann::json::object(),
               {{"params", {{"entities_json_max_bytes", 17}}}}),
      {}, &prepared, &diagnostic))
      << diagnostic.message;
  EXPECT_EQ(prepared.outputs[0].pool_spec.GetCapacity("entities_json"), 17U);
}

TEST_F(IoConverterRegistryTest,
       OutputSizeAbovePlatformLimitOrBelowOneIsRejected) {
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  EXPECT_FALSE(PrepareDeploymentDocument(
      Document(nlohmann::json::object(),
               {{"params", {{"entities_json_max_bytes", 65537}}}}),
      {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "CONFIG_FIELD_RANGE");
  EXPECT_EQ(diagnostic.path, "/io/output/0/params/entities_json_max_bytes");

  EXPECT_FALSE(PrepareDeploymentDocument(
      Document(nlohmann::json::object(),
               {{"params", {{"entities_json_max_bytes", 0}}}}),
      {}, &prepared, &diagnostic));
  EXPECT_EQ(diagnostic.code, "CONFIG_FIELD_RANGE");
  EXPECT_EQ(diagnostic.path, "/io/output/0/params/entities_json_max_bytes");

  ASSERT_TRUE(PrepareDeploymentDocument(
      Document(nlohmann::json::object(),
               {{"params", {{"entities_json_max_bytes", 65536}}}}),
      {}, &prepared, &diagnostic))
      << diagnostic.message;
}

TEST_F(IoConverterRegistryTest, PrepareDerivedSizeTakesEffect) {
  auto derived = MakeOutput("derived");
  derived.service_type = 9002;
  derived.params = DerivedSizeSpec();
  ASSERT_TRUE(IoConverterRegistry::Instance().RegisterOutputConverter(derived));
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  auto document = Document();
  document["io"]["output"][0] = {
      {"type", "entity_out"}, {"name", "derived"}, {"params", {{"base", 250}}}};
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  EXPECT_EQ(prepared.outputs[0].pool_spec.GetCapacity("entities_json"), 1000U);
  // 生效值反映 Prepare 之后的尺寸。
  EXPECT_EQ(EffectiveIoJson(
                prepared)["output"][0]["params"]["entities_json_max_bytes"],
            1000);
}

TEST_F(IoConverterRegistryTest,
       EffectiveIoJsonWritesTypeNameAndEffectiveParams) {
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(
      Document(nlohmann::json::object(),
               {{"params", {{"entities_json_max_bytes", 17}}}}),
      {}, &prepared, &diagnostic));
  EXPECT_EQ(EffectiveIoJson(prepared),
            (nlohmann::json{
                {"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
                {"output",
                 {{{"type", "entity_out"},
                   {"name", "test_biz"},
                   {"params", {{"entities_json_max_bytes", 17}}}}}}}));
}

// 可选输出槽总是分配输出池。
TEST_F(IoConverterRegistryTest, OptionalOutputSlotStillGetsAPoolSpec) {
  auto optional = MakeOutput("optional_slot");
  optional.service_type = 9002;
  optional.slot.required = false;
  ASSERT_TRUE(
      IoConverterRegistry::Instance().RegisterOutputConverter(optional));
  auto document = Document();
  document["io"]["output"][0]["name"] = "optional_slot";
  PreparedDeployment prepared;
  DeploymentDiagnostic diagnostic;
  ASSERT_TRUE(PrepareDeploymentDocument(document, {}, &prepared, &diagnostic))
      << diagnostic.message;
  ASSERT_EQ(prepared.outputs.size(), 1U);
  EXPECT_FALSE(prepared.outputs[0].converter->slot.required);
  EXPECT_EQ(prepared.outputs[0].pool_spec.type, "entity_out");
}

// ---------------------------------------------------------------------------
// 定位配置、模型路径与解析入口
// ---------------------------------------------------------------------------

TEST_F(IoConverterRegistryTest, DeploymentIoConfigValidation) {
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

  // 3. 拒绝顶层未知字段
  nlohmann::json bad_field = valid_cfg;
  bad_field["extra_field"] = "foo";
  EXPECT_FALSE(DeploymentIoConfig::Parse(bad_field, tmp_dir, &parsed, &err));
  EXPECT_NE(err.find("Unknown field"), std::string::npos);

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

  // 8. ReadFromFile 针对未知字段报错同样携带配置文件路径上下文
  const std::string dep_conf_path = tmp_dir + "/unknown_field.conf";
  {
    std::ofstream ofs(dep_conf_path);
    ofs << bad_field.dump();
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

TEST_F(IoConverterRegistryTest,
       StrictConfigDirectoryIsolationAndCwdInvariance) {
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

TEST_F(IoConverterRegistryTest, ModelPathResolutionFailurePointsToModelEntry) {
  nlohmann::json doc = {
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
        {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
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
  EXPECT_EQ(IoPlanResolver::ResolveFromPipelineJson(doc, "./models", &plan,
                                                    &err, &diagnostic),
            -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diagnostic.code, "INVALID_MODEL_PATH");
  EXPECT_EQ(diagnostic.path, "/models/0/model_path");
  EXPECT_NE(err.find(diagnostic.path), std::string::npos) << err;
}

TEST_F(IoConverterRegistryTest,
       PrepareDeploymentSuccessAndBoundaryExtraction_T01) {
  nlohmann::json doc = {
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
        {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
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
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  ASSERT_TRUE(PrepareDeploymentDocument(doc, options, &prepared, &diag))
      << diag.message;
  EXPECT_FALSE(prepared.neutral_pipeline_json.contains("io"));
  EXPECT_EQ(prepared.neutral_pipeline_json["models"], doc["models"]);
  ASSERT_EQ(prepared.inputs.size(), 1u);
  EXPECT_EQ(prepared.inputs[0].converter->Label(), "entity_in/test_biz");
  ASSERT_EQ(prepared.outputs.size(), 1u);
  EXPECT_EQ(prepared.outputs[0].converter->Label(), "entity_out/test_biz");
  EXPECT_EQ(prepared.outputs[0].pool_spec.type, "entity_out");
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

TEST_F(IoConverterRegistryTest, ModelPathMissingEmptyOrWrongTypeRejected_T03) {
  const nlohmann::json base_doc = {
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
        {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
      {"models",
       {{{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()}}}},
      {"pipeline", DefaultPipelineNodes()}};
  DeploymentPrepareOptions options;
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

TEST_F(IoConverterRegistryTest, ModelStructureInvalidRejected_T04) {
  DeploymentPrepareOptions options;
  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  // 情形 1：models 不是数组 (如对象)
  {
    nlohmann::json doc = {
        {"io",
         {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
          {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
        {"models", {{"mid_1", "not_an_array"}}},
        {"pipeline", DefaultPipelineNodes()}};
    EXPECT_FALSE(PrepareDeploymentDocument(doc, options, &prepared, &diag));
    EXPECT_EQ(diag.code, "FIELD_TYPE");
    EXPECT_EQ(diag.path, "/models");
  }

  // 情形 2：每个模型条目都必须有 model_path。
  {
    nlohmann::json doc = {
        {"io",
         {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
          {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
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
        {"io",
         {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
          {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
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

TEST_F(IoConverterRegistryTest, ModelPathNonexistentOnDiskIsAllowed_T05) {
  const auto temp_dir = fs::temp_directory_path() / "edgeflow_test_t05";
  fs::create_directories(temp_dir);
  const auto missing_path = temp_dir / "missing_dir/model.bin";
  ASSERT_FALSE(fs::exists(missing_path));
  const nlohmann::json doc = {
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
        {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
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

TEST_F(IoConverterRegistryTest,
       PublicIoContractQueryWorksWithoutInitAndClearsFailures) {
  const auto directory =
      fs::temp_directory_path() / "edgeflow_io_contract_query";
  fs::create_directories(directory);
  const std::string root = directory.string();
  nlohmann::json document = {
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
        {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
      {"pipeline",
       {{{"id", "copy"},
         {"node_type", "TextTemplateNode"},
         {"inputs", {{"primary", "input_sentences"}}},
         {"outputs", {{"text", "llm_answers"}}},
         {"config", {{"template", "{{primary}}"}}}}}}};
  std::ofstream(directory / "pipeline.json") << document;
  std::ofstream(directory / "pipeline.conf")
      << nlohmann::json{{"pipe_path", "pipeline.json"}};
  operator_api::OperatorIoContract contract;
  char error[512]{};
  const int decoded_before = dummy_decode_calls;
  const int encoded_before = dummy_encode_calls;
  ASSERT_EQ(operator_api::ResolveOperatorConfigIo(
                root.c_str(), "pipeline.conf", &contract, error, sizeof(error)),
            0)
      << error;
  ASSERT_EQ(contract.inputs.size(), 1U);
  EXPECT_EQ(contract.inputs[0].type, "entity_in");
  EXPECT_EQ(contract.inputs[0].name, "test_biz");
  EXPECT_EQ(contract.inputs[0].type_name, "CompanyOperatorEntityInput");
  EXPECT_EQ(contract.inputs[0].service_type, kTestServiceType);
  EXPECT_TRUE(contract.inputs[0].required);
  ASSERT_EQ(contract.outputs.size(), 1U);
  EXPECT_EQ(contract.outputs[0].type, "entity_out");
  EXPECT_EQ(contract.outputs[0].name, "test_biz");
  EXPECT_EQ(contract.outputs[0].type_name, "CompanyOperatorEntityOutput");
  EXPECT_EQ(contract.outputs[0].service_type, kTestServiceType);
  EXPECT_EQ(dummy_decode_calls, decoded_before);
  EXPECT_EQ(dummy_encode_calls, encoded_before);
  document["io"]["input"][0]["name"] = "unknown_biz";
  std::ofstream(directory / "pipeline.json") << document;
  EXPECT_EQ(operator_api::ResolveOperatorConfigIo(
                root.c_str(), "pipeline.conf", &contract, error, sizeof(error)),
            -2);
  EXPECT_TRUE(contract.inputs.empty());
  EXPECT_TRUE(contract.outputs.empty());
  EXPECT_NE(std::string(error).find("unknown_biz"), std::string::npos);
  EXPECT_EQ(operator_api::ResolveOperatorConfigIo(
                root.c_str(), "pipeline.conf", nullptr, error, sizeof(error)),
            -2);
  fs::remove_all(directory);
}

TEST_F(IoConverterRegistryTest,
       PrepareFailureResetsPreparedStateAtomically_T18) {
  nlohmann::json valid_doc = {
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
        {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
      {"models",
       {{{"model_id", "mid_1"},
         {"model_type", "test_biz_embedding"},
         {"backend", "test_tensor_backend"},
         {"model_config", {{"embedding_dim", 128}, {"max_batch_size", 4}}},
         {"backend_config", nlohmann::json::object()},
         {"model_path", "models/original.bin"}}}},
      {"pipeline", DefaultPipelineNodes()}};

  DeploymentPrepareOptions options;

  PreparedDeployment prepared;
  DeploymentDiagnostic diag;

  // 1. 首次准备成功
  ASSERT_TRUE(PrepareDeploymentDocument(valid_doc, options, &prepared, &diag));
  EXPECT_FALSE(prepared.inputs.empty());
  EXPECT_FALSE(prepared.outputs.empty());
  EXPECT_FALSE(prepared.neutral_pipeline_json.is_null());

  // 2. 在失败的文档上复用同一个 prepared 实例
  nlohmann::json invalid_doc = valid_doc;
  invalid_doc["io"]["output"][0]["name"] = "unknown_biz";
  const nlohmann::json invalid_doc_copy = invalid_doc;

  EXPECT_FALSE(
      PrepareDeploymentDocument(invalid_doc, options, &prepared, &diag));

  // 验证 prepared 的所有字段都已完全重置
  EXPECT_TRUE(prepared.inputs.empty());
  EXPECT_TRUE(prepared.outputs.empty());
  EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
  EXPECT_TRUE(prepared.io_boundary.input_published_ports.empty());
  EXPECT_TRUE(prepared.io_boundary.output_consumed_ports.empty());

  // 验证输入文档未被修改
  EXPECT_EQ(invalid_doc, invalid_doc_copy);

  // 解析完第一个模型后失败时，不得发布部分状态。
  valid_doc["models"].push_back(valid_doc["models"][0]);
  valid_doc["models"][1]["model_id"] = "mid_2";
  valid_doc["models"][1]["model_path"] = "models/second.bin";
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
  EXPECT_TRUE(prepared.inputs.empty());
  EXPECT_TRUE(prepared.outputs.empty());
  EXPECT_TRUE(prepared.neutral_pipeline_json.is_null());
  EXPECT_TRUE(prepared.io_boundary.input_published_ports.empty());
  EXPECT_TRUE(prepared.io_boundary.output_consumed_ports.empty());
  EXPECT_EQ(invalid_doc, invalid_second_copy);
}

TEST_F(IoConverterRegistryTest,
       ResolveFromPipelineJsonDiagnosticCarrier_T03_T06_T07_T08) {
  // T03：经 IoPlanResolver 缺少直接模型路径
  nlohmann::json t03_doc = {
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
        {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
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
  int rc = IoPlanResolver::ResolveFromPipelineJson(t03_doc, "./models", &plan,
                                                   &err, &diag);
  EXPECT_EQ(rc, -3);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "MISSING_FIELD");
  EXPECT_EQ(diag.path, "/models/0/model_path");

  // T06：io 中的未知键
  nlohmann::json t06_doc = t03_doc;
  t06_doc["models"][0]["model_path"] = "models/original.bin";
  t06_doc["io"]["unknown_field"] = 1;
  rc = IoPlanResolver::ResolveFromPipelineJson(t06_doc, "./models", &plan, &err,
                                               &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
  EXPECT_EQ(diag.path, "/io/unknown_field");

  // T07：经 IoPlanResolver 的未知 converter
  nlohmann::json t07_doc = t03_doc;
  t07_doc["models"][0]["model_path"] = "models/original.bin";
  t07_doc["io"]["input"][0]["name"] = "nonexistent_biz";
  rc = IoPlanResolver::ResolveFromPipelineJson(t07_doc, "./models", &plan, &err,
                                               &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "UNKNOWN_CONVERTER");
  EXPECT_EQ(diag.path, "/io/input/0");

  // T08：非法的 converter 参数保留其精确诊断
  nlohmann::json t08_doc = t03_doc;
  t08_doc["models"][0]["model_path"] = "models/original.bin";
  t08_doc["io"]["output"][0]["params"] = {{"entities_json_max_bytes", 0}};
  rc = IoPlanResolver::ResolveFromPipelineJson(t08_doc, "./models", &plan, &err,
                                               &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "CONFIG_FIELD_RANGE");
  EXPECT_EQ(diag.path, "/io/output/0/params/entities_json_max_bytes");
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

TEST_F(IoConverterRegistryTest,
       ResolveFromFileDiagnosticCarrier_T03_T06_T07_T08_AndFileErrors) {
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
      {"io",
       {{"input", {{{"type", "entity_in"}, {"name", "test_biz"}}}},
        {"output", {{{"type", "entity_out"}, {"name", "test_biz"}}}}}},
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
  int rc = IoPlanResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                           &diag);
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

    rc = IoPlanResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                         &diag);
    EXPECT_EQ(rc, -3);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "MISSING_FIELD");
    EXPECT_EQ(diag.path, "/models/0/model_path");
  }

  // 3. 经文件的 T06：io 中的未知键
  {
    nlohmann::json t06_pipe = base_pipeline;
    t06_pipe["io"]["unknown_field"] = 1;
    write_file(pipe_path, t06_pipe);

    rc = IoPlanResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                         &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "DEPLOYMENT_ERROR");
    EXPECT_EQ(diag.path, "/io/unknown_field");
  }

  // 4. 经文件的 T07：未知 converter
  {
    nlohmann::json t07_pipe = base_pipeline;
    t07_pipe["io"]["output"][0]["name"] = "unregistered_biz";
    write_file(pipe_path, t07_pipe);

    rc = IoPlanResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                         &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "UNKNOWN_CONVERTER");
    EXPECT_EQ(diag.path, "/io/output/0");
  }

  // 5. 经文件的 T08：非法的 converter 参数
  {
    nlohmann::json t08_pipe = base_pipeline;
    t08_pipe["io"]["input"][0]["params"] = {{"unknown_param", 1}};
    write_file(pipe_path, t08_pipe);

    rc = IoPlanResolver::ResolveFromFile(conf_path.string(), "", &plan, &err,
                                         &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "UNKNOWN_CONFIG_FIELD");
    EXPECT_EQ(diag.path, "/io/input/0/params/unknown_param");
  }

  // 恢复合法的 Pipeline 文件
  write_file(pipe_path, base_pipeline);

  // 6. conf 文件不存在 -> CONFIG_FILE_OPEN
  rc = IoPlanResolver::ResolveFromFile((temp_dir / "nonexistent.conf").string(),
                                       "", &plan, &err, &diag);
  EXPECT_EQ(rc, -2);
  EXPECT_EQ(plan, nullptr);
  EXPECT_EQ(diag.code, "CONFIG_FILE_OPEN");
  EXPECT_EQ(diag.path, "/");

  // 7. conf 文件 JSON 格式错误 -> JSON_PARSE
  {
    fs::path bad_conf = temp_dir / "bad_syntax.conf";
    write_raw(bad_conf, "{ unquoted: invalid JSON ...");
    rc = IoPlanResolver::ResolveFromFile(bad_conf.string(), "", &plan, &err,
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
    rc = IoPlanResolver::ResolveFromFile(missing_pipe_conf.string(), "", &plan,
                                         &err, &diag);
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

    rc = IoPlanResolver::ResolveFromFile(bad_pipe_conf.string(), "", &plan,
                                         &err, &diag);
    EXPECT_EQ(rc, -2);
    EXPECT_EQ(plan, nullptr);
    EXPECT_EQ(diag.code, "JSON_PARSE");
    EXPECT_EQ(diag.path, "/");
  }

  fs::remove_all(temp_dir);
}

}  // namespace llm_edgeflow
