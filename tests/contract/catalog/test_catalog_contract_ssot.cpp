#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "adapter/io_catalog.h"
#include "adapter/io_converter_registry.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "core/node_interface.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "edgeflow/operator/types.h"
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
    if (node_def.node_type == "text_chunk") {
      EXPECT_EQ(instance->Name(), "text_chunk");
    }

    // FindNode 查询一致性
    const auto found = PipelineCatalog::FindNode(node_def.node_type);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->node_type, node_def.node_type);
    EXPECT_EQ(found->category, node_def.category);
  }

  // 必须包含 11 个 Phase-1 Common 算子
  EXPECT_TRUE(seen_types.count("text_template"));
  EXPECT_TRUE(seen_types.count("text_chunk"));
  EXPECT_TRUE(seen_types.count("text_rule_match"));
  EXPECT_TRUE(seen_types.count("structured_json_parse"));
  EXPECT_TRUE(seen_types.count("text_embedding"));
  EXPECT_TRUE(seen_types.count("vector_top_k"));
  EXPECT_TRUE(seen_types.count("text_rerank"));
  EXPECT_TRUE(seen_types.count("llm_generate"));
  EXPECT_TRUE(seen_types.count("asr_transcribe"));
  EXPECT_TRUE(seen_types.count("ocr_detect"));
  EXPECT_TRUE(seen_types.count("text_corpus_source"));
}

TEST_F(CatalogContractSsotTest, ProductionModelBackendCatalogHasNoFixtures) {
  std::set<std::string> model_types;
  for (const auto& model : PipelineCatalog::Models()) {
    EXPECT_FALSE(model.impl_name.empty());
    EXPECT_FALSE(model.model_type.empty());
    EXPECT_TRUE(model_types.insert(model.impl_name).second);
  }
  EXPECT_TRUE(model_types.count("bge_embedding"));
  EXPECT_TRUE(model_types.count("bge_reranker"));
  EXPECT_TRUE(model_types.count("qwen_causal_lm"));
  EXPECT_TRUE(model_types.count("vision_document"));
  EXPECT_TRUE(model_types.count("whisper_asr"));
  const auto embedding = PipelineCatalog::FindModel("generated_text_embedding");
  ASSERT_TRUE(embedding.has_value());
  EXPECT_EQ(embedding->model_type, "embedding");
  EXPECT_EQ(embedding->required_protocol,
            ExecutionProtocol::kGeneratedTokenEmbedding);
  const auto vision = PipelineCatalog::FindModel("vision_document");
  ASSERT_TRUE(vision.has_value());
  EXPECT_EQ(vision->required_protocol, ExecutionProtocol::kImageTextGeneration);
  const auto asr = PipelineCatalog::FindModel("whisper_asr");
  ASSERT_TRUE(asr.has_value());
  EXPECT_EQ(asr->model_type, "asr");
  EXPECT_EQ(asr->required_protocol, ExecutionProtocol::kAudioTranscription);
  for (const auto& impl_name : model_types) {
    EXPECT_EQ(impl_name.find("test_"), std::string::npos);
    EXPECT_EQ(impl_name.find("mock"), std::string::npos);
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

// 3. 所有生产 I/O 转换器均按 (type, name) 登记。
TEST_F(CatalogContractSsotTest, AllProductionConvertersAreRegistered) {
  const auto& registry = IoConverterRegistry::Instance();
  const std::pair<const char*, const char*> inputs[] = {
      {"keyword_in", "keyword_match"}, {"entity_in", "entity_extract"},
      {"entity_in", "translate"},      {"doc_in", "doc_qa"},
      {"audit_in", "dialogue_audit"},  {"frame", "ocr_invoice_qa"},
      {"string", "ocr_invoice_qa"},    {"audio_in", "audio_asr_intent"},
      {"rerank_in", "cross_rerank"}};
  const std::pair<const char*, const char*> outputs[] = {
      {"keyword_out", "keyword_match"},  {"entity_out", "entity_extract"},
      {"entity_out", "translate"},       {"doc_out", "doc_qa"},
      {"audit_out", "dialogue_audit"},   {"od_out", "ocr_invoice_qa"},
      {"audio_out", "audio_asr_intent"}, {"rerank_out", "cross_rerank"}};
  EXPECT_EQ(registry.AllInputConverters().size(), 9U);
  EXPECT_EQ(registry.AllOutputConverters().size(), 8U);
  for (const auto& [type, name] : inputs) {
    const auto* converter = registry.FindInputConverter(type, name);
    ASSERT_NE(converter, nullptr) << type << "/" << name;
    EXPECT_EQ(converter->type, type);
    EXPECT_EQ(converter->name, name);
    EXPECT_EQ(converter->slot.type_suffix, type);
    EXPECT_NE(converter->decode_fn, nullptr);
  }
  for (const auto& [type, name] : outputs) {
    const auto* converter = registry.FindOutputConverter(type, name);
    ASSERT_NE(converter, nullptr) << type << "/" << name;
    EXPECT_EQ(converter->type, type);
    EXPECT_EQ(converter->name, name);
    EXPECT_EQ(converter->slot.type_suffix, type);
    EXPECT_NE(converter->encode_fn, nullptr);
  }
}

// 4. 不存在的实体查询返回空值。
TEST_F(CatalogContractSsotTest, FindReturnsEmptyForNonexistentEntities) {
  EXPECT_FALSE(PipelineCatalog::FindNode("non_existent_node12345").has_value());
  EXPECT_EQ(NodeRegistry::Instance().Create("non_existent_node123"), nullptr);
  EXPECT_FALSE(
      PipelineCatalog::FindModel("non_existent_model_999").has_value());
  EXPECT_FALSE(
      PipelineCatalog::FindBackend("non_existent_backend_999").has_value());
  EXPECT_EQ(IoConverterRegistry::Instance().FindInputConverter(
                "unregistered_input", "common"),
            nullptr);
  EXPECT_EQ(IoConverterRegistry::Instance().FindOutputConverter(
                "unregistered_output", "common"),
            nullptr);
}

TEST_F(CatalogContractSsotTest,
       ValueSnapshotsRemainStableDuringConcurrentRegistration) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  const auto original = PipelineCatalog::Snapshot();
  ASSERT_FALSE(original.nodes.empty());
  const std::string original_first = original.nodes.front().node_type;
  const auto original_json = PipelineCatalog::ToJson(original);
  std::atomic<bool> registration_ok{true};
  std::thread registrar([&]() {
    for (int i = 0; i < 16; ++i) {
      NodeDefinition def;
      def.node_type = "catalog_snapshot_probe_" + std::to_string(i);
      def.category = "snapshot_test";
      def.description = "catalog snapshot probe";
      if (!NodeRegistry::Instance().Register(
              def.node_type, []() -> std::unique_ptr<INode> { return nullptr; },
              def)) {
        registration_ok = false;
      }
    }
  });
  for (int i = 0; i < 32; ++i) {
    const auto current = PipelineCatalog::Snapshot();
    EXPECT_NE(current.FindNode(original_first), nullptr);
    EXPECT_GE(current.nodes.size(), original.nodes.size());
    EXPECT_EQ(PipelineCatalog::ToJson(original), original_json);
  }
  registrar.join();

  EXPECT_TRUE(registration_ok.load());
  EXPECT_EQ(original.nodes.front().node_type, original_first);
  EXPECT_EQ(original.FindNode("catalog_snapshot_probe_0"), nullptr);
  EXPECT_EQ(PipelineCatalog::ToJson(original), original_json);
  EXPECT_TRUE(
      PipelineCatalog::FindNode("catalog_snapshot_probe_0").has_value());
  EXPECT_EQ(PipelineCatalog::Snapshot().nodes.size(),
            original.nodes.size() + 16);
}

// 5. PipelineCatalog 序列化完整节点、模型和后端元数据。
TEST_F(CatalogContractSsotTest, ToJsonSerialization) {
  const auto snapshot = PipelineCatalog::Snapshot();
  const auto full_catalog = PipelineCatalog::ToJson(snapshot);
  EXPECT_EQ(full_catalog, PipelineCatalog::ToJson());
  EXPECT_TRUE(full_catalog.at("nodes").is_array());
  EXPECT_TRUE(full_catalog.at("models").is_array());
  EXPECT_TRUE(full_catalog.at("backends").is_array());
  EXPECT_GE(full_catalog.at("nodes").size(), 11U);
  bool found_match_node = false;
  for (const auto& node : full_catalog.at("nodes")) {
    ASSERT_TRUE(node.at("model_dependencies").is_array());
    const auto* definition =
        snapshot.FindNode(node.at("node_type").get<std::string>());
    ASSERT_NE(definition, nullptr);
    ASSERT_EQ(node.at("model_dependencies").size(),
              definition->model_dependencies.size());
    for (size_t i = 0; i < definition->model_dependencies.size(); ++i) {
      const auto& dependency = definition->model_dependencies[i];
      EXPECT_EQ(node.at("model_dependencies").at(i),
                (nlohmann::json{{"name", dependency.name},
                                {"model_type", dependency.model_type},
                                {"config_field", dependency.config_field}}));
    }
    for (const auto& port : node.at("inputs")) {
      EXPECT_TRUE(port.at("type_id").is_string());
    }
    for (const auto& port : node.at("outputs")) {
      EXPECT_TRUE(port.at("type_id").is_string());
    }
    found_match_node |= node.at("node_type") == "text_rule_match";
  }
  EXPECT_TRUE(found_match_node);
  for (const auto& model : full_catalog.at("models")) {
    ASSERT_TRUE(model.at("impl_name").is_string());
    ASSERT_TRUE(model.at("model_type").is_string());
    ASSERT_TRUE(model.at("backends").is_array());
    const auto definition =
        PipelineCatalog::FindModel(model.at("impl_name").get<std::string>());
    ASSERT_TRUE(definition.has_value());
    EXPECT_EQ(model.at("model_type"), definition->model_type);
    std::set<std::string> backend_types;
    for (const auto& backend : model.at("backends")) {
      ASSERT_TRUE(backend.is_string());
      const auto type = backend.get<std::string>();
      EXPECT_TRUE(backend_types.insert(type).second);
      const auto implementations = ModelRegistry::Instance().FindImplementation(
          definition->model_type, type);
      EXPECT_TRUE(std::any_of(implementations.begin(), implementations.end(),
                              [&](const auto& candidate) {
                                return candidate.impl_name ==
                                       definition->impl_name;
                              }));
    }
  }
}

// 5b. IoCatalog 聚合转换器单槽、typed 端口和参数元数据。
TEST_F(CatalogContractSsotTest, IoCatalogSerialization) {
  const auto snapshot = PipelineCatalog::Snapshot();
  const auto full_catalog = IoCatalog::ToJson(snapshot);
  EXPECT_EQ(full_catalog, IoCatalog::ToJson());
  EXPECT_TRUE(full_catalog.at("nodes").is_array());
  EXPECT_TRUE(full_catalog.at("models").is_array());
  EXPECT_TRUE(full_catalog.at("backends").is_array());
  ASSERT_TRUE(full_catalog.at("input_converters").is_array());
  ASSERT_TRUE(full_catalog.at("output_converters").is_array());
  EXPECT_EQ(full_catalog.at("input_converters").size(), 9U);
  EXPECT_EQ(full_catalog.at("output_converters").size(), 8U);
  const auto& registry = IoConverterRegistry::Instance();
  for (const char* kind : {"input_converters", "output_converters"}) {
    for (const auto& converter : full_catalog.at(kind)) {
      ASSERT_TRUE(converter.at("type").is_string());
      ASSERT_TRUE(converter.at("name").is_string());
      if (converter.contains("service_type")) {
        EXPECT_TRUE(converter.at("service_type").is_number_integer());
      }
      const auto& slot = converter.at("slot");
      ASSERT_TRUE(slot.is_object());
      EXPECT_EQ(slot.size(), 7U);
      EXPECT_TRUE(slot.at("type_id").is_string());
      EXPECT_EQ(slot.at("type_suffix"), converter.at("type"));
      EXPECT_TRUE(slot.at("required").is_boolean());
      EXPECT_TRUE(slot.at("allocator").is_string());
      EXPECT_TRUE(slot.at("allocator_params").is_string());
      EXPECT_TRUE(slot.at("metadata_count").is_number_integer());
      EXPECT_TRUE(slot.at("metadata_type_id").is_number_integer());
      ASSERT_TRUE(converter.at("logical_ports").is_array());
      EXPECT_FALSE(converter.at("logical_ports").empty());
      ASSERT_TRUE(converter.at("config_fields").is_array());
      const auto type = converter.at("type").get<std::string>();
      const auto name = converter.at("name").get<std::string>();
      const auto* binding =
          OperatorValueTypeRegistry::Instance().GetBindingBySuffix(type);
      ASSERT_NE(binding, nullptr);
      const auto service = binding->ServiceType(name);
      const std::vector<NodePortDefinition>* ports = nullptr;
      std::vector<ConfigFieldDefinition> fields;
      if (std::string(kind) == "input_converters") {
        const auto* definition = registry.FindInputConverter(type, name);
        ASSERT_NE(definition, nullptr);
        ports = &definition->logical_ports;
        fields = definition->params.Fields();
        if (service) {
          EXPECT_EQ(converter.at("service_type"), *service);
        } else {
          EXPECT_FALSE(converter.contains("service_type"));
        }
      } else {
        const auto* definition = registry.FindOutputConverter(type, name);
        ASSERT_NE(definition, nullptr);
        ports = &definition->logical_ports;
        fields = OutputConverterParameterFields(*definition);
        ASSERT_TRUE(service.has_value());
        EXPECT_EQ(converter.at("service_type"), *service);
      }
      ASSERT_EQ(converter.at("logical_ports").size(), ports->size());
      for (size_t i = 0; i < ports->size(); ++i) {
        const auto& port = (*ports)[i];
        EXPECT_EQ(converter.at("logical_ports").at(i),
                  PipelineCatalog::PortToJson(port.logical_name, port));
      }
      ASSERT_EQ(converter.at("config_fields").size(), fields.size());
      for (size_t i = 0; i < fields.size(); ++i) {
        EXPECT_EQ(converter.at("config_fields").at(i),
                  ConfigFieldToJson(fields[i]));
      }
    }
  }
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

TEST_F(CatalogContractSsotTest, IoCatalogExportsKeywordSlotNamesAndTypes) {
  const auto catalog = IoCatalog::ToJson();
  const auto find_keyword = [](const nlohmann::json& converters) {
    return std::find_if(converters.begin(), converters.end(),
                        [](const nlohmann::json& converter) {
                          return converter.at("name") == "keyword_match";
                        });
  };
  const auto& inputs = catalog.at("input_converters");
  const auto input = find_keyword(inputs);
  ASSERT_NE(input, inputs.end());
  EXPECT_EQ(input->at("type"), "keyword_in");
  EXPECT_EQ(input->at("name"), "keyword_match");
  EXPECT_EQ(input->at("service_type"), kMockServiceKeywordMatch);
  const auto& input_slot = input->at("slot");
  EXPECT_EQ(input_slot.at("type_id"), "CompanyOperatorKeywordInput");
  EXPECT_EQ(input_slot.at("type_suffix"), "keyword_in");
  EXPECT_EQ(input_slot.at("required"), true);
  EXPECT_EQ(input_slot.at("allocator"), "");
  EXPECT_EQ(input_slot.at("allocator_params"), "");
  EXPECT_EQ(input_slot.at("metadata_count"), 0);
  EXPECT_EQ(input_slot.at("metadata_type_id"), 0);
  EXPECT_EQ(input->at("config_fields"), nlohmann::json::array());

  const auto& outputs = catalog.at("output_converters");
  const auto output = find_keyword(outputs);
  ASSERT_NE(output, outputs.end());
  EXPECT_EQ(output->at("type"), "keyword_out");
  EXPECT_EQ(output->at("name"), "keyword_match");
  EXPECT_EQ(output->at("service_type"), kMockServiceKeywordMatch);
  const auto& output_slot = output->at("slot");
  EXPECT_EQ(output_slot.at("type_id"), "CompanyOperatorKeywordOutput");
  EXPECT_EQ(output_slot.at("type_suffix"), "keyword_out");
  EXPECT_EQ(output_slot.at("required"), true);
  EXPECT_EQ(output_slot.at("allocator"), "");
  EXPECT_EQ(output_slot.at("allocator_params"), "");
  EXPECT_EQ(output_slot.at("metadata_count"), 0);
  EXPECT_EQ(output_slot.at("metadata_type_id"), 0);
  ASSERT_EQ(output->at("config_fields").size(), 1U);
  const auto& capacity = output->at("config_fields").at(0);
  EXPECT_EQ(capacity.at("name"), "match_result_json_max_bytes");
  EXPECT_EQ(capacity.at("type"), "integer");
  EXPECT_EQ(capacity.at("required"), false);
  EXPECT_EQ(capacity.at("default"), 2047);
  EXPECT_EQ(capacity.at("minimum"), 1);
  EXPECT_EQ(capacity.at("maximum"), 65536);
}

TEST_F(CatalogContractSsotTest,
       IoCatalogDistinguishesConverterNamesAndParameterDefaults) {
  const auto& registry = IoConverterRegistry::Instance();
  const auto* extract_input =
      registry.FindInputConverter("entity_in", "entity_extract");
  const auto* translate_input =
      registry.FindInputConverter("entity_in", "translate");
  const auto* extract_output =
      registry.FindOutputConverter("entity_out", "entity_extract");
  const auto* translate_output =
      registry.FindOutputConverter("entity_out", "translate");
  ASSERT_NE(extract_input, nullptr);
  ASSERT_NE(translate_input, nullptr);
  ASSERT_NE(extract_output, nullptr);
  ASSERT_NE(translate_output, nullptr);
  EXPECT_NE(extract_input, translate_input);
  EXPECT_NE(extract_output, translate_output);
  EXPECT_EQ(extract_input->slot.value_type, translate_input->slot.value_type);
  EXPECT_EQ(extract_output->slot.value_type, translate_output->slot.value_type);
  EXPECT_EQ(OperatorValueTypeRegistry::Instance()
                .GetBindingBySuffix(extract_input->type)
                ->ServiceType(extract_input->name),
            kMockServiceEntityExtract);
  EXPECT_EQ(OperatorValueTypeRegistry::Instance()
                .GetBindingBySuffix(translate_input->type)
                ->ServiceType(translate_input->name),
            kMockServiceTranslate);
  EXPECT_EQ(OperatorValueTypeRegistry::Instance()
                .GetBindingBySuffix(extract_output->type)
                ->ServiceType(extract_output->name),
            kMockServiceEntityExtract);
  EXPECT_EQ(OperatorValueTypeRegistry::Instance()
                .GetBindingBySuffix(translate_output->type)
                ->ServiceType(translate_output->name),
            kMockServiceTranslate);

  std::shared_ptr<const ParameterValues> extract_values;
  std::shared_ptr<const ParameterValues> translate_values;
  std::string error;
  ASSERT_TRUE(extract_output->params.Parse(nlohmann::json::object(),
                                           &extract_values, &error))
      << error;
  ASSERT_TRUE(translate_output->params.Parse(nlohmann::json::object(),
                                             &translate_values, &error))
      << error;
  EXPECT_EQ(extract_values->Integer("entities_json_max_bytes"), 2047);
  EXPECT_EQ(translate_values->Integer("entities_json_max_bytes"), 8191);
  ASSERT_TRUE(extract_output->params.Parse({{"entities_json_max_bytes", 100}},
                                           &extract_values, &error))
      << error;
  EXPECT_EQ(extract_values->Integer("entities_json_max_bytes"), 100);
  EXPECT_EQ(translate_values->Integer("entities_json_max_bytes"), 8191);

  const auto catalog = IoCatalog::ToJson();
  const auto& outputs = catalog.at("output_converters");
  for (const auto& [name, expected_default] :
       std::vector<std::pair<std::string, int64_t>>{{"entity_extract", 2047},
                                                    {"translate", 8191}}) {
    const auto converter = std::find_if(
        outputs.begin(), outputs.end(), [&](const nlohmann::json& item) {
          return item.at("type") == "entity_out" && item.at("name") == name;
        });
    ASSERT_NE(converter, outputs.end());
    ASSERT_EQ(converter->at("config_fields").size(), 1U);
    const auto& capacity = converter->at("config_fields").at(0);
    EXPECT_EQ(capacity.at("name"), "entities_json_max_bytes");
    EXPECT_EQ(capacity.at("default"), expected_default);
    EXPECT_EQ(capacity.at("minimum"), 1);
    EXPECT_EQ(capacity.at("maximum"), 65536);
  }
}

// R6: 并发同名注册只有一个成功，另一方失败锁存
TEST_F(CatalogContractSsotTest, ConcurrentSameNameRegistrationSingleWinner) {
  test_support::RegistryTestAccess::ScopedNodeState scoped;
  const std::string race_type = "concurrent_race";
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
  const std::string node_a = "concurrent_control_node_a";
  const std::string node_b = "concurrent_control_node_b";
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
    const std::string node_name = "snapshot_writer_node_" + std::to_string(i);
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
    def.node_type = "scoped_temp";
    def.category = "temp";
    def.description = "temp";
    EXPECT_TRUE(NodeRegistry::Instance().Register(
        "scoped_temp", []() -> std::unique_ptr<INode> { return nullptr; },
        def));
    EXPECT_TRUE(NodeRegistry::Instance().Has("scoped_temp"));
    EXPECT_EQ(NodeRegistry::Instance().ListDefinitions().size(),
              original_count + 1);
  }
  EXPECT_FALSE(NodeRegistry::Instance().Has("scoped_temp"));
  EXPECT_EQ(NodeRegistry::Instance().ListDefinitions().size(), original_count);
  EXPECT_EQ(NodeRegistry::Instance().HasConflict(), original_conflict);
}

}  // namespace llm_edgeflow
