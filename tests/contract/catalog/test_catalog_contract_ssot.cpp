#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "adapter/biz_adapter_registry.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"

namespace llm_edgeflow {

class CatalogContractSsotTest : public ::testing::Test {};

// 1. 验证所有生产算子均具备合法的 NodeDefinition 元数据
TEST_F(CatalogContractSsotTest, AllProductionNodesHaveValidDefinitions) {
  const auto& nodes = PipelineCatalog::Nodes();
  EXPECT_GE(nodes.size(), 11U);

  std::set<std::string> seen_types;
  for (const auto& node_def : nodes) {
    EXPECT_FALSE(node_def.node_type.empty());
    EXPECT_FALSE(node_def.category.empty());
    EXPECT_FALSE(node_def.description.empty());
    EXPECT_TRUE(seen_types.insert(node_def.node_type).second)
        << "Duplicate node definition in catalog: " << node_def.node_type;

    // 必须在 NodeFactory 中可实例化
    EXPECT_TRUE(NodeFactory::Instance().Has(node_def.node_type))
        << "Node type in catalog but missing in NodeFactory: "
        << node_def.node_type;
    auto instance = NodeFactory::Instance().Create(node_def.node_type);
    EXPECT_NE(instance, nullptr)
        << "Failed to create node instance: " << node_def.node_type;

    // FindNode 查询一致性
    const auto* found = PipelineCatalog::FindNode(node_def.node_type);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->node_type, node_def.node_type);
    EXPECT_EQ(found->category, node_def.category);
  }

  // 必须包含 11 个 Phase-1 Common 算子
  EXPECT_TRUE(seen_types.count("TextTemplateNode"));
  EXPECT_TRUE(seen_types.count("TextChunkNode"));
  EXPECT_TRUE(seen_types.count("TextRuleMatchNode"));
  EXPECT_TRUE(seen_types.count("StructuredJsonParseNode"));
  EXPECT_TRUE(seen_types.count("TextEmbeddingNode"));
  EXPECT_TRUE(seen_types.count("VectorTopKNode"));
  EXPECT_TRUE(seen_types.count("TextRerankNode"));
  EXPECT_TRUE(seen_types.count("LlmGenerateNode"));
  EXPECT_TRUE(seen_types.count("AsrTranscribeNode"));
  EXPECT_TRUE(seen_types.count("OcrDetectNode"));
  EXPECT_TRUE(seen_types.count("TextCorpusSourceNode"));
}

TEST_F(CatalogContractSsotTest, ProductionModelBackendCatalogHasNoFixtures) {
  std::set<std::string> model_types;
  for (const auto& model : PipelineCatalog::Models()) {
    EXPECT_FALSE(model.model_type.empty());
    EXPECT_FALSE(model.capability.empty());
    EXPECT_TRUE(model_types.insert(model.model_type).second);
  }
  EXPECT_TRUE(model_types.count("bge_embedding"));
  EXPECT_TRUE(model_types.count("bge_reranker"));
  EXPECT_TRUE(model_types.count("qwen_causal_lm"));
  for (const auto& model_type : model_types) {
    EXPECT_EQ(model_type.find("test_"), std::string::npos);
    EXPECT_EQ(model_type.find("mock"), std::string::npos);
  }

  std::set<std::string> backend_types;
  for (const auto& backend : PipelineCatalog::Backends()) {
    EXPECT_FALSE(backend.backend_type.empty());
    EXPECT_FALSE(backend.supported_protocols.empty());
    EXPECT_TRUE(backend_types.insert(backend.backend_type).second);
    EXPECT_EQ(backend.backend_type.find("test_"), std::string::npos);
    EXPECT_EQ(backend.backend_type.find("mock"), std::string::npos);
    EXPECT_TRUE(backend.backend_type == "onnxruntime" ||
                backend.backend_type == "llama_cpp");
  }
}

// 3. 验证 7 种业务契约在 PipelineCatalog 中完整注册
TEST_F(CatalogContractSsotTest, AllBizDefinitionsAreRegistered) {
  const auto& bizs = PipelineCatalog::Bizs();
  EXPECT_GE(bizs.size(), 7U);

  std::set<std::string> biz_names;
  for (const auto& b : bizs) {
    EXPECT_FALSE(b.biz_name.empty());
    biz_names.insert(b.biz_name);
  }

  EXPECT_TRUE(biz_names.count("keyword_match_v1"));
  EXPECT_TRUE(biz_names.count("entity_extract_0.6b_v1"));
  EXPECT_TRUE(biz_names.count("smart_doc_qa_v1"));
  EXPECT_TRUE(biz_names.count("dialogue_compliance_audit_v1"));
  EXPECT_TRUE(biz_names.count("multimodal_ocr_invoice_qa"));
  EXPECT_TRUE(biz_names.count("speech_audio_asr_intent_slot"));
  EXPECT_TRUE(biz_names.count("dense_cross_rerank_scoring"));

  for (const auto& name : biz_names) {
    const auto* found = PipelineCatalog::FindBiz(name);
    ASSERT_NE(found, nullptr) << "Missing biz definition: " << name;
    EXPECT_EQ(found->biz_name, name);
  }
}

// 4. 验证不存在类型查询返回 nullptr
TEST_F(CatalogContractSsotTest, FindReturnsNullptrForNonexistentEntities) {
  EXPECT_EQ(PipelineCatalog::FindNode("NonExistentNode12345"), nullptr);
  EXPECT_FALSE(
      PipelineCatalog::FindModel("non_existent_model_999").has_value());
  EXPECT_FALSE(
      PipelineCatalog::FindBackend("non_existent_backend_999").has_value());
  EXPECT_EQ(PipelineCatalog::FindBiz("non_existent_biz_xyz"), nullptr);
}

TEST_F(CatalogContractSsotTest, BusinessBatchRegistrationIsAtomic) {
  const std::string first = "atomic_catalog_probe_first";
  const std::string last = "atomic_catalog_probe_last";
  ASSERT_EQ(PipelineCatalog::FindBiz(first), nullptr);
  ASSERT_EQ(PipelineCatalog::FindBiz(last), nullptr);

  const auto& existing = PipelineCatalog::Bizs();
  ASSERT_FALSE(existing.empty());
  std::vector<BizDefinition> batch = {
      BizDefinition{first, "probe"},
      BizDefinition{existing.front().biz_name, "probe"},
      BizDefinition{last, "probe"},
  };

  EXPECT_FALSE(PipelineCatalog::RegisterBizDefinitions(batch));
  EXPECT_EQ(PipelineCatalog::FindBiz(first), nullptr);
  EXPECT_EQ(PipelineCatalog::FindBiz(last), nullptr);
}

// 5. 验证 PipelineCatalog::ToJson 序列化规范性与过滤逻辑
TEST_F(CatalogContractSsotTest, ToJsonSerializationAndFiltering) {
  auto full_catalog = PipelineCatalog::ToJson();
  EXPECT_EQ(full_catalog["schema_version"], 2);
  EXPECT_TRUE(full_catalog["nodes"].is_array());
  EXPECT_FALSE(full_catalog.contains("engines"));
  EXPECT_TRUE(full_catalog["models"].is_array());
  EXPECT_TRUE(full_catalog["backends"].is_array());
  EXPECT_TRUE(full_catalog["bizs"].is_array());
  EXPECT_FALSE(full_catalog.contains("businesses"));
  EXPECT_GE(full_catalog["nodes"].size(), 11U);
  EXPECT_GE(full_catalog["bizs"].size(), 7U);
  for (const auto& node : full_catalog["nodes"]) {
    for (const auto& port : node["inputs"]) {
      EXPECT_FALSE(port.contains("allow_override"));
    }
    for (const auto& port : node["outputs"]) {
      EXPECT_FALSE(port.contains("allow_override"));
    }
  }

  // 业务过滤查询
  auto km_catalog = PipelineCatalog::ToJson("keyword_match_v1");
  EXPECT_EQ(km_catalog["schema_version"], 2);
  EXPECT_FALSE(km_catalog["nodes"].empty());
  EXPECT_EQ(km_catalog["bizs"].size(), 1U);
  EXPECT_EQ(km_catalog["bizs"][0]["biz_name"], "keyword_match_v1");
  EXPECT_FALSE(km_catalog["bizs"][0].contains("business_name"));
  EXPECT_FALSE(km_catalog["bizs"][0].contains("demo_business"));

  bool found_match_node = false;
  for (const auto& item : km_catalog["nodes"]) {
    if (item["node_type"] == "TextRuleMatchNode") {
      found_match_node = true;
      EXPECT_FALSE(item.contains("business_names"));
    }
  }
  EXPECT_TRUE(found_match_node);
}

}  // namespace llm_edgeflow
