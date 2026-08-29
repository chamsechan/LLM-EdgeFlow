#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_config.h"
#include "core/pipeline_diagnostic.h"
#include "core/session_context.h"
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"

namespace alg_framework {

// =============================================================================
// 测试探针类与测试替身
// =============================================================================

static NodeDefinition MakeTestNodeDef(const std::string& type) {
  NodeDefinition def;
  def.node_type = type;
  def.category = "test";
  def.description = "test node " + type;
  def.parallel_safe = true;
  return def;
}

static EngineDefinition MakeTestEngineDef(const std::string& type) {
  EngineDefinition def;
  def.engine_type = type;
  def.capability = "test";
  def.description = "test engine " + type;
  def.thread_model = EngineThreadModel::kConcurrent;
  return def;
}

// 1. 基础计数探针
class CountingEngine : public IModelEngine {
 public:
  inline static constexpr char kEngineType[] = "counting_engine";
  static inline std::atomic<int> create_count{0};
  static inline std::atomic<int> load_count{0};

  static void Reset() {
    create_count.store(0);
    load_count.store(0);
  }

  CountingEngine() { create_count.fetch_add(1); }

  bool Load(const std::string& model_path,
            const nlohmann::json& custom_config) override {
    (void)model_path;
    (void)custom_config;
    load_count.fetch_add(1);
    return true;
  }

  size_t GetMaxBatchSize() const override { return 4; }

  const std::string& EngineType() const override {
    static const std::string type = kEngineType;
    return type;
  }
};
REGISTER_ENGINE_WITH_DEFINITION(CountingEngine,
                                MakeTestEngineDef(CountingEngine::kEngineType));

class CountingNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "CountingNode";
  static inline std::atomic<int> create_count{0};
  static inline std::atomic<int> init_count{0};

  static void Reset() {
    create_count.store(0);
    init_count.store(0);
  }

  CountingNode() { create_count.fetch_add(1); }

  bool Init(const nlohmann::json& config,
            SessionContext* session_ctx) override {
    (void)config;
    (void)session_ctx;
    init_count.fetch_add(1);
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    (void)req_ctx;
    return 0;
  }

  NodeControlResult Control(int cmd, const std::string& param) override {
    (void)cmd;
    (void)param;
    return NodeControlResult::Handled(0);
  }

  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};
REGISTER_NODE_WITH_DEFINITION(CountingNode,
                              MakeTestNodeDef(CountingNode::kNodeType));

// 2. 异常与失败测试替身 (R1-ACC-001)
class ThrowingCtorEngine : public IModelEngine {
 public:
  inline static constexpr char kEngineType[] = "throwing_ctor_engine";

  ThrowingCtorEngine() {
    throw std::runtime_error("ThrowingCtorEngine constructor exception");
  }
  bool Load(const std::string&, const nlohmann::json&) override { return true; }
  size_t GetMaxBatchSize() const override { return 1; }
  const std::string& EngineType() const override {
    static const std::string type = kEngineType;
    return type;
  }
};
REGISTER_ENGINE_WITH_DEFINITION(
    ThrowingCtorEngine, MakeTestEngineDef(ThrowingCtorEngine::kEngineType));

class ThrowingLoadEngine : public IModelEngine {
 public:
  inline static constexpr char kEngineType[] = "throwing_load_engine";

  ThrowingLoadEngine() = default;
  bool Load(const std::string&, const nlohmann::json&) override {
    throw std::runtime_error("ThrowingLoadEngine Load exception");
  }
  size_t GetMaxBatchSize() const override { return 1; }
  const std::string& EngineType() const override {
    static const std::string type = kEngineType;
    return type;
  }
};
REGISTER_ENGINE_WITH_DEFINITION(
    ThrowingLoadEngine, MakeTestEngineDef(ThrowingLoadEngine::kEngineType));

class FailingLoadEngine : public IModelEngine {
 public:
  inline static constexpr char kEngineType[] = "failing_load_engine";

  FailingLoadEngine() = default;
  bool Load(const std::string&, const nlohmann::json&) override {
    return false;
  }
  size_t GetMaxBatchSize() const override { return 1; }
  const std::string& EngineType() const override {
    static const std::string type = kEngineType;
    return type;
  }
};
REGISTER_ENGINE_WITH_DEFINITION(
    FailingLoadEngine, MakeTestEngineDef(FailingLoadEngine::kEngineType));

class ThrowingCtorNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "ThrowingCtorNode";
  ThrowingCtorNode() {
    throw std::runtime_error("ThrowingCtorNode constructor exception");
  }
  bool Init(const nlohmann::json&, SessionContext*) override { return true; }
  int Process(AlgContext*) override { return 0; }
  NodeControlResult Control(int, const std::string&) override {
    return NodeControlResult::Handled(0);
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};
REGISTER_NODE_WITH_DEFINITION(ThrowingCtorNode,
                              MakeTestNodeDef(ThrowingCtorNode::kNodeType));

class ThrowingInitNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "ThrowingInitNode";
  ThrowingInitNode() = default;
  bool Init(const nlohmann::json&, SessionContext*) override {
    throw std::runtime_error("ThrowingInitNode Init exception");
  }
  int Process(AlgContext*) override { return 0; }
  NodeControlResult Control(int, const std::string&) override {
    return NodeControlResult::Handled(0);
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};
REGISTER_NODE_WITH_DEFINITION(ThrowingInitNode,
                              MakeTestNodeDef(ThrowingInitNode::kNodeType));

class FailingInitNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "FailingInitNode";
  FailingInitNode() = default;
  bool Init(const nlohmann::json&, SessionContext*) override { return false; }
  int Process(AlgContext*) override { return 0; }
  NodeControlResult Control(int, const std::string&) override {
    return NodeControlResult::Handled(0);
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};
REGISTER_NODE_WITH_DEFINITION(FailingInitNode,
                              MakeTestNodeDef(FailingInitNode::kNodeType));

// 辅助函数：查找配置文件相对路径
static std::string GetConfigPath(const std::string& rel_path) {
  FILE* fp = fopen(rel_path.c_str(), "r");
  if (fp) {
    fclose(fp);
    return rel_path;
  }
  return "../" + rel_path;
}

// =============================================================================
// GTest 测试套件
// =============================================================================
class PipelineConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // RECHECK-R1-002: 启动时严格断言全局静态注册无冲突，不依赖生产 Reset 接口
    ASSERT_FALSE(NodeFactory::Instance().HasConflict());
    ASSERT_FALSE(EngineFactory::Instance().HasConflict());
    CountingEngine::Reset();
    CountingNode::Reset();
  }

  void SetInternalBuildHook(Pipeline* p, std::function<void()> hook) {
    if (p) {
      p->test_internal_hook_ = std::move(hook);
    }
  }
};

// 1. 兼容正例：当前 9 份正式配置全部 Parse/Build 通过
TEST_F(PipelineConfigTest, PositiveNineFormalConfigs) {
  const std::vector<std::string> configs = {
      "configs/pipeline_keyword_match.json",
      "configs/pipeline_entity_extract.json",
      "configs/pipeline_doc_qa.json",
      "configs/pipeline_dialogue_audit.json",
      "configs/pipeline_doc_qa_onnx.json",
      "configs/pipeline_entity_extract_llamacpp.json",
      "configs/pipeline_ocr_doc_qa.json",
      "configs/pipeline_audio_asr_intent.json",
      "configs/pipeline_cross_rerank.json",
  };

  for (const auto& cfg_file : configs) {
    std::string full_path = GetConfigPath(cfg_file);
    std::ifstream ifs(full_path);
    ASSERT_TRUE(ifs.is_open()) << "Failed to open config file: " << full_path;

    nlohmann::json root;
    ifs >> root;

    ParsedPipelineConfig parsed_cfg;
    PipelineDiagnostic diag;
    bool parse_ok = ParsePipelineConfig(root, &parsed_cfg, &diag);
    EXPECT_TRUE(parse_ok) << "Parse failed for " << cfg_file << ": "
                          << diag.message << " at " << diag.path;
    EXPECT_EQ(diag.code, PipelineErrorCode::kOk);

    Pipeline pipeline;
    bool build_ok = pipeline.BuildFromConfigFile(full_path, &diag);
    EXPECT_TRUE(build_ok) << "Build failed for " << cfg_file << ": "
                          << diag.message << " at " << diag.path;
    EXPECT_EQ(diag.code, PipelineErrorCode::kOk);
    EXPECT_TRUE(pipeline.IsReady());
    EXPECT_EQ(pipeline.GetState(), Pipeline::State::kReady);
  }
}

// 2. 严格拦截：缺少 id 或 depends_on 时直接报错拦截 (拒绝隐式旧格式)
TEST_F(PipelineConfigTest, RejectsPipelineWithoutIdOrDependsOn) {
  nlohmann::json root = {
      {"biz_name", "seq_compat_test"},
      {"pipeline",
       nlohmann::json::array(
           {{{"node_type", "CountingNode"}, {"config", {{"k", 1}}}},
            {{"node_type", "CountingNode"}, {"config", {{"k", 2}}}}})}};

  ParsedPipelineConfig parsed_cfg;
  PipelineDiagnostic diag;
  EXPECT_FALSE(ParsePipelineConfig(root, &parsed_cfg, &diag));
  EXPECT_EQ(diag.code, PipelineErrorCode::kMissingField);
  EXPECT_EQ(diag.path, "/pipeline/0/id");
}

TEST_F(PipelineConfigTest, AcceptsLegacyBusinessNameField) {
  nlohmann::json root = {
      {"business_name", "legacy_compat_test"},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", "CountingNode"},
                               {"depends_on", nlohmann::json::array()}}})}};

  ParsedPipelineConfig parsed_cfg;
  PipelineDiagnostic diag;
  EXPECT_TRUE(ParsePipelineConfig(root, &parsed_cfg, &diag));
  EXPECT_EQ(parsed_cfg.biz_name, "legacy_compat_test");
  EXPECT_EQ(parsed_cfg.business_name, "legacy_compat_test");
}

// 3. 表驱动负例测试：结构、类型、字段、组合、DAG 负例与零副作用断言
// (R1-ACC-003, R1-ACC-006)
struct NegativeTestCase {
  std::string name;
  nlohmann::json input;
  PipelineErrorCode expected_code;
  std::string expected_path_prefix;
};

TEST_F(PipelineConfigTest, TableDrivenNegativeValidationAndZeroSideEffects) {
  std::vector<NegativeTestCase> cases;
  const auto valid_pipe =
      nlohmann::json::array({{{"id", "node_0"},
                              {"node_type", "CountingNode"},
                              {"depends_on", nlohmann::json::array()}}});

  // --- Root 校验 ---
  cases.push_back(NegativeTestCase{"RootNotObject",
                                   nlohmann::json::array({1, 2, 3}),
                                   PipelineErrorCode::kRootType, "/"});
  cases.push_back(NegativeTestCase{"RootUnknownField",
                                   nlohmann::json{{"biz_name", "test"},
                                                  {"unknown_root_key", 123},
                                                  {"pipeline", valid_pipe}},
                                   PipelineErrorCode::kUnknownField,
                                   "/unknown_root_key"});
  cases.push_back(NegativeTestCase{
      "RootCommentNotString",
      nlohmann::json{
          {"biz_name", "test"}, {"comment", 12345}, {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/comment"});
  cases.push_back(NegativeTestCase{
      "MissingBizName", nlohmann::json{{"pipeline", valid_pipe}},
      PipelineErrorCode::kMissingField, "/biz_name"});
  cases.push_back(NegativeTestCase{
      "EmptyBizName",
      nlohmann::json{{"biz_name", ""}, {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldRange, "/biz_name"});
  cases.push_back(NegativeTestCase{
      "NonStringBizName",
      nlohmann::json{{"biz_name", 12345}, {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/biz_name"});

  // --- Execution Mode & Workers 组合校验 (R1-ACC-003) ---
  cases.push_back(NegativeTestCase{
      "SequentialModeWithMaxParallelWorkers",
      nlohmann::json{{"biz_name", "test"},
                     {"execution_mode", "sequential"},
                     {"max_parallel_workers", 4},
                     {"pipeline", valid_pipe}},
      PipelineErrorCode::kInvalidCombination, "/max_parallel_workers"});
  cases.push_back(NegativeTestCase{"ExecutionModeAsyncRejected",
                                   nlohmann::json{{"biz_name", "test"},
                                                  {"execution_mode", "async"},
                                                  {"pipeline", valid_pipe}},
                                   PipelineErrorCode::kFieldRange,
                                   "/execution_mode"});
  cases.push_back(
      NegativeTestCase{"ExecutionModeUnknownString",
                       nlohmann::json{{"biz_name", "test"},
                                      {"execution_mode", "coroutine_mode"},
                                      {"pipeline", valid_pipe}},
                       PipelineErrorCode::kFieldRange, "/execution_mode"});
  cases.push_back(NegativeTestCase{"ExecutionModeNonString",
                                   nlohmann::json{{"biz_name", "test"},
                                                  {"execution_mode", true},
                                                  {"pipeline", valid_pipe}},
                                   PipelineErrorCode::kFieldType,
                                   "/execution_mode"});
  cases.push_back(NegativeTestCase{
      "WorkersZeroInParallel",
      nlohmann::json{
          {"biz_name", "test"},
          {"execution_mode", "parallel"},
          {"max_parallel_workers", 0},
          {"pipeline",
           nlohmann::json::array({{{"id", "n1"},
                                   {"node_type", "CountingNode"},
                                   {"depends_on", nlohmann::json::array()}}})}},
      PipelineErrorCode::kFieldRange, "/max_parallel_workers"});
  cases.push_back(NegativeTestCase{
      "WorkersOutOfRange65InParallel",
      nlohmann::json{
          {"biz_name", "test"},
          {"execution_mode", "parallel"},
          {"max_parallel_workers", 65},
          {"pipeline",
           nlohmann::json::array({{{"id", "n1"},
                                   {"node_type", "CountingNode"},
                                   {"depends_on", nlohmann::json::array()}}})}},
      PipelineErrorCode::kFieldRange, "/max_parallel_workers"});

  // --- Models 校验 ---
  cases.push_back(NegativeTestCase{"ModelsNotArray",
                                   nlohmann::json{{"biz_name", "test"},
                                                  {"models", "not_an_array"},
                                                  {"pipeline", valid_pipe}},
                                   PipelineErrorCode::kFieldType, "/models"});
  cases.push_back(NegativeTestCase{
      "ModelItemNotObject",
      nlohmann::json{{"biz_name", "test"},
                     {"models", nlohmann::json::array({"invalid_string"})},
                     {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/models/0"});
  cases.push_back(NegativeTestCase{
      "ModelCommentNotString",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"engine_type", "counting_engine"},
                                             {"comment", 123}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/models/0/comment"});
  cases.push_back(NegativeTestCase{
      "ModelUnknownField",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"engine_type", "counting_engine"},
                                             {"unknown_model_key", 1}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kUnknownField, "/models/0/unknown_model_key"});
  cases.push_back(NegativeTestCase{
      "ModelMissingId",
      nlohmann::json{{"biz_name", "test"},
                     {"models", nlohmann::json::array(
                                    {{{"engine_type", "counting_engine"}}})},
                     {"pipeline", valid_pipe}},
      PipelineErrorCode::kMissingField, "/models/0/model_id"});
  cases.push_back(NegativeTestCase{
      "ModelEmptyId",
      nlohmann::json{{"biz_name", "test"},
                     {"models", nlohmann::json::array(
                                    {{{"model_id", ""},
                                      {"engine_type", "counting_engine"}}})},
                     {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldRange, "/models/0/model_id"});
  cases.push_back(NegativeTestCase{
      "ModelDuplicateId",
      nlohmann::json{
          {"biz_name", "test"},
          {"models",
           nlohmann::json::array(
               {{{"model_id", "dup_m"}, {"engine_type", "counting_engine"}},
                {{"model_id", "dup_m"}, {"engine_type", "counting_engine"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kDuplicateModelId, "/models/1/model_id"});
  cases.push_back(NegativeTestCase{
      "ModelMissingEngineType",
      nlohmann::json{{"biz_name", "test"},
                     {"models", nlohmann::json::array({{{"model_id", "m1"}}})},
                     {"pipeline", valid_pipe}},
      PipelineErrorCode::kMissingField, "/models/0/engine_type"});
  cases.push_back(NegativeTestCase{
      "ModelConfigNotObject",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"engine_type", "counting_engine"},
                                             {"config", "invalid"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/models/0/config"});
  cases.push_back(NegativeTestCase{
      "ModelUnknownEngineType",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array(
                         {{{"model_id", "m1"},
                           {"engine_type", "unregistered_mock_engine_xyz"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kUnknownEngineType, "/models/0/engine_type"});

  // --- Model/Backend 方言及混用校验 (RFC 0015) ---
  cases.push_back(NegativeTestCase{
      "ModelBackendMissingCapability",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"model_type", "bge_embedding"},
                                             {"backend", "onnxruntime"},
                                             {"model_path", "./model.onnx"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kMissingField, "/models/0/capability"});
  cases.push_back(NegativeTestCase{
      "ModelBackendEmptyCapability",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", ""},
                                             {"model_type", "bge_embedding"},
                                             {"backend", "onnxruntime"},
                                             {"model_path", "./model.onnx"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldRange, "/models/0/capability"});
  cases.push_back(NegativeTestCase{
      "ModelBackendWrongTypeCapability",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", 123},
                                             {"model_type", "bge_embedding"},
                                             {"backend", "onnxruntime"},
                                             {"model_path", "./model.onnx"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/models/0/capability"});
  cases.push_back(NegativeTestCase{
      "ModelBackendMissingModelType",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"backend", "onnxruntime"},
                                             {"model_path", "./model.onnx"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kMissingField, "/models/0/model_type"});
  cases.push_back(NegativeTestCase{
      "ModelBackendEmptyModelType",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", ""},
                                             {"backend", "onnxruntime"},
                                             {"model_path", "./model.onnx"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldRange, "/models/0/model_type"});
  cases.push_back(NegativeTestCase{
      "ModelBackendWrongTypeModelType",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", true},
                                             {"backend", "onnxruntime"},
                                             {"model_path", "./model.onnx"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/models/0/model_type"});
  cases.push_back(NegativeTestCase{
      "ModelBackendMissingBackend",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", "bge_embedding"},
                                             {"model_path", "./model.onnx"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kMissingField, "/models/0/backend"});
  cases.push_back(NegativeTestCase{
      "ModelBackendEmptyBackend",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", "bge_embedding"},
                                             {"backend", ""},
                                             {"model_path", "./model.onnx"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldRange, "/models/0/backend"});
  cases.push_back(NegativeTestCase{
      "ModelBackendWrongTypeBackend",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", "bge_embedding"},
                                             {"backend", 456},
                                             {"model_path", "./model.onnx"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/models/0/backend"});
  cases.push_back(NegativeTestCase{
      "ModelBackendMissingModelPath",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", "bge_embedding"},
                                             {"backend", "onnxruntime"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kMissingField, "/models/0/model_path"});
  cases.push_back(NegativeTestCase{
      "ModelBackendEmptyModelPath",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", "bge_embedding"},
                                             {"backend", "onnxruntime"},
                                             {"model_path", ""}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldRange, "/models/0/model_path"});
  cases.push_back(NegativeTestCase{
      "ModelBackendWrongTypeModelPath",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", "bge_embedding"},
                                             {"backend", "onnxruntime"},
                                             {"model_path", 789}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/models/0/model_path"});
  cases.push_back(NegativeTestCase{
      "ModelBackendModelConfigNotObject",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", "bge_embedding"},
                                             {"backend", "onnxruntime"},
                                             {"model_path", "./model.onnx"},
                                             {"model_config", "not_object"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/models/0/model_config"});
  cases.push_back(NegativeTestCase{
      "ModelBackendBackendConfigNotObject",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", "bge_embedding"},
                                             {"backend", "onnxruntime"},
                                             {"model_path", "./model.onnx"},
                                             {"backend_config", 123}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kFieldType, "/models/0/backend_config"});
  cases.push_back(NegativeTestCase{
      "ModelBackendUnknownField",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"capability", "embedding"},
                                             {"model_type", "bge_embedding"},
                                             {"backend", "onnxruntime"},
                                             {"model_path", "./model.onnx"},
                                             {"unsupported_opt", true}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kUnknownField, "/models/0/unsupported_opt"});
  cases.push_back(NegativeTestCase{
      "MixedDialectEngineTypeAndCapability",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"engine_type", "counting_engine"},
                                             {"capability", "embedding"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kInvalidCombination, "/models/0/capability"});
  cases.push_back(NegativeTestCase{
      "MixedDialectEngineTypeAndBackend",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array({{{"model_id", "m1"},
                                             {"engine_type", "counting_engine"},
                                             {"backend", "onnxruntime"}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kInvalidCombination, "/models/0/backend"});
  cases.push_back(NegativeTestCase{
      "MixedDialectConfigAndModelConfig",
      nlohmann::json{
          {"biz_name", "test"},
          {"models", nlohmann::json::array(
                         {{{"model_id", "m1"},
                           {"capability", "embedding"},
                           {"model_type", "bge_embedding"},
                           {"backend", "onnxruntime"},
                           {"model_path", "./model.onnx"},
                           {"config", nlohmann::json::object()},
                           {"model_config", nlohmann::json::object()}}})},
          {"pipeline", valid_pipe}},
      PipelineErrorCode::kInvalidCombination, "/models/0/capability"});

  // --- Pipeline Nodes 校验 ---
  cases.push_back(
      NegativeTestCase{"MissingPipeline", nlohmann::json{{"biz_name", "test"}},
                       PipelineErrorCode::kMissingField, "/pipeline"});
  cases.push_back(NegativeTestCase{
      "PipelineNotArray",
      nlohmann::json{{"biz_name", "test"}, {"pipeline", "not_an_array"}},
      PipelineErrorCode::kFieldType, "/pipeline"});
  cases.push_back(
      NegativeTestCase{"PipelineEmptyArray",
                       nlohmann::json{{"biz_name", "test"},
                                      {"pipeline", nlohmann::json::array()}},
                       PipelineErrorCode::kFieldRange, "/pipeline"});
  cases.push_back(NegativeTestCase{
      "NodeNotObject",
      nlohmann::json{{"biz_name", "test"},
                     {"pipeline", nlohmann::json::array({"string_node"})}},
      PipelineErrorCode::kFieldType, "/pipeline/0"});
  cases.push_back(NegativeTestCase{
      "NodeCommentNotString",
      nlohmann::json{{"biz_name", "test"},
                     {"pipeline", nlohmann::json::array(
                                      {{{"id", "n0"},
                                        {"node_type", "CountingNode"},
                                        {"depends_on", nlohmann::json::array()},
                                        {"comment", 999}}})}},
      PipelineErrorCode::kFieldType, "/pipeline/0/comment"});
  cases.push_back(NegativeTestCase{
      "NodeUnknownTopLevelField",
      nlohmann::json{{"biz_name", "test"},
                     {"pipeline", nlohmann::json::array(
                                      {{{"id", "n0"},
                                        {"node_type", "CountingNode"},
                                        {"depends_on", nlohmann::json::array()},
                                        {"unknown_top_level", 123}}})}},
      PipelineErrorCode::kUnknownField, "/pipeline/0/unknown_top_level"});
  cases.push_back(NegativeTestCase{
      "NodeMissingNodeType",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline",
           nlohmann::json::array({{{"id", "n0"},
                                   {"depends_on", nlohmann::json::array()},
                                   {"config", nlohmann::json::object()}}})}},
      PipelineErrorCode::kMissingField, "/pipeline/0/node_type"});
  cases.push_back(NegativeTestCase{
      "NodeEmptyNodeType",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline",
           nlohmann::json::array({{{"id", "n0"},
                                   {"node_type", ""},
                                   {"depends_on", nlohmann::json::array()}}})}},
      PipelineErrorCode::kFieldRange, "/pipeline/0/node_type"});
  cases.push_back(NegativeTestCase{
      "NodeConfigNotObject",
      nlohmann::json{{"biz_name", "test"},
                     {"pipeline", nlohmann::json::array(
                                      {{{"id", "n0"},
                                        {"node_type", "CountingNode"},
                                        {"depends_on", nlohmann::json::array()},
                                        {"config", "not_an_object"}}})}},
      PipelineErrorCode::kFieldType, "/pipeline/0/config"});
  cases.push_back(NegativeTestCase{
      "NodeUnregisteredNodeType",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline",
           nlohmann::json::array({{{"id", "n0"},
                                   {"node_type", "GhostUnregisteredNodeXYZ"},
                                   {"depends_on", nlohmann::json::array()}}})}},
      PipelineErrorCode::kUnknownNodeType, "/pipeline/0/node_type"});

  // --- DAG 校验 ---
  cases.push_back(NegativeTestCase{
      "DagMissingId",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline",
           nlohmann::json::array({{{"node_type", "CountingNode"},
                                   {"depends_on", nlohmann::json::array()}}})}},
      PipelineErrorCode::kMissingField, "/pipeline/0/id"});
  cases.push_back(NegativeTestCase{
      "DagDuplicateNodeId",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline",
           nlohmann::json::array({{{"id", "node_dup"},
                                   {"node_type", "CountingNode"},
                                   {"depends_on", nlohmann::json::array()}},
                                  {{"id", "node_dup"},
                                   {"node_type", "CountingNode"},
                                   {"depends_on", nlohmann::json::array()}}})}},
      PipelineErrorCode::kDuplicateNodeId, "/pipeline/1/id"});
  cases.push_back(NegativeTestCase{
      "DagMissingDependsOn",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline",
           nlohmann::json::array(
               {{{"id", "node_a"},
                 {"node_type", "CountingNode"},
                 {"depends_on", nlohmann::json::array()}},
                {{"id", "node_b"}, {"node_type", "CountingNode"}}})}},
      PipelineErrorCode::kMissingField, "/pipeline/1/depends_on"});
  cases.push_back(NegativeTestCase{
      "DagDependsOnNotArray",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline", nlohmann::json::array({{{"id", "node_a"},
                                               {"node_type", "CountingNode"},
                                               {"depends_on", "node_prev"}}})}},
      PipelineErrorCode::kFieldType, "/pipeline/0/depends_on"});
  cases.push_back(NegativeTestCase{
      "DagDependsOnNonStringItem",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline", nlohmann::json::array(
                           {{{"id", "node_a"},
                             {"node_type", "CountingNode"},
                             {"depends_on", nlohmann::json::array({123})}}})}},
      PipelineErrorCode::kFieldType, "/pipeline/0/depends_on/0"});
  cases.push_back(NegativeTestCase{
      "DagDependsOnEmptyStringItem",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline", nlohmann::json::array(
                           {{{"id", "node_a"},
                             {"node_type", "CountingNode"},
                             {"depends_on", nlohmann::json::array({""})}}})}},
      PipelineErrorCode::kFieldRange, "/pipeline/0/depends_on/0"});
  cases.push_back(NegativeTestCase{
      "DagDependsOnDuplicateItemInNode",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline", nlohmann::json::array(
                           {{{"id", "node_a"},
                             {"node_type", "CountingNode"},
                             {"depends_on", nlohmann::json::array()}},
                            {{"id", "node_b"},
                             {"node_type", "CountingNode"},
                             {"depends_on",
                              nlohmann::json::array({"node_a", "node_a"})}}})}},
      PipelineErrorCode::kInvalidDependency, "/pipeline/1/depends_on/1"});
  cases.push_back(NegativeTestCase{
      "DagSelfLoopCycle",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline",
           nlohmann::json::array(
               {{{"id", "node_a"},
                 {"node_type", "CountingNode"},
                 {"depends_on", nlohmann::json::array({"node_a"})}}})}},
      PipelineErrorCode::kDagCycle, "/pipeline/0/depends_on/0"});
  cases.push_back(NegativeTestCase{
      "DagNonExistentDependency",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline",
           nlohmann::json::array(
               {{{"id", "node_a"},
                 {"node_type", "CountingNode"},
                 {"depends_on", nlohmann::json::array({"ghost_dep"})}}})}},
      PipelineErrorCode::kInvalidDependency, "/pipeline/0/depends_on/0"});
  cases.push_back(NegativeTestCase{
      "DagCycle3Nodes",
      nlohmann::json{
          {"biz_name", "test"},
          {"pipeline",
           nlohmann::json::array(
               {{{"id", "node_a"},
                 {"node_type", "CountingNode"},
                 {"depends_on", nlohmann::json::array({"node_c"})}},
                {{"id", "node_b"},
                 {"node_type", "CountingNode"},
                 {"depends_on", nlohmann::json::array({"node_a"})}},
                {{"id", "node_c"},
                 {"node_type", "CountingNode"},
                 {"depends_on", nlohmann::json::array({"node_b"})}}})}},
      PipelineErrorCode::kDagCycle, "/pipeline"});

  for (const auto& tc : cases) {
    CountingEngine::Reset();
    CountingNode::Reset();

    Pipeline pipeline;
    PipelineDiagnostic diag;
    bool ok = pipeline.BuildFromJson(
        tc.input, &diag, ValidationPolicy::kPrivateExtensionCompatible);

    EXPECT_FALSE(ok) << "Test case '" << tc.name
                     << "' was expected to fail, but succeeded!";
    EXPECT_EQ(diag.code, tc.expected_code)
        << "Test case '" << tc.name
        << "': code mismatch (got: " << static_cast<int>(diag.code)
        << ", expected: " << static_cast<int>(tc.expected_code)
        << ", message: " << diag.message << ")";
    EXPECT_EQ(diag.path.rfind(tc.expected_path_prefix, 0), 0U)
        << "Test case '" << tc.name << "': path mismatch (got: '" << diag.path
        << "', expected prefix: '" << tc.expected_path_prefix << "')";

    // R1-ACC-006: 关键断言：校验失败发生在任何模型/算子 Create/Load/Init
    // 副作用之前
    EXPECT_EQ(CountingEngine::create_count.load(), 0)
        << "CountingEngine::Create had side effects during failed case '"
        << tc.name << "'";
    EXPECT_EQ(CountingEngine::load_count.load(), 0)
        << "CountingEngine::Load had side effects during failed case '"
        << tc.name << "'";
    EXPECT_EQ(CountingNode::create_count.load(), 0)
        << "CountingNode::Create had side effects during failed case '"
        << tc.name << "'";
    EXPECT_EQ(CountingNode::init_count.load(), 0)
        << "CountingNode::Init had side effects during failed case '" << tc.name
        << "'";
  }
}

// 4. 物化异常与细粒度错误诊断测试 (R1-ACC-001 & RECHECK-R1-001)
TEST_F(PipelineConfigTest, MaterializationExceptionsAndFineGrainedDiagnostics) {
  PipelineDiagnostic diag;

  // 4.1 配置文件不存在
  {
    Pipeline p;
    EXPECT_FALSE(p.BuildFromConfigFile("/non/existent/path.json", &diag));
    EXPECT_EQ(diag.code, PipelineErrorCode::kConfigFileOpen);
    EXPECT_EQ(diag.path, "/");
    EXPECT_EQ(p.GetState(), Pipeline::State::kFailed);
  }

  // 4.2 JSON 语法错误文件
  {
    std::string bad_json_path = "/tmp/bad_syntax_test.json";
    std::ofstream ofs(bad_json_path);
    ofs << "{ business_name: invalid_json, }";
    ofs.close();

    Pipeline p;
    EXPECT_FALSE(p.BuildFromConfigFile(bad_json_path, &diag));
    EXPECT_EQ(diag.code, PipelineErrorCode::kJsonParse);
    EXPECT_EQ(diag.path, "/");
    EXPECT_EQ(p.GetState(), Pipeline::State::kFailed);
    std::remove(bad_json_path.c_str());
  }

  // 4.3 Engine 构造函数抛异常
  {
    nlohmann::json cfg = {
        {"biz_name", "t"},
        {"models",
         nlohmann::json::array(
             {{{"model_id", "m1"}, {"engine_type", "throwing_ctor_engine"}}})},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0"},
                                 {"node_type", "CountingNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    Pipeline p;
    EXPECT_FALSE(p.BuildFromJson(
        cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(diag.code, PipelineErrorCode::kEngineCreateFailed);
    EXPECT_EQ(diag.path, "/models/0/engine_type");
    EXPECT_EQ(p.GetState(), Pipeline::State::kFailed);
  }

  // 4.4 Engine Load 抛异常
  {
    nlohmann::json cfg = {
        {"biz_name", "t"},
        {"models",
         nlohmann::json::array(
             {{{"model_id", "m1"}, {"engine_type", "throwing_load_engine"}}})},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0"},
                                 {"node_type", "CountingNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    Pipeline p;
    EXPECT_FALSE(p.BuildFromJson(
        cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(diag.code, PipelineErrorCode::kEngineLoadFailed);
    EXPECT_EQ(diag.path, "/models/0");
    EXPECT_EQ(p.GetState(), Pipeline::State::kFailed);
  }

  // 4.5 Engine Load 返回 false
  {
    nlohmann::json cfg = {
        {"biz_name", "t"},
        {"models",
         nlohmann::json::array(
             {{{"model_id", "m1"}, {"engine_type", "failing_load_engine"}}})},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0"},
                                 {"node_type", "CountingNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    Pipeline p;
    EXPECT_FALSE(p.BuildFromJson(
        cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(diag.code, PipelineErrorCode::kEngineLoadFailed);
    EXPECT_EQ(diag.path, "/models/0");
    EXPECT_EQ(p.GetState(), Pipeline::State::kFailed);
  }

  // 4.6 Node 构造函数抛异常
  {
    nlohmann::json cfg = {
        {"biz_name", "t"},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0"},
                                 {"node_type", "ThrowingCtorNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    Pipeline p;
    EXPECT_FALSE(p.BuildFromJson(
        cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(diag.code, PipelineErrorCode::kNodeCreateFailed);
    EXPECT_EQ(diag.path, "/pipeline/0/node_type");
    EXPECT_EQ(p.GetState(), Pipeline::State::kFailed);
  }

  // 4.7 Node Init 抛异常
  {
    nlohmann::json cfg = {
        {"biz_name", "t"},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0"},
                                 {"node_type", "ThrowingInitNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    Pipeline p;
    EXPECT_FALSE(p.BuildFromJson(
        cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(diag.code, PipelineErrorCode::kNodeInitFailed);
    EXPECT_EQ(diag.path, "/pipeline/0/config");
    EXPECT_EQ(p.GetState(), Pipeline::State::kFailed);
  }

  // 4.8 Node Init 返回 false
  {
    nlohmann::json cfg = {
        {"biz_name", "t"},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0"},
                                 {"node_type", "FailingInitNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    Pipeline p;
    EXPECT_FALSE(p.BuildFromJson(
        cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(diag.code, PipelineErrorCode::kNodeInitFailed);
    EXPECT_EQ(diag.path, "/pipeline/0/config");
    EXPECT_EQ(p.GetState(), Pipeline::State::kFailed);
  }

  // 4.9 内部未捕获异常被外层兜底拦截并映射为 kInternalException (FINAL-R1-003)
  {
    Pipeline p;
    SetInternalBuildHook(&p, []() {
      throw std::runtime_error("Simulated unhandled internal exception");
    });
    nlohmann::json cfg = {
        {"biz_name", "internal_exc_test"},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0"},
                                 {"node_type", "CountingNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    EXPECT_FALSE(p.BuildFromJson(
        cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(diag.code, PipelineErrorCode::kInternalException);
    EXPECT_EQ(diag.path, "/");
    EXPECT_TRUE(diag.message.find("Simulated unhandled internal exception") !=
                std::string::npos);
    EXPECT_EQ(p.GetState(), Pipeline::State::kFailed);
  }
}

// 5. 一次性构建契约与生命周期状态机测试 (R1-ACC-002 & RECHECK-R1-001)
TEST_F(PipelineConfigTest, OnceOnlyBuildContractAndStateMachineProtection) {
  PipelineDiagnostic diag;
  nlohmann::json valid_cfg = {
      {"biz_name", "state_test"},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0"},
                               {"node_type", "CountingNode"},
                               {"depends_on", nlohmann::json::array()}}})}};

  // 5.1 成功构建后的第二次 Build 在任何操作前直接拒绝
  {
    Pipeline p;
    EXPECT_EQ(p.GetState(), Pipeline::State::kEmpty);
    EXPECT_FALSE(p.IsReady());

    EXPECT_TRUE(p.BuildFromJson(valid_cfg, &diag,
                                ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(p.GetState(), Pipeline::State::kReady);
    EXPECT_TRUE(p.IsReady());
    int init_count_before = CountingNode::init_count.load();

    // 第二次 Build
    EXPECT_FALSE(p.BuildFromJson(
        valid_cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(diag.code, PipelineErrorCode::kInvalidBuildState);
    EXPECT_EQ(diag.path, "/");
    // 断言没有任何重复初始化副作用
    EXPECT_EQ(CountingNode::init_count.load(), init_count_before);
  }

  // 5.2 构建失败后的重试也直接拒绝
  {
    Pipeline p;
    nlohmann::json invalid_cfg = {
        {"biz_name", "state_test"},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0"},
                                 {"node_type", "FailingInitNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};

    EXPECT_FALSE(p.BuildFromJson(
        invalid_cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(p.GetState(), Pipeline::State::kFailed);
    EXPECT_FALSE(p.IsReady());

    // 失败实例上再次尝试 Build
    EXPECT_FALSE(p.BuildFromJson(
        valid_cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(diag.code, PipelineErrorCode::kInvalidBuildState);
  }

  // 5.3 未 Ready 或 Failed 状态下 Execute / Control 拒绝执行
  {
    Pipeline empty_p;
    AlgContext ctx;
    EXPECT_EQ(empty_p.Execute(&ctx), -1);
    EXPECT_EQ(empty_p.Control(1, "{}"), -1);

    Pipeline failed_p;
    nlohmann::json invalid_cfg = {{"biz_name", "fail"}};
    failed_p.BuildFromJson(invalid_cfg, nullptr,
                           ValidationPolicy::kPrivateExtensionCompatible);
    EXPECT_EQ(failed_p.Execute(&ctx), -1);
    EXPECT_EQ(failed_p.Control(1, "{}"), -1);
  }
}

// 6. ModelManager 重复 model_id 注册防御性拦截测试
TEST_F(PipelineConfigTest, ModelManagerDuplicateRejection) {
  ModelManager manager;
  auto eng1 = std::make_shared<CountingEngine>();
  auto eng2 = std::make_shared<CountingEngine>();

  EXPECT_TRUE(manager.RegisterModel("model_x", eng1));
  EXPECT_FALSE(manager.RegisterModel("model_x", eng2))
      << "Duplicate model_id registration must return false without "
         "overwriting";
  EXPECT_EQ(manager.GetModel<CountingEngine>("model_x"), eng1);
}

// 7. 并发模式边界测试 (R1-ACC-003)
TEST_F(PipelineConfigTest, ParallelModeWorkersBoundaries) {
  PipelineDiagnostic diag;

  // workers = 1
  {
    nlohmann::json cfg = {
        {"biz_name", "par_1"},
        {"execution_mode", "parallel"},
        {"max_parallel_workers", 1},
        {"pipeline",
         nlohmann::json::array({{{"id", "n1"},
                                 {"node_type", "CountingNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    Pipeline p;
    EXPECT_TRUE(p.BuildFromJson(cfg, &diag,
                                ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(p.GetExecutionMode(), Pipeline::ExecutionMode::PARALLEL);
  }

  // workers = 64
  {
    nlohmann::json cfg = {
        {"biz_name", "par_64"},
        {"execution_mode", "parallel"},
        {"max_parallel_workers", 64},
        {"pipeline",
         nlohmann::json::array({{{"id", "n1"},
                                 {"node_type", "CountingNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    Pipeline p;
    EXPECT_TRUE(p.BuildFromJson(cfg, &diag,
                                ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_EQ(p.GetExecutionMode(), Pipeline::ExecutionMode::PARALLEL);
  }
}

// 8. 双轨方言解析正例测试 (RFC 0015)
TEST_F(PipelineConfigTest, DualTrackDialectPositiveParsing) {
  nlohmann::json root = {
      {"biz_name", "dual_dialect_test"},
      {"models", nlohmann::json::array({
                     {{"model_id", "m_legacy"},
                      {"engine_type", "counting_engine"},
                      {"model_path", "./path/to/legacy"},
                      {"config", {{"batch_size", 4}}}},
                     {{"model_id", "m_mb_full"},
                      {"capability", "embedding"},
                      {"model_type", "bge_embedding"},
                      {"backend", "onnxruntime"},
                      {"model_path", "./models/bge/model.onnx"},
                      {"model_config", {{"max_length", 512}}},
                      {"backend_config", {{"device", "cpu"}}}},
                     {{"model_id", "m_mb_minimal"},
                      {"capability", "rerank"},
                      {"model_type", "bge_reranker"},
                      {"backend", "onnxruntime"},
                      {"model_path", "./models/rerank/model.onnx"}},
                 })},
      {"pipeline",
       nlohmann::json::array({{{"id", "n0"},
                               {"node_type", "CountingNode"},
                               {"depends_on", nlohmann::json::array()}}})}};

  ParsedPipelineConfig parsed_cfg;
  PipelineDiagnostic diag;
  EXPECT_TRUE(ParsePipelineConfig(root, &parsed_cfg, &diag));
  EXPECT_EQ(diag.code, PipelineErrorCode::kOk);
  ASSERT_EQ(parsed_cfg.models.size(), 3u);

  // Model 0: Legacy Engine Dialect
  const auto& m0 = parsed_cfg.models[0];
  EXPECT_EQ(m0.dialect, ModelConfigDialect::kLegacyEngine);
  EXPECT_EQ(m0.model_id, "m_legacy");
  EXPECT_EQ(m0.engine_type, "counting_engine");
  EXPECT_EQ(m0.model_path, "./path/to/legacy");
  EXPECT_EQ(m0.config.value("batch_size", 0), 4);
  EXPECT_EQ(m0.source_index, 0u);

  // Model 1: Model/Backend Dialect (Full)
  const auto& m1 = parsed_cfg.models[1];
  EXPECT_EQ(m1.dialect, ModelConfigDialect::kModelBackend);
  EXPECT_EQ(m1.model_id, "m_mb_full");
  EXPECT_EQ(m1.capability, "embedding");
  EXPECT_EQ(m1.model_type, "bge_embedding");
  EXPECT_EQ(m1.backend, "onnxruntime");
  EXPECT_EQ(m1.model_path, "./models/bge/model.onnx");
  EXPECT_TRUE(m1.model_config.is_object());
  EXPECT_EQ(m1.model_config.value("max_length", 0), 512);
  EXPECT_TRUE(m1.backend_config.is_object());
  EXPECT_EQ(m1.backend_config.value("device", ""), "cpu");
  EXPECT_EQ(m1.source_index, 1u);

  // Model 2: Model/Backend Dialect (Minimal with default empty configs)
  const auto& m2 = parsed_cfg.models[2];
  EXPECT_EQ(m2.dialect, ModelConfigDialect::kModelBackend);
  EXPECT_EQ(m2.model_id, "m_mb_minimal");
  EXPECT_EQ(m2.capability, "rerank");
  EXPECT_EQ(m2.model_type, "bge_reranker");
  EXPECT_EQ(m2.backend, "onnxruntime");
  EXPECT_EQ(m2.model_path, "./models/rerank/model.onnx");
  EXPECT_TRUE(m2.model_config.is_object());
  EXPECT_TRUE(m2.model_config.empty());
  EXPECT_TRUE(m2.backend_config.is_object());
  EXPECT_TRUE(m2.backend_config.empty());
  EXPECT_EQ(m2.source_index, 2u);
}

}  // namespace alg_framework
