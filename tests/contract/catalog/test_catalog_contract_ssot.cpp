#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "adapter/io_binding_registry.h"
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

  // R1: NodeRegistry::ListDefinitions() equals PipelineCatalog::Nodes()
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

TEST_F(CatalogContractSsotTest, BizContractsDoNotDependOnDeploymentVariants) {
  for (const char* biz : {"entity_extract_v1", "smart_doc_qa_v1"}) {
    EXPECT_TRUE(PipelineCatalog::FindBiz(biz).has_value());
  }
  for (const char* name :
       {"entity_extract_0.6b_v1", "entity_extract_llamacpp_0.6b_v1",
        "smart_doc_qa_onnx_llamacpp_v1", "smart_doc_qa_rerank_llm_v1"}) {
    EXPECT_FALSE(PipelineCatalog::FindBiz(name));
    EXPECT_EQ(IoBindingRegistry::Instance().FindBinding(name), nullptr);
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

// 3. 验证 7 种业务契约在 PipelineCatalog 中完整注册
TEST_F(CatalogContractSsotTest, AllBizDefinitionsAreRegistered) {
  const auto bizs = PipelineCatalog::Bizs();
  EXPECT_GE(bizs.size(), 7U);

  std::set<std::string> biz_names;
  for (const auto& b : bizs) {
    EXPECT_FALSE(b.biz_name.empty());
    biz_names.insert(b.biz_name);
  }

  EXPECT_TRUE(biz_names.count("keyword_match_v1"));
  EXPECT_TRUE(biz_names.count("entity_extract_v1"));
  EXPECT_TRUE(biz_names.count("smart_doc_qa_v1"));
  EXPECT_TRUE(biz_names.count("dialogue_compliance_audit_v1"));
  EXPECT_TRUE(biz_names.count("multimodal_ocr_invoice_qa"));
  EXPECT_TRUE(biz_names.count("speech_audio_asr_intent_slot"));
  EXPECT_TRUE(biz_names.count("dense_cross_rerank_scoring"));

  for (const auto& name : biz_names) {
    const auto found = PipelineCatalog::FindBiz(name);
    ASSERT_TRUE(found.has_value()) << "Missing biz definition: " << name;
    EXPECT_EQ(found->biz_name, name);
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
  EXPECT_FALSE(PipelineCatalog::FindBiz("non_existent_biz_xyz").has_value());
}

TEST_F(CatalogContractSsotTest, BusinessBatchRegistrationIsAtomic) {
  const std::string first = "atomic_catalog_probe_first";
  const std::string last = "atomic_catalog_probe_last";
  ASSERT_FALSE(PipelineCatalog::FindBiz(first).has_value());
  ASSERT_FALSE(PipelineCatalog::FindBiz(last).has_value());

  const auto existing = PipelineCatalog::Bizs();
  ASSERT_FALSE(existing.empty());
  std::vector<BizDefinition> batch = {
      BizDefinition{first, "probe"},
      BizDefinition{existing.front().biz_name, "probe"},
      BizDefinition{last, "probe"},
  };

  EXPECT_FALSE(PipelineCatalog::RegisterBizDefinitions(batch));
  EXPECT_FALSE(PipelineCatalog::FindBiz(first).has_value());
  EXPECT_FALSE(PipelineCatalog::FindBiz(last).has_value());
}

TEST_F(CatalogContractSsotTest,
       ValueSnapshotsRemainStableDuringConcurrentRegistration) {
  const auto original = PipelineCatalog::Snapshot();
  ASSERT_FALSE(original.bizs.empty());
  const std::string original_first = original.bizs.front().biz_name;
  std::atomic<bool> registration_ok{true};
  std::thread registrar([&]() {
    for (int i = 0; i < 16; ++i) {
      if (!PipelineCatalog::RegisterBizDefinition(BizDefinition{
              "snapshot_concurrency_probe_" + std::to_string(i), "probe"})) {
        registration_ok = false;
      }
    }
  });
  for (int i = 0; i < 32; ++i) {
    const auto current = PipelineCatalog::Snapshot();
    EXPECT_NE(current.FindBiz(original_first), nullptr);
  }
  registrar.join();

  EXPECT_TRUE(registration_ok.load());
  EXPECT_EQ(original.bizs.front().biz_name, original_first);
  EXPECT_EQ(original.FindBiz("snapshot_concurrency_probe_0"), nullptr);
  EXPECT_TRUE(
      PipelineCatalog::FindBiz("snapshot_concurrency_probe_0").has_value());
}

// 5. 验证 PipelineCatalog::ToJson 序列化规范性与过滤逻辑
TEST_F(CatalogContractSsotTest, ToJsonSerializationAndFiltering) {
  auto full_catalog = PipelineCatalog::ToJson();
  EXPECT_FALSE(full_catalog.contains("schema_version"));
  EXPECT_TRUE(full_catalog["nodes"].is_array());
  EXPECT_FALSE(full_catalog.contains("engines"));
  EXPECT_TRUE(full_catalog["models"].is_array());
  EXPECT_TRUE(full_catalog["backends"].is_array());
  EXPECT_TRUE(full_catalog["bizs"].is_array());
  EXPECT_FALSE(full_catalog.contains("businesses"));
  EXPECT_GE(full_catalog["nodes"].size(), 11U);
  EXPECT_GE(full_catalog["bizs"].size(), 7U);
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

  // 业务过滤查询
  auto km_catalog = PipelineCatalog::ToJson("keyword_match_v1");
  EXPECT_FALSE(km_catalog.contains("schema_version"));
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

// 5b. 验证 IoCatalog Schema 4 聚合与对外规范性
TEST_F(CatalogContractSsotTest, IoCatalogSchema4SerializationAndFiltering) {
  auto full_catalog = IoCatalog::ToJson();
  EXPECT_EQ(full_catalog["schema_version"], 4);
  EXPECT_TRUE(full_catalog["nodes"].is_array());
  EXPECT_TRUE(full_catalog["models"].is_array());
  EXPECT_TRUE(full_catalog["backends"].is_array());
  EXPECT_TRUE(full_catalog["bizs"].is_array());
  EXPECT_TRUE(full_catalog["input_converters"].is_array());
  EXPECT_TRUE(full_catalog["output_converters"].is_array());
  EXPECT_TRUE(full_catalog["io_bindings"].is_array());

  // 业务过滤查询
  auto km_catalog = IoCatalog::ToJson("keyword_match_v1");
  EXPECT_EQ(km_catalog["schema_version"], 4);
  EXPECT_TRUE(km_catalog["bizs"].is_array());
  EXPECT_TRUE(km_catalog["nodes"].is_array());
  EXPECT_TRUE(km_catalog["input_converters"].is_array());
  EXPECT_TRUE(km_catalog["output_converters"].is_array());
  EXPECT_TRUE(km_catalog["io_bindings"].is_array());
}

TEST_F(CatalogContractSsotTest, IoCatalogExportsKeywordSlotNamesAndTypes) {
  const auto catalog = IoCatalog::ToJson("keyword_match_v1");
  ASSERT_EQ(catalog.at("input_converters").size(), 1U);
  ASSERT_EQ(catalog.at("output_converters").size(), 1U);

  const auto& input = catalog.at("input_converters").at(0);
  EXPECT_EQ(input.at("converter_id"), "keyword.plain.operator.v1");
  ASSERT_EQ(input.at("external_slots").size(), 1U);
  const auto& input_slot = input.at("external_slots").at(0);
  EXPECT_EQ(input_slot.at("slot_name"), "keyword_in");
  EXPECT_EQ(input_slot.at("type_id"), "CompanyOperatorKeywordInput");
  EXPECT_EQ(input_slot.at("type_suffix"), "keyword_in");
  EXPECT_EQ(input_slot.at("key_suffix"), "keyword_in");
  EXPECT_EQ(input_slot.at("value_type"), "keyword_in");
  EXPECT_EQ(input_slot.at("direction"), "input");
  EXPECT_EQ(input_slot.at("required"), true);
  EXPECT_EQ(input_slot.at("capacity_fields"), nlohmann::json::array());

  const auto& output = catalog.at("output_converters").at(0);
  EXPECT_EQ(output.at("converter_id"), "keyword.result.operator.v1");
  ASSERT_EQ(output.at("external_slots").size(), 1U);
  const auto& output_slot = output.at("external_slots").at(0);
  EXPECT_EQ(output_slot.at("slot_name"), "keyword_out");
  EXPECT_EQ(output_slot.at("type_id"), "CompanyOperatorKeywordOutput");
  EXPECT_EQ(output_slot.at("type_suffix"), "keyword_out");
  EXPECT_EQ(output_slot.at("key_suffix"), "keyword_out");
  EXPECT_EQ(output_slot.at("value_type"), "CompanyOperatorKeywordOutput");
  EXPECT_EQ(output_slot.at("direction"), "output");
  EXPECT_EQ(output_slot.at("required"), true);
  EXPECT_EQ(output_slot.at("capacity_fields"),
            nlohmann::json::array({"match_result_json"}));
}

TEST_F(CatalogContractSsotTest,
       IoCatalogDistinguishesSlotTypeAndEffectiveKeySuffixes) {
  auto& registry = IoConverterRegistry::Instance();
  ASSERT_FALSE(registry.HasConflict());
  struct ScopedConverterState {
    std::vector<InputConverterDefinition> inputs;
    std::vector<OutputConverterDefinition> outputs;
    ~ScopedConverterState() {
      auto& registry = IoConverterRegistry::Instance();
      registry.ClearForTesting();
      for (const auto& input : inputs) {
        EXPECT_TRUE(registry.RegisterInputConverter(input));
      }
      for (const auto& output : outputs) {
        EXPECT_TRUE(registry.RegisterOutputConverter(output));
      }
    }
  } scoped{registry.AllInputConverters(), registry.AllOutputConverters()};

  const auto* original_input =
      registry.FindInputConverter("keyword.plain.operator.v1");
  const auto* original_output =
      registry.FindOutputConverter("keyword.result.operator.v1");
  ASSERT_NE(original_input, nullptr);
  ASSERT_NE(original_output, nullptr);
  auto input = *original_input;
  auto output = *original_output;
  input.converter_id = "catalog.slot_names.input";
  input.external_slots = {
      {"request_payload",
       "CompanyOperatorKeywordInput",
       PortDirection::kInput,
       true,
       "keyword_in",
       "keyword_in",
       {},
       "service_request"},
      {"fallback_request", "CompanyOperatorKeywordInput", PortDirection::kInput,
       false, "keyword_in", "keyword_in"}};
  output.converter_id = "catalog.slot_names.output";
  output.external_slots = {{"reply_payload",
                            "CompanyOperatorKeywordOutput",
                            PortDirection::kOutput,
                            true,
                            "CompanyOperatorKeywordOutput",
                            "keyword_out",
                            {"match_result_json"},
                            "service_reply"},
                           {"fallback_reply",
                            "CompanyOperatorKeywordOutput",
                            PortDirection::kOutput,
                            false,
                            "CompanyOperatorKeywordOutput",
                            "keyword_out",
                            {"match_result_json"}}};
  ASSERT_TRUE(registry.RegisterInputConverter(input));
  ASSERT_TRUE(registry.RegisterOutputConverter(output));

  const auto catalog = IoCatalog::ToJson();
  EXPECT_EQ(catalog.at("schema_version"), 4);
  const auto& inputs = catalog.at("input_converters");
  const auto input_it = std::find_if(
      inputs.begin(), inputs.end(), [](const nlohmann::json& converter) {
        return converter.at("converter_id") == "catalog.slot_names.input";
      });
  ASSERT_NE(input_it, inputs.end());
  const auto& input_slots = input_it->at("external_slots");
  ASSERT_EQ(input_slots.size(), 2U);
  EXPECT_EQ(input_slots.at(0).at("slot_name"), "request_payload");
  EXPECT_EQ(input_slots.at(0).at("type_id"), "CompanyOperatorKeywordInput");
  EXPECT_EQ(input_slots.at(0).at("type_suffix"), "keyword_in");
  EXPECT_EQ(input_slots.at(0).at("key_suffix"), "service_request");
  EXPECT_EQ(input_slots.at(1).at("slot_name"), "fallback_request");
  EXPECT_EQ(input_slots.at(1).at("type_suffix"), "keyword_in");
  EXPECT_EQ(input_slots.at(1).at("key_suffix"), "keyword_in");

  const auto& outputs = catalog.at("output_converters");
  const auto output_it = std::find_if(
      outputs.begin(), outputs.end(), [](const nlohmann::json& converter) {
        return converter.at("converter_id") == "catalog.slot_names.output";
      });
  ASSERT_NE(output_it, outputs.end());
  const auto& output_slots = output_it->at("external_slots");
  ASSERT_EQ(output_slots.size(), 2U);
  EXPECT_EQ(output_slots.at(0).at("slot_name"), "reply_payload");
  EXPECT_EQ(output_slots.at(0).at("type_id"), "CompanyOperatorKeywordOutput");
  EXPECT_EQ(output_slots.at(0).at("type_suffix"), "keyword_out");
  EXPECT_EQ(output_slots.at(0).at("key_suffix"), "service_reply");
  EXPECT_EQ(output_slots.at(1).at("slot_name"), "fallback_reply");
  EXPECT_EQ(output_slots.at(1).at("type_suffix"), "keyword_out");
  EXPECT_EQ(output_slots.at(1).at("key_suffix"), "keyword_out");
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
    cmd.shared_id = false;
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
