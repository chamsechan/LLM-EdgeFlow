#include "core/pipeline_catalog.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace alg_framework {
namespace {

using Kind = ConfigValueKind;

PortDefinition Port(std::string key, std::string type, bool required = true,
                    bool allow_override = false) {
  return {std::move(key), std::move(type), required, allow_override};
}

ConfigFieldDefinition Field(std::string name, Kind kind,
                            nlohmann::json default_value = nullptr,
                            std::optional<double> minimum = std::nullopt,
                            std::optional<double> maximum = std::nullopt,
                            std::string semantic = {}) {
  ConfigFieldDefinition result;
  result.name = std::move(name);
  result.kind = kind;
  result.default_value = std::move(default_value);
  result.minimum = minimum;
  result.maximum = maximum;
  result.semantic = std::move(semantic);
  return result;
}

std::vector<std::string> DocBusinesses() {
  return {"smart_doc_qa_v1", "smart_doc_qa_onnx_llamacpp_v1",
          "smart_doc_qa_rerank_llm_v1"};
}

NodeDefinition Node(std::string type, std::string category,
                    std::string description, std::vector<PortDefinition> inputs,
                    std::vector<PortDefinition> outputs,
                    std::vector<ConfigFieldDefinition> fields = {},
                    std::string capability = {},
                    std::vector<std::string> businesses = {}) {
  NodeDefinition result;
  result.node_type = std::move(type);
  result.category = std::move(category);
  result.description = std::move(description);
  result.inputs = std::move(inputs);
  result.outputs = std::move(outputs);
  result.config_fields = std::move(fields);
  result.model_capability = std::move(capability);
  result.model_config_field =
      result.model_capability.empty() ? std::string() : "bind_model";
  result.business_names = std::move(businesses);
  return result;
}

const std::vector<NodeDefinition>& BuiltinNodes() {
  static const std::vector<NodeDefinition> nodes = {
      Node("DocChunkPreNode", "preprocess", "长文档切片与请求溯源",
           {Port("raw_docs", "vector<string>"),
            Port("raw_queries", "vector<string>")},
           {Port("chunked_doc_items", "traceable<string>[]"),
            Port("query_items", "traceable<string>[]"),
            Port("chunk_counts_per_req", "vector<int>")},
           {Field("chunk_size", Kind::kInteger, 100, 1, 1048576)}, {},
           DocBusinesses()),
      Node("DocEmbeddingNode", "inference", "文档与查询向量化",
           {Port("chunked_doc_items", "traceable<string>[]"),
            Port("query_items", "traceable<string>[]")},
           {Port("chunk_embeddings", "traceable<vector<float>>[]"),
            Port("query_embeddings", "traceable<vector<float>>[]")},
           {Field("bind_model", Kind::kString, "embed_model_v1", std::nullopt,
                  std::nullopt, "model_ref")},
           "embedding", DocBusinesses()),
      Node("VectorSearchNode", "search", "向量相似度 Top-K 检索",
           {Port("chunk_embeddings", "traceable<vector<float>>[]"),
            Port("chunked_doc_items", "traceable<string>[]"),
            Port("query_embeddings", "traceable<vector<float>>[]")},
           {Port("matched_top_chunks", "traceable<string>[]")},
           {Field("top_k", Kind::kInteger, 1, 1, 1024),
            Field("min_score", Kind::kNumber, 0.0, -1.0, 1.0)},
           {}, DocBusinesses()),
      Node("RerankRefineNode", "search", "对召回片段进行二次精排",
           {Port("matched_top_chunks", "traceable<string>[]"),
            Port("raw_queries", "vector<string>")},
           {Port("matched_top_chunks", "traceable<string>[]", true, true)},
           {Field("bind_model", Kind::kString, "rerank_model_v1", std::nullopt,
                  std::nullopt, "model_ref"),
            Field("top_k", Kind::kInteger, 1, 1, 1024),
            Field("candidates_key", Kind::kString, "matched_top_chunks"),
            Field("query_key", Kind::kString, "raw_queries"),
            Field("output_key", Kind::kString, "matched_top_chunks")},
           "rerank", DocBusinesses()),
      Node("PromptBuilderNode", "preprocess", "组装 RAG Prompt",
           {Port("matched_top_chunks", "traceable<string>[]"),
            Port("raw_queries", "vector<string>")},
           {Port("llm_input_prompts", "traceable<string>[]")},
           {Field("template", Kind::kString,
                  "Context: {context}\nQuery: {query}\nAnswer:")},
           {}, DocBusinesses()),
      Node("IntentRuleNode", "rule", "基于规则识别查询意图",
           {Port("raw_queries", "vector<string>")},
           {Port("recognized_intents", "vector<string>"),
            Port("intent_confidences", "vector<float>")},
           {Field("threshold", Kind::kNumber, 0.75, 0.0, 1.0),
            Field("default_intent", Kind::kString, "GENERAL_CONSULT"),
            Field("rules", Kind::kObject, nlohmann::json::object())},
           {}, DocBusinesses()),
      Node("LlmGenerateNode", "inference", "批量 LLM 文本生成",
           {Port("llm_input_prompts", "traceable<string>[]")},
           {Port("generated_llm_answers", "traceable<string>[]")},
           {Field("bind_model", Kind::kString, "llm_model_v1", std::nullopt,
                  std::nullopt, "model_ref"),
            Field("temperature", Kind::kNumber, 0.7, 0.0, 2.0),
            Field("max_tokens", Kind::kInteger, 128, 1, 32768)},
           "llm",
           {"smart_doc_qa_v1", "smart_doc_qa_onnx_llamacpp_v1",
            "smart_doc_qa_rerank_llm_v1", "entity_extract_0.6b_v1",
            "entity_extract_llamacpp_0.6b_v1", "multimodal_ocr_invoice_qa"}),
      Node("DocQaPostNode", "postprocess", "聚合文档问答结果",
           {Port("raw_request_ids", "vector<uint64>"),
            Port("recognized_intents", "vector<string>"),
            Port("intent_confidences", "vector<float>"),
            Port("chunk_counts_per_req", "vector<int>"),
            Port("generated_llm_answers", "traceable<string>[]")},
           {Port("final_doc_outputs", "vector<DocQaResult>")}, {}, {},
           DocBusinesses()),
      Node("KeywordMatcherNode", "rule", "分类关键词匹配",
           {Port("input_sentences", "vector<string>"),
            Port("raw_request_ids", "vector<uint64>")},
           {Port("keyword_match_outputs", "vector<KeywordMatchResult>")},
           {Field("default_categories", Kind::kObject,
                  nlohmann::json::object())},
           {}, {"keyword_match_v1"}),
      Node("EntityExtractPreNode", "preprocess", "实体抽取 Prompt 构造",
           {Port("input_sentences", "vector<string>")},
           {Port("llm_input_prompts", "traceable<string>[]")},
           {Field("prompt_template", Kind::kString,
                  "请提取句子中的实体和名词：{text}")},
           {}, {"entity_extract_0.6b_v1", "entity_extract_llamacpp_0.6b_v1"}),
      Node("EntityExtractPostNode", "postprocess", "实体抽取结果封装",
           {Port("raw_request_ids", "vector<uint64>"),
            Port("generated_llm_answers", "traceable<string>[]")},
           {Port("entity_extract_outputs", "vector<EntityExtractResult>")}, {},
           {}, {"entity_extract_0.6b_v1", "entity_extract_llamacpp_0.6b_v1"}),
      Node("SafetyRulePreNode", "rule", "对话黑名单规则过滤",
           {Port("user_texts", "vector<string>")},
           {Port("hard_risk_flags", "vector<bool>"),
            Port("hit_keywords", "vector<vector<string>>")},
           {Field("blacklist", Kind::kArray, nlohmann::json::array())}, {},
           {"dialogue_compliance_audit_v1"}),
      Node("DenseRetrievalNode", "search", "召回候选风控制度",
           {Port("user_texts", "vector<string>")},
           {Port("candidate_policies", "traceable<vector<string>>[]")},
           {Field("bind_model", Kind::kString, "embed_model_v2", std::nullopt,
                  std::nullopt, "model_ref")},
           "embedding", {"dialogue_compliance_audit_v1"}),
      Node("CrossRerankNode", "inference", "风控制度 Cross-Encoder 精排",
           {Port("user_texts", "vector<string>"),
            Port("candidate_policies", "traceable<vector<string>>[]")},
           {Port("matched_policy_clauses", "vector<string>"),
            Port("rerank_scores", "vector<float>")},
           {Field("bind_model", Kind::kString, "rerank_model_v1", std::nullopt,
                  std::nullopt, "model_ref")},
           "rerank", {"dialogue_compliance_audit_v1"}),
      Node("RiskPromptNode", "preprocess", "构造风控审核 Prompt",
           {Port("user_texts", "vector<string>"),
            Port("matched_policy_clauses", "vector<string>"),
            Port("hard_risk_flags", "vector<bool>")},
           {Port("llm_audit_prompts", "traceable<string>[]")},
           {Field("template", Kind::kString, "{user_text}\n{policy}")}, {},
           {"dialogue_compliance_audit_v1"}),
      Node("LlmAuditNode", "inference", "LLM 合规审核",
           {Port("llm_audit_prompts", "traceable<string>[]")},
           {Port("generated_verdicts", "traceable<string>[]")},
           {Field("bind_model", Kind::kString, "audit_llm_v1", std::nullopt,
                  std::nullopt, "model_ref"),
            Field("temperature", Kind::kNumber, 0.1, 0.0, 2.0),
            Field("max_tokens", Kind::kInteger, 256, 1, 32768)},
           "llm", {"dialogue_compliance_audit_v1"}),
      Node("AuditPostNode", "postprocess", "聚合合规审核结果",
           {Port("raw_request_ids", "vector<uint64>"),
            Port("matched_policy_clauses", "vector<string>"),
            Port("rerank_scores", "vector<float>", false),
            Port("generated_verdicts", "traceable<string>[]")},
           {Port("compliance_audit_outputs", "vector<DialogueAuditResult>")},
           {}, {}, {"dialogue_compliance_audit_v1"}),
      Node("ImagePreNode", "preprocess", "图像请求预处理与溯源",
           {Port("raw_image_paths", "vector<string>"),
            Port("raw_queries", "vector<string>"),
            Port("raw_request_ids", "vector<uint64>")},
           {Port("traceable_image_items", "traceable<string>[]")}, {}, {},
           {"multimodal_ocr_invoice_qa"}),
      Node("OcrInferNode", "inference", "OCR 检测识别与 Prompt 组装",
           {Port("traceable_image_items", "traceable<string>[]"),
            Port("raw_queries", "vector<string>"),
            Port("raw_request_ids", "vector<uint64>")},
           {Port("ocr_box_counts", "vector<int>"),
            Port("llm_input_prompts", "traceable<string>[]")},
           {Field("bind_model", Kind::kString, "ocr_model_v1", std::nullopt,
                  std::nullopt, "model_ref")},
           "ocr", {"multimodal_ocr_invoice_qa"}),
      Node("OcrDocPostNode", "postprocess", "OCR 文档结果封装",
           {Port("raw_request_ids", "vector<uint64>"),
            Port("ocr_box_counts", "vector<int>"),
            Port("generated_llm_answers", "traceable<string>[]")},
           {Port("ocr_doc_final_outputs", "vector<OcrDocResult>")}, {}, {},
           {"multimodal_ocr_invoice_qa"}),
      Node("AudioFeaturePreNode", "preprocess", "音频输入溯源与特征准备",
           {Port("raw_audio_inputs", "vector<AudioInputDto>"),
            Port("raw_request_ids", "vector<uint64>")},
           {Port("traceable_audio_items", "traceable<AudioPcmData>[]")}, {}, {},
           {"speech_audio_asr_intent_slot"}),
      Node("AsrInferNode", "inference", "ASR 批量转写",
           {Port("traceable_audio_items", "traceable<AudioPcmData>[]")},
           {Port("asr_transcripts", "traceable<string>[]")},
           {Field("bind_model", Kind::kString, "asr_model_v1", std::nullopt,
                  std::nullopt, "model_ref")},
           "asr", {"speech_audio_asr_intent_slot"}),
      Node("SlotExtractNode", "rule", "语音意图与槽位提取",
           {Port("asr_transcripts", "traceable<string>[]")},
           {Port("intent_slot_results", "vector<string>")}, {}, {},
           {"speech_audio_asr_intent_slot"}),
      Node("AudioPostNode", "postprocess", "语音结果封装",
           {Port("raw_request_ids", "vector<uint64>"),
            Port("asr_transcripts", "traceable<string>[]"),
            Port("intent_slot_results", "vector<string>")},
           {Port("audio_final_outputs", "vector<AudioAsrResult>")}, {}, {},
           {"speech_audio_asr_intent_slot"}),
      Node("RerankPairBuilderNode", "preprocess", "构建 Query-Passage 对",
           {Port("raw_rerank_inputs", "vector<RerankQueryInput>")},
           {Port("rerank_pair_items", "traceable<PairInput>[]"),
            Port("rerank_counts_per_req", "vector<int>")},
           {}, {}, {"dense_cross_rerank_scoring"}),
      Node("CrossRerankBatchNode", "inference", "批量语义精排打分",
           {Port("rerank_pair_items", "traceable<PairInput>[]")},
           {Port("rerank_scored_items", "traceable<float>[]")},
           {Field("bind_model", Kind::kString, "rerank_model_v1", std::nullopt,
                  std::nullopt, "model_ref")},
           "rerank", {"dense_cross_rerank_scoring"}),
      Node("RerankSortPostNode", "postprocess", "精排结果排序与封装",
           {Port("raw_rerank_inputs", "vector<RerankQueryInput>"),
            Port("rerank_scored_items", "traceable<float>[]")},
           {Port("rerank_batch_final_outputs", "vector<RerankQueryResult>")},
           {}, {}, {"dense_cross_rerank_scoring"}),
  };
  return nodes;
}

const std::vector<EngineDefinition>& BuiltinEngines() {
  static const std::vector<EngineDefinition> engines = {
      {"mock_npu_embedding",
       "embedding",
       "Mock NPU embedding engine",
       {Field("max_batch_size", Kind::kInteger, 4, 1, 4096),
        Field("embedding_dim", Kind::kInteger, 128, 1, 65536),
        Field("device_id", Kind::kInteger, -1, -1, 1024)}},
      {"onnx_embedding",
       "embedding",
       "ONNX Runtime embedding engine",
       {Field("max_batch_size", Kind::kInteger, 4, 1, 4096),
        Field("embedding_dim", Kind::kInteger, 128, 1, 65536),
        Field("device_id", Kind::kInteger, -1, -1, 1024)}},
      {"mock_npu_llm",
       "llm",
       "Mock NPU LLM engine",
       {Field("max_batch_size", Kind::kInteger, 2, 1, 4096),
        Field("max_seq_len", Kind::kInteger, 1024, 1, 1048576),
        Field("device_id", Kind::kInteger, -1, -1, 1024)}},
      {"llama_cpp",
       "llm",
       "llama.cpp LLM engine",
       {Field("max_batch_size", Kind::kInteger, 2, 1, 4096),
        Field("max_seq_len", Kind::kInteger, 1024, 1, 1048576),
        Field("device_id", Kind::kInteger, -1, -1, 1024)}},
      {"mock_npu_rerank",
       "rerank",
       "Mock NPU rerank engine",
       {Field("max_batch_size", Kind::kInteger, 4, 1, 4096),
        Field("device_id", Kind::kInteger, -1, -1, 1024)}},
      {"onnx_rerank",
       "rerank",
       "ONNX Runtime rerank engine",
       {Field("max_batch_size", Kind::kInteger, 4, 1, 4096),
        Field("device_id", Kind::kInteger, -1, -1, 1024)}},
      {"mock_npu_ocr",
       "ocr",
       "Mock NPU OCR engine",
       {Field("max_batch_size", Kind::kInteger, 2, 1, 4096),
        Field("device_id", Kind::kInteger, -1, -1, 1024)}},
      {"mock_npu_asr",
       "asr",
       "Mock NPU ASR engine",
       {Field("max_batch_size", Kind::kInteger, 2, 1, 4096),
        Field("device_id", Kind::kInteger, -1, -1, 1024)}},
  };
  return engines;
}

const std::vector<BusinessDefinition>& BuiltinBusinesses() {
  static const std::vector<BusinessDefinition> businesses = {
      {"keyword_match_v1",
       "keyword_match",
       "关注词匹配",
       {Port("raw_request_ids", "vector<uint64>"),
        Port("input_sentences", "vector<string>")},
       {Port("keyword_match_outputs", "vector<KeywordMatchResult>")}},
      {"entity_extract_0.6b_v1",
       "entity_extract",
       "实体抽取",
       {Port("raw_request_ids", "vector<uint64>"),
        Port("input_sentences", "vector<string>")},
       {Port("entity_extract_outputs", "vector<EntityExtractResult>")}},
      {"entity_extract_llamacpp_0.6b_v1",
       "entity_extract",
       "实体抽取（llama.cpp）",
       {Port("raw_request_ids", "vector<uint64>"),
        Port("input_sentences", "vector<string>")},
       {Port("entity_extract_outputs", "vector<EntityExtractResult>")}},
      {"smart_doc_qa_v1",
       "doc_qa",
       "智能文档问答",
       {Port("raw_request_ids", "vector<uint64>"),
        Port("raw_docs", "vector<string>"),
        Port("raw_queries", "vector<string>")},
       {Port("final_doc_outputs", "vector<DocQaResult>")}},
      {"smart_doc_qa_onnx_llamacpp_v1",
       "doc_qa",
       "智能文档问答（ONNX/llama.cpp）",
       {Port("raw_request_ids", "vector<uint64>"),
        Port("raw_docs", "vector<string>"),
        Port("raw_queries", "vector<string>")},
       {Port("final_doc_outputs", "vector<DocQaResult>")}},
      {"smart_doc_qa_rerank_llm_v1",
       "doc_qa",
       "智能文档问答（精排）",
       {Port("raw_request_ids", "vector<uint64>"),
        Port("raw_docs", "vector<string>"),
        Port("raw_queries", "vector<string>")},
       {Port("final_doc_outputs", "vector<DocQaResult>")}},
      {"dialogue_compliance_audit_v1",
       "dialogue_audit",
       "对话合规审核",
       {Port("raw_request_ids", "vector<uint64>"),
        Port("user_texts", "vector<string>"),
        Port("channel_names", "vector<string>")},
       {Port("compliance_audit_outputs", "vector<DialogueAuditResult>")}},
      {"multimodal_ocr_invoice_qa",
       "ocr_doc_qa",
       "OCR 票据问答",
       {Port("raw_request_ids", "vector<uint64>"),
        Port("raw_image_paths", "vector<string>"),
        Port("raw_queries", "vector<string>")},
       {Port("ocr_doc_final_outputs", "vector<OcrDocResult>")}},
      {"speech_audio_asr_intent_slot",
       "audio_asr",
       "语音识别与意图槽位",
       {Port("raw_request_ids", "vector<uint64>"),
        Port("raw_audio_inputs", "vector<AudioInputDto>")},
       {Port("audio_final_outputs", "vector<AudioAsrResult>")}},
      {"dense_cross_rerank_scoring",
       "cross_rerank",
       "Cross-Encoder 精排",
       {Port("raw_rerank_inputs", "vector<RerankQueryInput>")},
       {Port("rerank_batch_final_outputs", "vector<RerankQueryResult>")}},
  };
  return businesses;
}

std::vector<NodeDefinition>& RegisteredNodes() {
  static std::vector<NodeDefinition> definitions;
  return definitions;
}

std::vector<EngineDefinition>& RegisteredEngines() {
  static std::vector<EngineDefinition> definitions;
  return definitions;
}

std::mutex& CatalogMutex() {
  static std::mutex mutex;
  return mutex;
}

nlohmann::json PortJson(const PortDefinition& port) {
  return {{"key", port.key},
          {"type_id", port.type_id},
          {"required", port.required},
          {"allow_override", port.allow_override}};
}

nlohmann::json FieldJson(const ConfigFieldDefinition& field) {
  nlohmann::json result = {{"name", field.name},
                           {"type", ConfigValueKindName(field.kind)},
                           {"required", field.required}};
  if (!field.default_value.is_null()) result["default"] = field.default_value;
  if (field.minimum) result["minimum"] = *field.minimum;
  if (field.maximum) result["maximum"] = *field.maximum;
  if (!field.enum_values.empty()) result["enum"] = field.enum_values;
  if (!field.semantic.empty()) result["semantic"] = field.semantic;
  return result;
}

}  // namespace

const char* ConfigValueKindName(ConfigValueKind kind) {
  switch (kind) {
    case ConfigValueKind::kString:
      return "string";
    case ConfigValueKind::kInteger:
      return "integer";
    case ConfigValueKind::kNumber:
      return "number";
    case ConfigValueKind::kBoolean:
      return "boolean";
    case ConfigValueKind::kObject:
      return "object";
    case ConfigValueKind::kArray:
      return "array";
  }
  return "unknown";
}

bool PipelineCatalog::RegisterNodeDefinition(const NodeDefinition& definition) {
  if (definition.node_type.empty()) return false;
  std::lock_guard<std::mutex> lock(CatalogMutex());
  auto& definitions = RegisteredNodes();
  if (std::any_of(definitions.begin(), definitions.end(),
                  [&](const auto& item) {
                    return item.node_type == definition.node_type;
                  })) {
    return false;
  }
  definitions.push_back(definition);
  std::sort(definitions.begin(), definitions.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.node_type < rhs.node_type;
            });
  return true;
}

bool PipelineCatalog::RegisterEngineDefinition(
    const EngineDefinition& definition) {
  if (definition.engine_type.empty()) return false;
  std::lock_guard<std::mutex> lock(CatalogMutex());
  auto& definitions = RegisteredEngines();
  if (std::any_of(definitions.begin(), definitions.end(),
                  [&](const auto& item) {
                    return item.engine_type == definition.engine_type;
                  })) {
    return false;
  }
  definitions.push_back(definition);
  std::sort(definitions.begin(), definitions.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.engine_type < rhs.engine_type;
            });
  return true;
}

const std::vector<NodeDefinition>& PipelineCatalog::Nodes() {
  return RegisteredNodes();
}

const std::vector<EngineDefinition>& PipelineCatalog::Engines() {
  return RegisteredEngines();
}

const std::vector<BusinessDefinition>& PipelineCatalog::Businesses() {
  return BuiltinBusinesses();
}

const NodeDefinition* PipelineCatalog::FindNode(const std::string& node_type) {
  const auto& nodes = Nodes();
  auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto& item) {
    return item.node_type == node_type;
  });
  return it == nodes.end() ? nullptr : &*it;
}

const EngineDefinition* PipelineCatalog::FindEngine(
    const std::string& engine_type) {
  const auto& engines = Engines();
  auto it = std::find_if(engines.begin(), engines.end(), [&](const auto& item) {
    return item.engine_type == engine_type;
  });
  return it == engines.end() ? nullptr : &*it;
}

const BusinessDefinition* PipelineCatalog::FindBusiness(
    const std::string& business_name) {
  const auto& businesses = Businesses();
  auto it = std::find_if(
      businesses.begin(), businesses.end(),
      [&](const auto& item) { return item.business_name == business_name; });
  return it == businesses.end() ? nullptr : &*it;
}

const NodeDefinition* PipelineCatalog::FindBuiltinNode(
    const std::string& node_type) {
  const auto& definitions = BuiltinNodes();
  auto it = std::find_if(
      definitions.begin(), definitions.end(),
      [&](const auto& item) { return item.node_type == node_type; });
  return it == definitions.end() ? nullptr : &*it;
}

const EngineDefinition* PipelineCatalog::FindBuiltinEngine(
    const std::string& engine_type) {
  const auto& definitions = BuiltinEngines();
  auto it = std::find_if(
      definitions.begin(), definitions.end(),
      [&](const auto& item) { return item.engine_type == engine_type; });
  return it == definitions.end() ? nullptr : &*it;
}

nlohmann::json PipelineCatalog::NodeToJson(const NodeDefinition& definition) {
  nlohmann::json inputs = nlohmann::json::array();
  nlohmann::json outputs = nlohmann::json::array();
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& item : definition.inputs) inputs.push_back(PortJson(item));
  for (const auto& item : definition.outputs) outputs.push_back(PortJson(item));
  for (const auto& item : definition.config_fields)
    fields.push_back(FieldJson(item));
  return {{"node_type", definition.node_type},
          {"category", definition.category},
          {"description", definition.description},
          {"inputs", std::move(inputs)},
          {"outputs", std::move(outputs)},
          {"config_fields", std::move(fields)},
          {"model_capability", definition.model_capability},
          {"model_config_field", definition.model_config_field},
          {"parallel_safe", definition.parallel_safe},
          {"business_names", definition.business_names}};
}

nlohmann::json PipelineCatalog::ToJson(const std::string& business_filter) {
  nlohmann::json nodes = nlohmann::json::array();
  for (const auto& item : Nodes()) {
    if (!business_filter.empty() &&
        std::find(item.business_names.begin(), item.business_names.end(),
                  business_filter) == item.business_names.end()) {
      continue;
    }
    nodes.push_back(NodeToJson(item));
  }

  nlohmann::json engines = nlohmann::json::array();
  for (const auto& item : Engines()) {
    nlohmann::json fields = nlohmann::json::array();
    for (const auto& field : item.config_fields)
      fields.push_back(FieldJson(field));
    engines.push_back({{"engine_type", item.engine_type},
                       {"capability", item.capability},
                       {"description", item.description},
                       {"config_fields", std::move(fields)}});
  }

  nlohmann::json businesses = nlohmann::json::array();
  for (const auto& item : Businesses()) {
    if (!business_filter.empty() && item.business_name != business_filter)
      continue;
    nlohmann::json ingress = nlohmann::json::array();
    nlohmann::json egress = nlohmann::json::array();
    for (const auto& port : item.ingress) ingress.push_back(PortJson(port));
    for (const auto& port : item.egress) egress.push_back(PortJson(port));
    businesses.push_back({{"business_name", item.business_name},
                          {"demo_business", item.demo_business},
                          {"display_name", item.display_name},
                          {"ingress", std::move(ingress)},
                          {"egress", std::move(egress)}});
  }

  return {{"schema_version", 1},
          {"nodes", std::move(nodes)},
          {"engines", std::move(engines)},
          {"businesses", std::move(businesses)}};
}

}  // namespace alg_framework
