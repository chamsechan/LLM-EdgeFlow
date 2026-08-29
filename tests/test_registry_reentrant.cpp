#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "core/node_base.h"
#include "core/node_registry.h"
#include "core/pipeline.h"
#include "core/pipeline_diagnostic.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"

namespace alg_framework {

static NodeDefinition MakeTestNodeDef(const std::string& type) {
  NodeDefinition def;
  def.node_type = type;
  def.category = "test";
  def.description = "test node " + type;
  def.parallel_safe = true;
  return def;
}

static ModelDefinition MakeTestModelDef(const std::string& type) {
  ModelDefinition def;
  def.model_type = type;
  def.capability = "embedding";
  def.description = "test model " + type;
  def.required_protocol = ExecutionProtocol::kTensorGraph;
  def.concurrency = InferenceConcurrency::kConcurrent;
  return def;
}

// RECHECK-R1-003: 构造期重入自身 Registry 查询，测试锁粒度是否正确释放
class ReentrantNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "ReentrantNode";
  ReentrantNode() {
    // 构造期间同步调用 NodeFactory 查询
    volatile bool has = NodeFactory::Instance().Has("ReentrantNode");
    (void)has;
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
REGISTER_NODE_WITH_DEFINITION(ReentrantNode,
                              MakeTestNodeDef(ReentrantNode::kNodeType));

class ReentrantModel : public IEmbeddingModel {
 public:
  inline static constexpr char kModelType[] = "reentrant_model";

  ReentrantModel() {
    // 构造期间同步调用 ModelRegistry 查询
    volatile bool has = ModelRegistry::Instance().Has(kModelType);
    (void)has;
  }
  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string*) {
    return std::make_shared<ReentrantModel>();
  }
  size_t GetMaxBatchSize() const noexcept override { return 1; }
  const std::string& ModelType() const noexcept override {
    static const std::string type = kModelType;
    return type;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "embedding";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  int Embed(const TextBatch&, const EmbeddingOptions&,
            EmbeddingBatch*) noexcept override {
    return 0;
  }
};
REGISTER_MODEL_WITH_DEFINITION(ReentrantModel,
                               MakeTestModelDef(ReentrantModel::kModelType));

TEST(RegistryReentrantTest, ReentrantCreationZeroDeadlock) {
  // 1. 同步测试 Node 构造期重入 NodeFactory
  {
    Pipeline p;
    PipelineDiagnostic diag;
    nlohmann::json cfg = {
        {"business_name", "reentrant_node_test"},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0_ReentrantNode"},
                                 {"node_type", "ReentrantNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    EXPECT_TRUE(p.BuildFromJson(cfg, &diag,
                                ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_TRUE(p.IsReady());
  }

  // 2. 同步测试 Model 构造期重入 ModelRegistry
  {
    Pipeline p;
    PipelineDiagnostic diag;
    nlohmann::json cfg = {
        {"business_name", "reentrant_model_test"},
        {"models", nlohmann::json::array(
                       {{{"model_id", "m1"},
                         {"capability", "embedding"},
                         {"model_type", ReentrantModel::kModelType},
                         {"backend", "test_tensor_backend"},
                         {"model_path", "reentrant.bin"},
                         {"model_config", nlohmann::json::object()},
                         {"backend_config", nlohmann::json::object()}}})},
        {"pipeline",
         nlohmann::json::array({{{"id", "node_0_ReentrantNode"},
                                 {"node_type", "ReentrantNode"},
                                 {"depends_on", nlohmann::json::array()}}})}};
    EXPECT_TRUE(p.BuildFromJson(cfg, &diag,
                                ValidationPolicy::kPrivateExtensionCompatible));
    EXPECT_TRUE(p.IsReady());
  }
}

}  // namespace alg_framework
