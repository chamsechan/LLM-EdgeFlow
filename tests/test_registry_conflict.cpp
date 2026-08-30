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

inline NodeDefinition MakeTestNodeDef(const std::string& type) {
  NodeDefinition def;
  def.node_type = type;
  def.category = "test";
  def.description = "test node " + type;
  def.parallel_safe = true;
  return def;
}

inline ModelDefinition MakeTestModelDef(const std::string& type) {
  ModelDefinition def;
  def.model_type = type;
  def.capability = "embedding";
  def.description = "test model " + type;
  def.required_protocol = ExecutionProtocol::kTensorGraph;
  def.concurrency = InferenceConcurrency::kConcurrent;
  return def;
}

class DummyNode : public INode {
 public:
  inline static constexpr char kNodeType[] = "DummyNode";
  bool Init(const NodeInitContext&) override { return true; }
  int Process(AlgContext*) override { return 0; }
  NodeControlResult Control(int, const std::string&) override {
    return NodeControlResult::Handled(0);
  }
  const std::string& Name() const override {
    static const std::string name = kNodeType;
    return name;
  }
};
REGISTER_NODE_WITH_DEFINITION(DummyNode, MakeTestNodeDef(DummyNode::kNodeType));

class DummyModel : public IEmbeddingModel {
 public:
  inline static constexpr char kModelType[] = "dummy_model";
  static std::shared_ptr<IModel> Create(const ModelCreateContext&,
                                        std::string*) {
    return std::make_shared<DummyModel>();
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
REGISTER_MODEL_WITH_DEFINITION(DummyModel,
                               MakeTestModelDef(DummyModel::kModelType));

TEST(RegistryConflictNodeTest, DuplicateNodeFailClosed) {
  ASSERT_FALSE(NodeFactory::Instance().HasConflict());
  ASSERT_FALSE(ModelRegistry::Instance().HasConflict());
  EXPECT_FALSE(NodeFactory::Instance().Register(
      DummyNode::kNodeType, []() { return std::make_unique<DummyNode>(); },
      MakeTestNodeDef(DummyNode::kNodeType)));
  EXPECT_TRUE(NodeFactory::Instance().HasConflict());
  EXPECT_NE(NodeFactory::Instance().Create(DummyNode::kNodeType), nullptr);

  Pipeline pipe;
  PipelineDiagnostic diag;
  nlohmann::json cfg = {
      {"biz_name", "conflict_node_test"},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0_DummyNode"},
                               {"node_type", DummyNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};
  EXPECT_FALSE(pipe.BuildFromJson(
      cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
  EXPECT_EQ(diag.code, PipelineErrorCode::kRegistryConflict);
  EXPECT_EQ(diag.path, "/pipeline");
}

TEST(RegistryConflictModelTest, DuplicateModelFailClosed) {
  ASSERT_FALSE(NodeFactory::Instance().HasConflict());
  ASSERT_FALSE(ModelRegistry::Instance().HasConflict());
  EXPECT_FALSE(ModelRegistry::Instance().Register(
      MakeTestModelDef(DummyModel::kModelType), DummyModel::Create));
  EXPECT_TRUE(ModelRegistry::Instance().HasConflict());

  Pipeline pipe;
  PipelineDiagnostic diag;
  nlohmann::json cfg = {
      {"biz_name", "conflict_model_test"},
      {"models",
       nlohmann::json::array({{{"model_id", "m1"},
                               {"capability", "embedding"},
                               {"model_type", DummyModel::kModelType},
                               {"backend", "unused_backend"},
                               {"model_path", "unused.bin"},
                               {"model_config", nlohmann::json::object()},
                               {"backend_config", nlohmann::json::object()}}})},
      {"pipeline",
       nlohmann::json::array({{{"id", "node_0_DummyNode"},
                               {"node_type", DummyNode::kNodeType},
                               {"depends_on", nlohmann::json::array()}}})}};
  EXPECT_FALSE(pipe.BuildFromJson(
      cfg, &diag, ValidationPolicy::kPrivateExtensionCompatible));
  EXPECT_EQ(diag.code, PipelineErrorCode::kRegistryConflict);
  EXPECT_EQ(diag.path, "/models");
}

}  // namespace alg_framework
