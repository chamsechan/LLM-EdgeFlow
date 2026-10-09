#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/backend_registry.h"
#include "engine/model_registry.h"
#include "engine/models/common/from_model.h"
#include "tests/support/parameter_support.h"

namespace llm_edgeflow {
namespace {

// ============================================================================
// ResolveFromModel：配置值、模型读到的值与后备值的取舍
// ============================================================================

TEST(ResolveFromModelTest, ConfiguredValueWinsWhenModelAgreesOrIsSilent) {
  int64_t value = 0;
  std::string diagnostic;
  EXPECT_TRUE(ResolveFromModel("length", 128, 128, 512, &value, &diagnostic))
      << diagnostic;
  EXPECT_EQ(value, 128);
  EXPECT_TRUE(
      ResolveFromModel("length", 64, std::nullopt, 512, &value, &diagnostic))
      << diagnostic;
  EXPECT_EQ(value, 64);
}

TEST(ResolveFromModelTest, ConfiguredValueMustMatchWhatTheModelReports) {
  int64_t value = -1;
  std::string diagnostic;
  EXPECT_FALSE(ResolveFromModel("length", 128, 256, 512, &value, &diagnostic));
  EXPECT_EQ(value, -1);
  EXPECT_NE(diagnostic.find("length"), std::string::npos);
  EXPECT_NE(diagnostic.find("128"), std::string::npos);
  EXPECT_NE(diagnostic.find("256"), std::string::npos);
}

TEST(ResolveFromModelTest, UsesModelValueWhenNotConfigured) {
  int64_t value = 0;
  std::string diagnostic;
  EXPECT_TRUE(
      ResolveFromModel("length", std::nullopt, 256, 512, &value, &diagnostic))
      << diagnostic;
  EXPECT_EQ(value, 256);
}

TEST(ResolveFromModelTest, FallsBackThenFailsWithAHintToConfigure) {
  int64_t value = 0;
  std::string diagnostic;
  EXPECT_TRUE(ResolveFromModel("length", std::nullopt, std::nullopt, 512,
                               &value, &diagnostic))
      << diagnostic;
  EXPECT_EQ(value, 512);

  value = -1;
  EXPECT_FALSE(ResolveFromModel("embedding_dim", std::nullopt, std::nullopt,
                                std::nullopt, &value, &diagnostic));
  EXPECT_EQ(value, -1);
  EXPECT_NE(diagnostic.find("embedding_dim"), std::string::npos);
  EXPECT_NE(diagnostic.find("set it in the parameters"), std::string::npos);
}

// ============================================================================
// 每个实现：{} 使用默认值、覆盖生效、非法值在参数层被拒绝
// ============================================================================

struct ParameterCase {
  const char* name;
  bool empty_config_is_valid;
  nlohmann::json expected_defaults;
  std::vector<nlohmann::json> valid_overrides;
  std::vector<nlohmann::json> invalid_overrides;
};

const std::vector<ParameterCase>& ModelCases() {
  static const std::vector<ParameterCase> cases = {
      {"bge_embedding",
       true,
       {{"tokenizer_file", "vocab.txt"},
        {"do_lower_case", true},
        {"pooling_strategy", "cls"},
        {"output_name", "last_hidden_state"},
        {"max_batch_size", 4}},
       {{{"tokenizer_file", "vocab.txt"}},
        {{"do_lower_case", false}},
        {{"max_length", 128}},
        {{"pooling_strategy", "mean"}},
        {{"output_name", "pooled"}},
        {{"embedding_dim", 768}},
        {{"max_batch_size", 8}}},
       {{{"tokenizer_file", ""}},
        {{"output_name", ""}},
        {{"max_length", 1}},
        {{"max_length", 4097}},
        {{"pooling_strategy", "max"}},
        {{"embedding_dim", 0}},
        {{"embedding_dim", 65537}},
        {{"max_batch_size", 0}},
        {{"unknown_field", true}}}},
      {"bge_reranker",
       true,
       {{"tokenizer_file", "vocab.txt"},
        {"do_lower_case", true},
        {"output_name", "logits"},
        {"score_activation", "sigmoid"},
        {"max_batch_size", 4}},
       {{{"tokenizer_file", "vocab.txt"}},
        {{"max_length", 3}},
        {{"output_name", "scores"}},
        {{"score_activation", "identity"}},
        {{"max_batch_size", 8}}},
       {{{"tokenizer_file", ""}},
        {{"output_name", ""}},
        {{"max_length", 2}},
        {{"score_activation", "tanh"}},
        {{"max_batch_size", 1025}}}},
      {"generated_text_embedding",
       false,
       {{"embedding_dim", 2},
        {"max_tokens", 1},
        {"pooling", "last"},
        {"prefix", ""},
        {"suffix", ""},
        {"add_bos", false}},
       {{{"embedding_dim", 2}},
        {{"embedding_dim", 2}, {"max_tokens", 64}},
        {{"embedding_dim", 2}, {"pooling", "mean"}},
        {{"embedding_dim", 2}, {"prefix", "p:"}, {"suffix", ":s"}},
        {{"embedding_dim", 2}, {"add_bos", true}}},
       {{{"embedding_dim", 0}},
        {{"embedding_dim", 2}, {"max_tokens", 0}},
        {{"embedding_dim", 2}, {"pooling", "cls"}}}},
      {"qwen_causal_lm",
       true,
       {{"system_prompt", ""}, {"add_bos", false}, {"random_seed", -1}},
       {{{"system_prompt", "You are concise."}},
        {{"add_bos", true}},
        {{"random_seed", 0}},
        {{"random_seed", 7}}},
       {{{"random_seed", -2}}, {{"add_bos", "yes"}}}},
      {"vision_document",
       true,
       {{"prompt",
         "Read all text visible in this image. Return only the transcribed "
         "text."},
        {"patch_size", 16},
        {"max_pixels", 4194304},
        {"max_tokens", 512}},
       {{{"prompt", "Read the text."}},
        {{"patch_size", 14}},
        {{"max_pixels", 1048576}},
        {{"max_tokens", 64}}},
       {{{"prompt", ""}},
        {{"patch_size", 0}},
        {{"patch_size", 257}},
        {{"max_pixels", 0}},
        {{"max_tokens", 4097}}}},
      {"whisper_asr",
       true,
       {{"language", "zh"},
        {"max_audio_seconds", 30},
        {"max_output_bytes", 65536}},
       {{{"language", "en"}},
        {{"language", "auto"}},
        {{"max_audio_seconds", 60}},
        {{"max_output_bytes", 1024}}},
       {{{"language", "fr"}},
        {{"max_audio_seconds", 0}},
        {{"max_audio_seconds", 61}},
        {{"max_output_bytes", 65537}}}},
  };
  return cases;
}

const std::vector<ParameterCase>& BackendCases() {
  static const std::vector<ParameterCase> cases = {
      {"onnxruntime",
       true,
       {{"max_batch_size", 4},
        {"intra_op_num_threads", 2},
        {"inter_op_num_threads", 1},
        {"graph_optimization_level", "all"}},
       {{{"max_batch_size", 8}},
        {{"intra_op_num_threads", 4}},
        {{"inter_op_num_threads", 2}},
        {{"graph_optimization_level", "basic"}}},
       {{{"max_batch_size", 0}},
        {{"intra_op_num_threads", 65}},
        {{"graph_optimization_level", "max"}},
        {{"threads", 2}}}},
      {"llama_cpp",
       true,
       {{"context_size", 2048},
        {"decode_batch_size", 512},
        {"n_threads", 0},
        {"n_threads_batch", 0},
        {"n_gpu_layers", 0},
        {"check_tensors", false}},
       {{{"context_size", 4096}},
        {{"context_size", 1024}, {"decode_batch_size", 1024}},
        {{"n_threads", 8}},
        {{"n_threads_batch", 8}},
        {{"n_gpu_layers", 32}},
        {{"check_tensors", true}}},
       {{{"context_size", 15}},
        {{"decode_batch_size", 0}},
        {{"context_size", 128}, {"decode_batch_size", 129}},
        {{"n_threads", -1}},
        {{"check_tensors", 1}}}},
      {"kite_llm",
       true,
       {{"run_config_file", ""}},
       {{{"run_config_file", "run.json"}}},
       {{{"run_config_file", 1}}, {{"unknown", true}}}},
      {"whisper_cpp",
       true,
       {{"n_threads", 4}},
       {{{"n_threads", 8}}},
       {{{"n_threads", 0}}, {{"n_threads", 65}}, {{"unknown", true}}}},
  };
  return cases;
}

void ExpectParameterBehavior(const ParameterCase& test_case,
                             const ParameterSet& params) {
  SCOPED_TRACE(test_case.name);
  std::shared_ptr<const ParameterValues> values;
  std::string diagnostic;
  EXPECT_EQ(params.Parse(nlohmann::json::object(), &values, &diagnostic),
            test_case.empty_config_is_valid)
      << diagnostic;
  if (test_case.empty_config_is_valid) {
    ASSERT_NE(values, nullptr);
    EXPECT_EQ(values->Effective(), test_case.expected_defaults);
  }
  // 必填参数（generated_text_embedding 的 embedding_dim）在 {} 下必须报错；
  // 其余用例在 {} 下都使用默认值。
  for (const auto& config : test_case.valid_overrides) {
    SCOPED_TRACE(config.dump());
    nlohmann::json full = nlohmann::json::object();
    if (!test_case.empty_config_is_valid) full["embedding_dim"] = 2;
    full.update(config);
    values.reset();
    ASSERT_TRUE(params.Parse(full, &values, &diagnostic)) << diagnostic;
    ASSERT_NE(values, nullptr);
    auto expected = test_case.expected_defaults;
    expected.update(config);
    EXPECT_EQ(values->Effective(), expected);
  }
  for (const auto& config : test_case.invalid_overrides) {
    SCOPED_TRACE(config.dump());
    nlohmann::json full = nlohmann::json::object();
    if (!test_case.empty_config_is_valid && !config.contains("embedding_dim")) {
      full["embedding_dim"] = 2;
    }
    full.update(config);
    EXPECT_FALSE(params.Parse(full, &values, &diagnostic));
    EXPECT_FALSE(diagnostic.empty());
  }
}

TEST(ModelBackendParametersTest, EveryModelParsesDefaultsAndOverrides) {
  for (const auto& test_case : ModelCases()) {
    const auto definition = ModelRegistry::Instance().Find(test_case.name);
    ASSERT_TRUE(definition.has_value()) << test_case.name;
    ExpectParameterBehavior(test_case, definition->params);
  }
}

TEST(ModelBackendParametersTest,
     EveryRegisteredBackendParsesDefaultsAndOverrides) {
  for (const auto& test_case : BackendCases()) {
    const auto definition = BackendRegistry::Instance().Find(test_case.name);
    if (!definition.has_value()) {
      RecordProperty(std::string("not_built_") + test_case.name, "true");
      continue;  // 该后端未编入本构建
    }
    ExpectParameterBehavior(test_case, definition->params);
    RecordProperty(std::string("tested_") + test_case.name, "true");
  }
}

TEST(ModelBackendParametersTest,
     BgeLengthAndDimensionAreOptionalWithoutDefaults) {
  for (const auto& [name, field_name] :
       std::vector<std::pair<std::string, std::string>>{
           {"bge_embedding", "max_length"},
           {"bge_embedding", "embedding_dim"},
           {"bge_reranker", "max_length"}}) {
    SCOPED_TRACE(name + "." + field_name);
    const auto definition = ModelRegistry::Instance().Find(name);
    ASSERT_TRUE(definition.has_value());
    const auto& fields = definition->params.Fields();
    const auto field =
        std::find_if(fields.begin(), fields.end(),
                     [&](const auto& item) { return item.name == field_name; });
    ASSERT_NE(field, fields.end());
    EXPECT_FALSE(field->required);
    EXPECT_TRUE(field->default_value.is_null());
    EXPECT_NE(field->semantic.find("不写时"), std::string::npos);
  }
}

TEST(ModelBackendParametersTest, WhisperAndKiteDescriptionsAreChinese) {
  const auto contains_cjk = [](const std::string& text) {
    // UTF-8 中文字符的首字节在 0xE4..0xE9 范围内。
    return std::any_of(text.begin(), text.end(), [](char c) {
      const auto byte = static_cast<unsigned char>(c);
      return byte >= 0xE4 && byte <= 0xE9;
    });
  };
  std::vector<ParameterSet> sets;
  if (const auto model = ModelRegistry::Instance().Find("whisper_asr")) {
    sets.push_back(model->params);
  }
  for (const char* backend_type : {"whisper_cpp", "kite_llm"}) {
    if (const auto backend = BackendRegistry::Instance().Find(backend_type)) {
      sets.push_back(backend->params);
    }
  }
  ASSERT_FALSE(sets.empty());
  for (const auto& set : sets) {
    ASSERT_FALSE(set.Fields().empty());
    for (const auto& field : set.Fields()) {
      EXPECT_TRUE(contains_cjk(field.semantic)) << field.name;
    }
  }
}

}  // namespace
}  // namespace llm_edgeflow
