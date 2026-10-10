#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adapter/converter_authoring.h"
#include "adapter/io_converter_registry.h"
#include "adapter/output/rule_match_response.h"
#include "adapter/platform_value_binding.h"
#include "core/common_contracts.h"
#include "edgeflow/operator/interface.h"
#include "tests/support/operator_test_fixture.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {
namespace {

constexpr char kConverterName[] = "test_prepared_io_parameters";
constexpr int32_t kInputService = 21001;
constexpr int32_t kOutputService = 21002;

struct InputParams {
  std::string prefix;
  std::string derived_prefix;
};

struct OutputParams {
  std::string marker;
  int64_t match_result_json_max_bytes = 0;
  std::string derived_marker;
  int64_t requested_size = 0;
};

struct InputObservation {
  const ParameterValues* values;
  const InputParams* typed;
  nlohmann::json effective;
  std::string transformed_text;
};

struct OutputObservation {
  const ParameterValues* values;
  const OutputParams* typed;
  nlohmann::json effective;
  uint32_t pool_capacity;
};

std::atomic<int> input_prepare_calls{0};
std::atomic<int> output_prepare_calls{0};
std::mutex observations_mutex;
std::map<uint64_t, InputObservation> input_observations;
std::map<uint64_t, OutputObservation> output_observations;
DecodeInputFn production_keyword_decode = nullptr;

Parameters<InputParams> InputParamSpec() {
  return Parameters<InputParams>{
      Field("prefix", &InputParams::prefix).Default("alpha")}
      .Prepare([](InputParams* params, std::string*) {
        ++input_prepare_calls;
        params->prefix += ":prepared";
        params->derived_prefix = params->prefix + ":derived";
        return true;
      });
}

Parameters<OutputParams> OutputParamSpec() {
  return Parameters<OutputParams>{
      Field("marker", &OutputParams::marker).Default("default"),
      MaxBytes("match_result_json", &OutputParams::match_result_json_max_bytes)
          .Maximum(4096)
          .Default(64)}
      .Prepare([](OutputParams* params, std::string*) {
        ++output_prepare_calls;
        params->requested_size = params->match_result_json_max_bytes;
        params->match_result_json_max_bytes += 512;
        params->marker += ":prepared";
        params->derived_marker = params->marker + ":derived";
        return true;
      });
}

int DecodeWithParameters(const ExternalInputBatchView& source,
                         const InputDecodeOptions& options, AlgContext* context,
                         AdapterStatus* status) {
  AlgContext decoded;
  const int code = production_keyword_decode(source, options, &decoded, status);
  if (code != 0) return code;
  const auto& params = options.Params<InputParams>();
  const std::string input_key = options.Port("sentence_text");
  auto sentences = *decoded.Read<TextBatch>(input_key);
  for (size_t i = 0; i < sentences.size(); ++i) {
    sentences[i].data =
        params.prefix + "|" + params.derived_prefix + "|" + sentences[i].data;
    std::lock_guard<std::mutex> lock(observations_mutex);
    input_observations.emplace(
        options.request_ids->at(i),
        InputObservation{options.params, &params, options.params->Effective(),
                         sentences[i].data});
  }
  return AdapterValidationHelper::PublishContextValue(
             *context, input_key, std::move(sentences), options.Label().c_str(),
             status)
             ? COMPANY_ALG_SUCCESS
             : COMPANY_ALG_ERR_INVALID_INPUT;
}

int EncodeWithParameters(AlgContext* context,
                         const OutputEncodeOptions& options,
                         ExternalOutputBatchView* destination,
                         size_t* written_count, AdapterStatus* status) {
  const auto& params = options.Params<OutputParams>();
  const auto capacity = destination->GetPoolSpec("keyword_out")
                            ->capacities.at("match_result_json");
  for (const auto id : *options.request_ids) {
    std::lock_guard<std::mutex> lock(observations_mutex);
    output_observations.emplace(
        id, OutputObservation{options.params, &params,
                              options.params->Effective(), capacity});
  }
  return EncodeResultRows<KeywordOutputValue>(
      context, options, destination, written_count, status, "keyword_out",
      MakeBlackboardKey<RuleMatchBatch>("matches"),
      [&params](const RuleMatchItem& result, KeywordOutputValue* output) {
        auto response =
            nlohmann::json::parse(SerializeRuleMatchResponse(result));
        response["marker"] = params.marker;
        response["derived_marker"] = params.derived_marker;
        output->is_hit = result.is_hit;
        output->status_code = result.status_code;
        output->match_result_json = response.dump();
        return AdapterStatus::Ok();
      });
}

class IoParametersTest : public test_support::OperatorTestFixture {
 protected:
  void SetUp() override {
    OperatorTestFixture::SetUp();
    if (HasFatalFailure()) return;
    auto& registry = IoConverterRegistry::Instance();
    const auto* input =
        registry.FindInputConverter("keyword_in", "keyword_match");
    const auto* output =
        registry.FindOutputConverter("keyword_out", "keyword_match");
    ASSERT_NE(input, nullptr);
    ASSERT_NE(output, nullptr);
    production_keyword_decode = input->decode_fn;
    if (!registry.FindInputConverter("keyword_in", kConverterName)) {
      auto definition = *input;
      definition.name = kConverterName;
      test_support::RegistryTestAccess::SetService(
          definition.type, definition.name, kInputService);
      definition.params = InputParamSpec();
      definition.decode_fn = &DecodeWithParameters;
      ASSERT_TRUE(registry.RegisterInputConverter(definition));
    }
    if (!registry.FindOutputConverter("keyword_out", kConverterName)) {
      auto definition = *output;
      definition.name = kConverterName;
      test_support::RegistryTestAccess::SetService(
          definition.type, definition.name, kOutputService);
      definition.params = OutputParamSpec();
      definition.encode_fn = &EncodeWithParameters;
      ASSERT_TRUE(registry.RegisterOutputConverter(definition));
    }
    input_prepare_calls = 0;
    output_prepare_calls = 0;
    input_observations.clear();
    output_observations.clear();
    directory_ =
        std::filesystem::temp_directory_path() /
        ("edgeflow-io-parameters-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(directory_));
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    OperatorTestFixture::TearDown();
  }

  std::string WriteConfig(const std::string& file, const std::string& prefix,
                          const std::string& marker, int64_t requested_size) {
    const nlohmann::json pipeline = {
        {"io",
         {{"input",
           {{{"type", "keyword_in"},
             {"name", kConverterName},
             {"params", {{"prefix", prefix}}}}}},
          {"output",
           {{{"type", "keyword_out"},
             {"name", kConverterName},
             {"inputs", {{"matches", "rules.matches"}}},
             {"params",
              {{"marker", marker},
               {"match_result_json_max_bytes", requested_size}}}}}}}},
        {"models", nlohmann::json::array()},
        {"pipeline",
         {{{"name", "rules"},
           {"type", "text_rule_match"},
           {"inputs", {{"text", "input.sentence_text"}}},
           {"params",
            {{"categories",
              {{"alpha", {"alpha:prepared|alpha:prepared:derived"}},
               {"beta", {"beta:prepared|beta:prepared:derived"}}}}}}}}}};
    std::ofstream(directory_ / (file + ".json")) << pipeline.dump();
    std::ofstream(directory_ / (file + ".conf"))
        << nlohmann::json({{"pipe_path", file + ".json"}}).dump();
    return file + ".conf";
  }

  void ProcessAndCheck(void* handle, uint64_t request_id,
                       const std::string& prefix, const std::string& marker,
                       int64_t requested_size) {
    char payload[] = "payload";
    CompanyString text{7, payload};
    CompanyOperatorKeywordInput input{request_id, kInputService, &text};
    operator_api::NamedIoBatch inputs(1), outputs(1);
    inputs[0]["test.keyword_in"] =
        operator_api::MakeBorrowedOperatorInput(&input);
    outputs[0]["test.keyword_out"] = nullptr;
    ASSERT_EQ(ops_.Process(handle, inputs, outputs), 0)
        << operator_api::GetOperatorLastError();
    const auto output = outputs[0].at("test.keyword_out");
    ASSERT_NE(output, nullptr);
    const auto* result =
        static_cast<const CompanyOperatorKeywordOutput*>(output.get());
    EXPECT_EQ(result->request_id, request_id);
    EXPECT_EQ(result->service_type, kOutputService);
    EXPECT_EQ(result->status_code, 0);
    EXPECT_EQ(result->is_hit, 1);
    ASSERT_NE(result->match_result_json, nullptr);
    ASSERT_NE(result->match_result_json->data, nullptr);
    EXPECT_GT(result->match_result_json->length, requested_size);
    EXPECT_LE(result->match_result_json->length, requested_size + 512);
    auto response = nlohmann::json::parse(
        result->match_result_json->data,
        result->match_result_json->data + result->match_result_json->length,
        nullptr, false);
    ASSERT_TRUE(response.is_object());
    EXPECT_EQ(response["intent"], prefix);
    EXPECT_EQ(response["marker"], marker + ":prepared");
    EXPECT_EQ(response["derived_marker"], marker + ":prepared:derived");
  }

  std::filesystem::path directory_;
};

TEST_F(IoParametersTest,
       PrepareRunsOnceAndRepeatedConcurrentProcessSharesValues) {
  test_support::ScopedTestOperator instance(ops_);
  ASSERT_EQ(instance.Create(WriteConfig("single", "alpha", "red", 32),
                            directory_.string()),
            0)
      << instance.create_diagnostic();
  EXPECT_EQ(input_prepare_calls.load(), 1);
  EXPECT_EQ(output_prepare_calls.load(), 1);
  ProcessAndCheck(instance.get(), 101, "alpha", "red", 32);
  ProcessAndCheck(instance.get(), 102, "alpha", "red", 32);
  std::vector<std::thread> workers;
  for (uint64_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      for (uint64_t call = 0; call < 3; ++call)
        ProcessAndCheck(instance.get(), 1000 + worker * 10 + call, "alpha",
                        "red", 32);
    });
  }
  for (auto& worker : workers) worker.join();
  EXPECT_EQ(input_prepare_calls.load(), 1);
  EXPECT_EQ(output_prepare_calls.load(), 1);
  ASSERT_EQ(input_observations.size(), 14U);
  ASSERT_EQ(output_observations.size(), 14U);
  const auto& first_input = input_observations.at(101);
  const auto& first_output = output_observations.at(101);
  for (const auto& [id, observation] : input_observations) {
    EXPECT_EQ(observation.values, first_input.values);
    EXPECT_EQ(observation.typed, first_input.typed);
    EXPECT_EQ(observation.typed->prefix, "alpha:prepared");
    EXPECT_EQ(observation.typed->derived_prefix, "alpha:prepared:derived");
    EXPECT_EQ(observation.effective,
              (nlohmann::json{{"prefix", "alpha:prepared"}}));
    EXPECT_EQ(observation.transformed_text,
              "alpha:prepared|alpha:prepared:derived|payload");
  }
  for (const auto& [id, observation] : output_observations) {
    EXPECT_EQ(observation.values, first_output.values);
    EXPECT_EQ(observation.typed, first_output.typed);
    EXPECT_EQ(observation.typed->requested_size, 32);
    EXPECT_EQ(observation.typed->match_result_json_max_bytes, 544);
    EXPECT_EQ(observation.pool_capacity, 544U);
    EXPECT_EQ(observation.effective,
              (nlohmann::json{{"marker", "red:prepared"},
                              {"match_result_json_max_bytes", 544}}));
  }
}

TEST_F(IoParametersTest,
       SameConverterPairKeepsTwoHandlesParametersIndependent) {
  test_support::ScopedTestOperator first(ops_), second(ops_);
  ASSERT_EQ(first.Create(WriteConfig("first", "alpha", "red", 32),
                         directory_.string()),
            0)
      << first.create_diagnostic();
  ASSERT_EQ(second.Create(WriteConfig("second", "beta", "blue", 64),
                          directory_.string()),
            0)
      << second.create_diagnostic();
  EXPECT_EQ(input_prepare_calls.load(), 2);
  EXPECT_EQ(output_prepare_calls.load(), 2);
  ProcessAndCheck(first.get(), 201, "alpha", "red", 32);
  ProcessAndCheck(second.get(), 202, "beta", "blue", 64);
  ProcessAndCheck(first.get(), 203, "alpha", "red", 32);
  ProcessAndCheck(second.get(), 204, "beta", "blue", 64);
  EXPECT_EQ(input_prepare_calls.load(), 2);
  EXPECT_EQ(output_prepare_calls.load(), 2);
  ASSERT_EQ(input_observations.size(), 4U);
  ASSERT_EQ(output_observations.size(), 4U);
  EXPECT_NE(input_observations.at(201).values,
            input_observations.at(202).values);
  EXPECT_NE(input_observations.at(201).typed, input_observations.at(202).typed);
  EXPECT_EQ(input_observations.at(201).values,
            input_observations.at(203).values);
  EXPECT_EQ(input_observations.at(202).values,
            input_observations.at(204).values);
  EXPECT_NE(output_observations.at(201).values,
            output_observations.at(202).values);
  EXPECT_NE(output_observations.at(201).typed,
            output_observations.at(202).typed);
  EXPECT_EQ(output_observations.at(201).values,
            output_observations.at(203).values);
  EXPECT_EQ(output_observations.at(202).values,
            output_observations.at(204).values);
  EXPECT_EQ(output_observations.at(201).pool_capacity, 544U);
  EXPECT_EQ(output_observations.at(202).pool_capacity, 576U);
  EXPECT_EQ(output_observations.at(202).typed->requested_size, 64);
  EXPECT_EQ(output_observations.at(202).effective,
            (nlohmann::json{{"marker", "blue:prepared"},
                            {"match_result_json_max_bytes", 576}}));
}

}  // namespace
}  // namespace llm_edgeflow
