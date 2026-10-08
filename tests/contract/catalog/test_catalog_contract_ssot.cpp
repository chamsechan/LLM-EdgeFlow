#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "adapter/io_catalog.h"
#include "adapter/io_converter_registry.h"
#include "core/node_interface.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "engine/backend_registry.h"
#include "engine/model_registry.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {

class CatalogContractSsotTest : public ::testing::Test {};

// 1. 验证所有生产算子均具备合法的 NodeDefinition 元数据
TEST_F(CatalogContractSsotTest, AllProductionNodesHaveValidDefinitions) {
  const auto nodes = PipelineCatalog::Nodes();
  EXPECT_GE(nodes.size(), 11U);

  // R1：NodeRegistry::ListDefinitions() 等于 PipelineCatalog::Nodes()
  const auto reg_defs = NodeRegistry::Instance().ListDefinitions();
  EXPECT_EQ(reg_defs.size(), nodes.size());
  EXPECT_TRUE(
      std::is_sorted(nodes.begin(), nodes.end(),
                     [](const NodeDefinition& a, const NodeDefinition& b) {
                       return a.node_type < b.node_type;
                     }));

  std::set<std::string> seen_types;
  for (const auto& node_def : nodes) {
    EXPECT_FALSE(node_def.node_type.empty());
    EXPECT_FALSE(node_def.category.empty());
    EXPECT_FALSE(node_def.description.empty());
    EXPECT_TRUE(seen_types.insert(node_def.node_type).second)
        << "Duplicate node definition in catalog: " << node_def.node_type;

    // 必须在 NodeRegistry 中可实例化
    EXPECT_TRUE(NodeRegistry::Instance().Has(node_def.node_type))
        << "Node type in catalog but missing in NodeRegistry: "
        << node_def.node_type;
    auto instance = NodeRegistry::Instance().Create(node_def.node_type);
    EXPECT_NE(instance, nullptr)
        << "Failed to create node instance: " << node_def.node_type;
    if (node_def.node_type == "TextChunkNode") {
      EXPECT_EQ(instance->Name(), "TextChunkNode");
    }

    // FindNode 查询一致性
    const auto found = PipelineCatalog::FindNode(node_def.node_type);
    ASSERT_TRUE(found.has_value());
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

TEST_F(CatalogContractSsotTest, NodeCatalogExportsElementDeclarations) {
  const auto definition = PipelineCatalog::FindNode("LlmGenerateNode");
  ASSERT_TRUE(definition.has_value());
  const auto exported = PipelineCatalog::NodeToJson(*definition);
  bool saw_stop_words = false;
  for (const auto& field : exported.at("config_fields")) {
    if (field.at("name") != "stop_words") continue;
    saw_stop_words = true;
    EXPECT_EQ(field.at("type"), "array");
    ASSERT_TRUE(field.contains("items"));
    EXPECT_EQ(field.at("items").at("type"), "string");
  }
  EXPECT_TRUE(saw_stop_words);

  // 标量参数没有元素说明；绑定模型的说明由框架按类别生成
  for (const auto& field : exported.at("config_fields")) {
    if (field.at("name") == "max_tokens") EXPECT_FALSE(field.contains("items"));
    if (field.at("name") == "bind_model") {
      const std::string semantic = field.at("semantic");
      EXPECT_NE(semantic.find("models[].model_id"), std::string::npos);
      EXPECT_NE(semantic.find("llm"), std::string::npos);
    }
  }
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
  EXPECT_TRUE(model_types.count("vision_document"));
  EXPECT_TRUE(model_types.count("whisper_asr"));
  const auto embedding = PipelineCatalog::FindModel("generated_text_embedding");
  ASSERT_TRUE(embedding.has_value());
  EXPECT_EQ(embedding->capability, "embedding");
  EXPECT_EQ(embedding->required_protocol,
            ExecutionProtocol::kGeneratedTokenEmbedding);
  const auto vision = PipelineCatalog::FindModel("vision_document");
  ASSERT_TRUE(vision.has_value());
  EXPECT_EQ(vision->required_protocol, ExecutionProtocol::kImageTextGeneration);
  const auto asr = PipelineCatalog::FindModel("whisper_asr");
  ASSERT_TRUE(asr.has_value());
  EXPECT_EQ(asr->capability, "asr");
  EXPECT_EQ(asr->required_protocol, ExecutionProtocol::kAudioTranscription);
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
                backend.backend_type == "llama_cpp" ||
                backend.backend_type == "kite_llm" ||
                backend.backend_type == "whisper_cpp");
  }
}

// 4. 验证不存在类型查询返回 nullptr
TEST_F(CatalogContractSsotTest, FindReturnsEmptyForNonexistentEntities) {
  EXPECT_FALSE(PipelineCatalog::FindNode("NonExistentNode12345").has_value());
  EXPECT_EQ(NodeRegistry::Instance().Create("NonExistentNode123"), nullptr);
  EXPECT_FALSE(
      PipelineCatalog::FindModel("non_existent_model_999").has_value());
  EXPECT_FALSE(
      PipelineCatalog::FindBackend("non_existent_backend_999").has_value());
}

// 5. 验证 PipelineCatalog::ToJson 序列化规范性与过滤逻辑
TEST_F(CatalogContractSsotTest, ToJsonSerializationAndFiltering) {
  auto full_catalog = PipelineCatalog::ToJson();
  EXPECT_TRUE(full_catalog["nodes"].is_array());
  EXPECT_FALSE(full_catalog.contains("engines"));
  EXPECT_TRUE(full_catalog["models"].is_array());
  EXPECT_TRUE(full_catalog["backends"].is_array());
  EXPECT_GE(full_catalog["nodes"].size(), 11U);
  for (const auto& node : full_catalog["nodes"]) {
    EXPECT_TRUE(node["model_dependencies"].is_array());
    EXPECT_FALSE(node.contains("model_capability"));
    EXPECT_FALSE(node.contains("model_config_field"));
    for (const auto& port : node["inputs"]) {
      EXPECT_FALSE(port.contains("allow_override"));
    }
    for (const auto& port : node["outputs"]) {
      EXPECT_FALSE(port.contains("allow_override"));
    }
  }

  bool found_match_node = false;
  for (const auto& item : full_catalog["nodes"]) {
    if (item["node_type"] == "TextRuleMatchNode") {
      found_match_node = true;
      EXPECT_FALSE(item.contains("business_names"));
    }
  }
  EXPECT_TRUE(found_match_node);
}

// 5b. 验证 IoCatalog 聚合与对外规范性：converter 直接列出。
TEST_F(CatalogContractSsotTest, IoCatalogListsConverters) {
  auto full_catalog = IoCatalog::ToJson();
  EXPECT_TRUE(full_catalog["nodes"].is_array());
  EXPECT_TRUE(full_catalog["models"].is_array());
  EXPECT_TRUE(full_catalog["backends"].is_array());
  EXPECT_TRUE(full_catalog["input_converters"].is_array());
  EXPECT_TRUE(full_catalog["output_converters"].is_array());
}

TEST_F(CatalogContractSsotTest, CatalogHasNoRequestIdPort) {
  const auto catalog = IoCatalog::ToJson();
  for (const char* kind : {"input_converters", "output_converters"}) {
    for (const auto& converter : catalog.at(kind)) {
      for (const auto& port : converter.at("logical_ports")) {
        EXPECT_NE(port.at("key"), "raw_request_ids")
            << converter.at("type") << "/" << converter.at("name");
      }
    }
  }
}

TEST_F(CatalogContractSsotTest, IoCatalogExportsKeywordConverterDeclarations) {
  const auto catalog = IoCatalog::ToJson();
  const auto find = [&](const char* kind, const char* type) {
    const auto& list = catalog.at(kind);
    return std::find_if(list.begin(), list.end(), [&](const auto& converter) {
      return converter.at("type") == type &&
             converter.at("name") == "keyword_match";
    });
  };
  const auto input_it = find("input_converters", "keyword_in");
  ASSERT_NE(input_it, catalog.at("input_converters").end());
  EXPECT_EQ(input_it->at("external_type"), "CompanyOperatorKeywordInput");
  EXPECT_EQ(input_it->at("service_type"), COMPANY_MOCK_SERVICE_KEYWORD_MATCH);
  EXPECT_EQ(input_it->at("slot").at("type_id"), "CompanyOperatorKeywordInput");
  EXPECT_EQ(input_it->at("slot").at("type_suffix"), "keyword_in");
  EXPECT_EQ(input_it->at("slot").at("required"), true);
  EXPECT_TRUE(input_it->at("config_fields").empty());

  const auto output_it = find("output_converters", "keyword_out");
  ASSERT_NE(output_it, catalog.at("output_converters").end());
  EXPECT_EQ(output_it->at("external_type"), "CompanyOperatorKeywordOutput");
  EXPECT_EQ(output_it->at("slot").at("type_suffix"), "keyword_out");
  // 尺寸参数由 converter 声明，最大值由框架按平台上限补齐。
  ASSERT_EQ(output_it->at("config_fields").size(), 1U);
  const auto& size = output_it->at("config_fields").at(0);
  EXPECT_EQ(size.at("name"), "match_result_json_max_bytes");
  EXPECT_EQ(size.at("type"), "integer");
  EXPECT_EQ(size.at("default"), 2047);
  EXPECT_EQ(size.at("minimum"), 1);
  EXPECT_EQ(size.at("maximum"), 65536);
}

// R6: 并发同名注册只有一个成功，另一方失败锁存
TEST_F(CatalogContractSsotTest, ConcurrentSameNameRegistrationSingleWinner) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  const std::string race_type = "ConcurrentRaceNode";
  ASSERT_FALSE(NodeRegistry::Instance().Has(race_type));

  std::atomic<int> start_flag{0};
  std::atomic<int> success_count{0};
  std::atomic<int> fail_count{0};

  constexpr int kThreads = 4;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      while (start_flag.load() == 0) {
        std::this_thread::yield();
      }
      NodeDefinition def;
      def.node_type = race_type;
      def.category = "race";
      def.description = "thread " + std::to_string(i);
      bool ok = NodeRegistry::Instance().Register(
          race_type, []() -> std::unique_ptr<INode> { return nullptr; }, def);
      if (ok) {
        success_count.fetch_add(1);
      } else {
        fail_count.fetch_add(1);
      }
    });
  }

  start_flag.store(1);
  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), 1);
  EXPECT_EQ(fail_count.load(), kThreads - 1);
  EXPECT_TRUE(NodeRegistry::Instance().HasConflict());
  EXPECT_TRUE(NodeRegistry::Instance().Has(race_type));
  EXPECT_TRUE(PipelineCatalog::FindNode(race_type).has_value());
}

// R6: 并发不同类型相同 Control ID 的冲突在提交时被发现
TEST_F(CatalogContractSsotTest, ConcurrentConflictingControlIdDetected) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  const std::string node_a = "ConcurrentControlNodeA";
  const std::string node_b = "ConcurrentControlNodeB";
  ASSERT_FALSE(NodeRegistry::Instance().Has(node_a));
  ASSERT_FALSE(NodeRegistry::Instance().Has(node_b));

  std::atomic<int> start_flag{0};
  std::atomic<int> success_count{0};
  std::atomic<int> fail_count{0};

  auto make_conflicting_def = [](const std::string& type,
                                 const std::string& cmd_name) {
    NodeDefinition def;
    def.node_type = type;
    def.category = "test";
    def.description = "control conflict";
    ControlCommandDefinition cmd;
    cmd.cmd_id = 8888;
    cmd.name = cmd_name;
    cmd.payload_schema = nlohmann::json::object();
    def.control_commands.push_back(cmd);
    return def;
  };

  std::thread t1([&]() {
    while (start_flag.load() == 0) std::this_thread::yield();
    bool ok = NodeRegistry::Instance().Register(
        node_a, []() -> std::unique_ptr<INode> { return nullptr; },
        make_conflicting_def(node_a, "cmd_a"));
    if (ok)
      success_count.fetch_add(1);
    else
      fail_count.fetch_add(1);
  });

  std::thread t2([&]() {
    while (start_flag.load() == 0) std::this_thread::yield();
    bool ok = NodeRegistry::Instance().Register(
        node_b, []() -> std::unique_ptr<INode> { return nullptr; },
        make_conflicting_def(node_b, "cmd_b"));
    if (ok)
      success_count.fetch_add(1);
    else
      fail_count.fetch_add(1);
  });

  start_flag.store(1);
  t1.join();
  t2.join();

  EXPECT_LE(success_count.load(), 1);
  EXPECT_GE(fail_count.load(), 1);
  EXPECT_TRUE(NodeRegistry::Instance().HasConflict());
}

// R7: 并发读快照与新增注册只能得到完整条目；旧快照不受影响
TEST_F(CatalogContractSsotTest,
       ConcurrentSnapshotReadersWhileRegisteringNodes) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  const auto initial_snapshot = NodeRegistry::Instance().Snapshot();
  ASSERT_FALSE(initial_snapshot.definitions.empty());
  const size_t initial_count = initial_snapshot.definitions.size();

  std::atomic<bool> writer_done{false};
  std::atomic<bool> readers_ok{true};

  std::thread reader([&]() {
    while (!writer_done.load(std::memory_order_relaxed)) {
      const auto snap = NodeRegistry::Instance().Snapshot();
      EXPECT_GE(snap.definitions.size(), initial_count);
      bool sorted =
          std::is_sorted(snap.definitions.begin(), snap.definitions.end(),
                         [](const NodeDefinition& a, const NodeDefinition& b) {
                           return a.node_type < b.node_type;
                         });
      if (!sorted) readers_ok.store(false);
      for (const auto& def : snap.definitions) {
        if (def.node_type.empty() || def.category.empty()) {
          readers_ok.store(false);
        }
      }
    }
  });

  for (int i = 0; i < 16; ++i) {
    const std::string node_name = "SnapshotWriterNode_" + std::to_string(i);
    NodeDefinition def;
    def.node_type = node_name;
    def.category = "snapshot_test";
    def.description = "node " + std::to_string(i);
    bool ok = NodeRegistry::Instance().Register(
        node_name, []() -> std::unique_ptr<INode> { return nullptr; }, def);
    EXPECT_TRUE(ok);
  }
  writer_done.store(true, std::memory_order_release);
  reader.join();

  EXPECT_TRUE(readers_ok.load());
  EXPECT_EQ(initial_snapshot.definitions.size(), initial_count);
}

// R8: ScopedNodeState 测试隔离与还原
TEST_F(CatalogContractSsotTest, ScopedNodeStateRestoresCleanly) {
  const size_t original_count =
      NodeRegistry::Instance().ListDefinitions().size();
  const bool original_conflict = NodeRegistry::Instance().HasConflict();
  {
    test_support::RegistryTestAccess::ScopedNodeState scoped;
    NodeDefinition def;
    def.node_type = "ScopedTempNode";
    def.category = "temp";
    def.description = "temp";
    EXPECT_TRUE(NodeRegistry::Instance().Register(
        "ScopedTempNode", []() -> std::unique_ptr<INode> { return nullptr; },
        def));
    EXPECT_TRUE(NodeRegistry::Instance().Has("ScopedTempNode"));
    EXPECT_EQ(NodeRegistry::Instance().ListDefinitions().size(),
              original_count + 1);
  }
  EXPECT_FALSE(NodeRegistry::Instance().Has("ScopedTempNode"));
  EXPECT_EQ(NodeRegistry::Instance().ListDefinitions().size(), original_count);
  EXPECT_EQ(NodeRegistry::Instance().HasConflict(), original_conflict);
}

}  // namespace llm_edgeflow
