#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "adapter/biz_adapter_registry.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "engine/engine_registry.h"

namespace alg_framework {

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

// 2. 验证所有推理引擎均具备合法的 EngineDefinition 元数据
TEST_F(CatalogContractSsotTest, AllInferenceEnginesHaveValidDefinitions) {
  const auto& engines = PipelineCatalog::Engines();
  EXPECT_GE(engines.size(), 7U);

  std::set<std::string> seen_engines;
  for (const auto& engine_def : engines) {
    EXPECT_FALSE(engine_def.engine_type.empty());
    EXPECT_FALSE(engine_def.capability.empty());
    EXPECT_FALSE(engine_def.description.empty());
    EXPECT_TRUE(seen_engines.insert(engine_def.engine_type).second)
        << "Duplicate engine definition in catalog: " << engine_def.engine_type;

    // 必须在 EngineFactory 中可实例化
    EXPECT_TRUE(EngineFactory::Instance().Has(engine_def.engine_type))
        << "Engine type in catalog but missing in EngineFactory: "
        << engine_def.engine_type;
    auto instance = EngineFactory::Instance().Create(engine_def.engine_type);
    ASSERT_NE(instance, nullptr)
        << "Failed to create engine instance: " << engine_def.engine_type;
    EXPECT_EQ(instance->EngineType(), engine_def.engine_type);

    // FindEngine 查询一致性
    const auto* found = PipelineCatalog::FindEngine(engine_def.engine_type);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->engine_type, engine_def.engine_type);
    EXPECT_EQ(found->capability, engine_def.capability);
  }

  // 必须包含全部 Mock 与 Real 引擎类型
  EXPECT_TRUE(seen_engines.count("mock_npu_embedding"));
  EXPECT_TRUE(seen_engines.count("mock_npu_llm"));
  EXPECT_TRUE(seen_engines.count("mock_npu_rerank"));
  EXPECT_TRUE(seen_engines.count("mock_npu_ocr"));
  EXPECT_TRUE(seen_engines.count("mock_npu_asr"));
  EXPECT_TRUE(seen_engines.count("onnx_embedding"));
  EXPECT_FALSE(seen_engines.count("onnx_rerank"));
  EXPECT_TRUE(seen_engines.count("llama_cpp"));
}

// 3. 验证 7 种业务契约在 PipelineCatalog 中完整注册
TEST_F(CatalogContractSsotTest, AllBusinessDefinitionsAreRegistered) {
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
  EXPECT_EQ(PipelineCatalog::FindEngine("non_existent_engine_999"), nullptr);
  EXPECT_EQ(PipelineCatalog::FindEngine("onnx_rerank"), nullptr);
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
  EXPECT_EQ(full_catalog["schema_version"], 1);
  EXPECT_TRUE(full_catalog["nodes"].is_array());
  EXPECT_TRUE(full_catalog["engines"].is_array());
  EXPECT_TRUE(full_catalog["bizs"].is_array());
  EXPECT_GE(full_catalog["nodes"].size(), 11U);
  EXPECT_GE(full_catalog["engines"].size(), 7U);
  EXPECT_GE(full_catalog["bizs"].size(), 7U);
  EXPECT_GE(full_catalog["bizs"].size(), 7U);
  for (const auto& engine : full_catalog["engines"]) {
    ASSERT_TRUE(engine.contains("thread_model"));
    EXPECT_TRUE(engine["thread_model"] == "serialized" ||
                engine["thread_model"] == "concurrent");
  }

  // 业务过滤查询
  auto km_catalog = PipelineCatalog::ToJson("keyword_match_v1");
  EXPECT_EQ(km_catalog["schema_version"], 1);
  EXPECT_FALSE(km_catalog["nodes"].empty());
  EXPECT_EQ(km_catalog["businesses"].size(), 1U);
  EXPECT_EQ(km_catalog["businesses"][0]["business_name"], "keyword_match_v1");

  bool found_match_node = false;
  for (const auto& item : km_catalog["nodes"]) {
    if (item["node_type"] == "TextRuleMatchNode") {
      found_match_node = true;
    }
  }
  EXPECT_TRUE(found_match_node);
}

}  // namespace alg_framework
