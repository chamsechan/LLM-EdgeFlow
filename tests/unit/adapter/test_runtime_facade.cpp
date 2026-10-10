#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "adapter/converter_authoring.h"
#include "adapter/io_plan_resolver.h"
#include "adapter/io_values.h"
#include "adapter/shared_algorithm_runtime.h"
#include "contracts/inference_payloads.h"
#include "core/pipeline.h"

namespace llm_edgeflow {
namespace {

// 这些测试载体刻意不依赖平台载体头文件或 Operator ABI，也不提供请求 ID
// 和业务类型成员；仅由对应 binding 解释布局。
struct FacadeHostInput {
  std::string text;
};
struct FacadeHostOutput {
  std::string text;
};

constexpr auto kSource = MakeBlackboardKey<TextBatch>("sentence_text");
constexpr auto kResult = MakeBlackboardKey<TextBatch>("text");

struct FacadeOutputParams {
  int64_t text_max_bytes = 0;
};

int DecodeFacadeInput(const ExternalInputBatchView& source,
                      const InputDecodeOptions& options, AlgContext* context,
                      AdapterStatus* status) {
  return DecodeRequestRows<TextInputValue>(
      source, options, context, status, "facade_text", kSource,
      [](const TextInputValue& value, std::string* text) {
        *text = value.sentence_text;
        return AdapterStatus::Ok();
      });
}

int EncodeFacadeOutput(AlgContext* context, const OutputEncodeOptions& options,
                       ExternalOutputBatchView* destination, size_t* written,
                       AdapterStatus* status) {
  return EncodeResultRows<TextInputValue>(
      context, options, destination, written, status, "facade_result", kResult,
      [](const std::string& text, TextInputValue* value) {
        value->sentence_text = text;
        return AdapterStatus::Ok();
      });
}

bool RegisterFacadeBindings() {
  OperatorValueTypeBinding input;
  input.canonical_suffix = "facade_text";
  input.external_c_type_name = "FacadeHostInput";
  input.direction = IoDirection::kInput;
  input.validate_external = [](const void* raw, const InputLimits& limits,
                               std::string* error) {
    if (!raw || static_cast<const FacadeHostInput*>(raw)->text.size() >
                    limits.max_text_bytes) {
      if (error) *error = "Invalid facade text input";
      return -3;
    }
    return 0;
  };
  SetInputValue<FacadeHostInput, TextInputValue>(
      &input,
      [](const FacadeHostInput& host) { return TextInputValue{host.text}; });

  OperatorValueTypeBinding output;
  output.canonical_suffix = "facade_result";
  output.external_c_type_name = "FacadeHostOutput";
  output.direction = IoDirection::kOutput;
  output.output_layout.string_capacity_fields["text"] = {1024};
  output.output_layout.compute_block_payload_bytes =
      [](const ResolvedOutputPoolSpec& spec, size_t* bytes, std::string*) {
        *bytes = sizeof(FacadeHostOutput) + spec.GetCapacity("text") + 1;
        return true;
      };
  output.allocate_external = [](const ResolvedOutputPoolSpec&,
                                OwnedExternalBlock* block, std::string*) {
    block->raw_struct = block->Own(std::make_unique<FacadeHostOutput>());
    return 0;
  };
  output.reset_external = [](void* raw, const ResolvedOutputPoolSpec&) {
    static_cast<FacadeHostOutput*>(raw)->text.clear();
  };
  output.destroy_external = [](OwnedExternalBlock* block) { block->Destroy(); };
  SetOutputValue<FacadeHostOutput, TextInputValue>(
      &output, [](FacadeHostOutput& host, const TextInputValue& value,
                  const ResolvedOutputPoolSpec& spec) {
        if (value.sentence_text.size() > spec.GetCapacity("text"))
          return AdapterStatus::BufferTooSmall("Facade text capacity exceeded",
                                               "text");
        host.text = value.sentence_text;
        return AdapterStatus::Ok();
      });
  return RegisterOperatorValueType(input) && RegisterOperatorValueType(output);
}

InputConverterDefinition FacadeInputDefinition() {
  InputConverterDefinition def;
  def.type = "facade_text";
  def.name = "copy";
  def.slot = ExternalInputSlot<TextInputValue>(def.type);
  def.logical_ports = {OutputPort(kSource)};
  def.decode_fn = DecodeFacadeInput;
  return def;
}

OutputConverterDefinition FacadeOutputDefinition(bool required) {
  OutputConverterDefinition def;
  def.type = "facade_result";
  def.name = required ? "required" : "optional";
  def.slot = ExternalOutputSlot<TextInputValue>(def.type);
  def.slot.required = required;
  def.logical_ports = {RequiredInputPort(kResult)};
  def.params = Parameters<FacadeOutputParams>{
      MaxBytes("text", &FacadeOutputParams::text_max_bytes).Default(8)};
  def.encode_fn = EncodeFacadeOutput;
  return def;
}

const bool kFacadeBindingsRegistered = RegisterFacadeBindings();
REGISTER_INPUT_CONVERTER(FacadeInputDefinition());
REGISTER_OUTPUT_CONVERTER(FacadeOutputDefinition(true));
REGISTER_OUTPUT_CONVERTER(FacadeOutputDefinition(false));

nlohmann::json FacadeDocument() {
  return {{"io",
           {{"input", {{{"type", "facade_text"}, {"name", "copy"}}}},
            {"output",
             {{{"type", "facade_result"},
               {"name", "required"},
               {"inputs", {{"text", "copy.text"}}}},
              {{"type", "facade_result"},
               {"name", "optional"},
               {"inputs", {{"text", "copy.text"}}}}}}}},
          {"models", nlohmann::json::array()},
          {"pipeline",
           {{{"name", "copy"},
             {"type", "text_template"},
             {"inputs", {{"primary", "input.sentence_text"}}},
             {"params", {{"template", "{{primary}}"}}}}}}};
}

RuntimeInputBatch Inputs(std::vector<FacadeHostInput>* hosts) {
  RuntimeInputBatch batch;
  batch.count = hosts->size();
  ExternalInputBatchView view;
  view.count = batch.count;
  view.binding =
      OperatorValueTypeRegistry::Instance().GetBindingBySuffix("facade_text");
  view.slot_types["facade_text"] = "FacadeHostInput";
  for (auto& host : *hosts)
    view.slots["facade_text"].emplace_back(&host, [](void*) {});
  batch.views.push_back(std::move(view));
  return batch;
}

RuntimeOutputBatch OutputSelection(bool optional_first, bool optional_second) {
  RuntimeOutputBatch batch;
  batch.rows = {{{0, nullptr}}, {{0, nullptr}}};
  if (optional_first) batch.rows[0].push_back({1, nullptr});
  if (optional_second) batch.rows[1].push_back({1, nullptr});
  return batch;
}

const std::string& OutputText(const RuntimeOutputSlot& slot) {
  return static_cast<const FacadeHostOutput*>(slot.value.get())->text;
}

class RuntimeFacadeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(kFacadeBindingsRegistered);
    std::string error;
    ASSERT_EQ(SharedAlgorithmRuntime::GlobalInit(&error), 0) << error;
    std::unique_ptr<ValidatedIoPlan> plan;
    ASSERT_EQ(IoPlanResolver::ResolveFromPipelineJson(
                  FacadeDocument(), "", &plan, &error, nullptr, 2),
              0)
        << error;
    RuntimeCreateOptions options;
    options.output_pool_depth = 2;
    const auto created = SharedAlgorithmRuntime::CreateFromIoPlan(
        std::move(plan), options, &runtime_, &error);
    ASSERT_EQ(created, 0) << error;
    ASSERT_NE(runtime_, nullptr);
  }

  std::unique_ptr<SharedAlgorithmRuntime> runtime_;
};

TEST_F(RuntimeFacadeTest, DirectProcessingPairsRowsAndPreservesOptionalHoles) {
  std::string error;
  std::vector<FacadeHostInput> hosts = {{"red"}, {"blue"}};
  auto output = OutputSelection(false, true);
  const auto status = runtime_->Process(Inputs(&hosts), &output, &error);
  ASSERT_EQ(status, 0) << error;
  ASSERT_EQ(output.rows.size(), 2u);
  ASSERT_EQ(output.rows[0].size(), 1u);
  ASSERT_EQ(output.rows[1].size(), 2u);
  ASSERT_NE(output.rows[0][0].value, nullptr);
  ASSERT_NE(output.rows[1][0].value, nullptr);
  ASSERT_NE(output.rows[1][1].value, nullptr);
  EXPECT_EQ(OutputText(output.rows[0][0]), "red");
  EXPECT_EQ(OutputText(output.rows[1][0]), "blue");
  EXPECT_EQ(OutputText(output.rows[1][1]), "blue");
  output.rows.clear();
  EXPECT_EQ(runtime_->Close(&error), 0) << error;
}

TEST_F(RuntimeFacadeTest, EncodeFailurePublishesNothingAndReturnsAllLeases) {
  std::string error;
  std::vector<FacadeHostInput> hosts = {{"short"}, {"exceeds capacity"}};
  auto output = OutputSelection(true, true);
  const auto failed = runtime_->Process(Inputs(&hosts), &output, &error);
  EXPECT_EQ(failed, -4);
  EXPECT_FALSE(error.empty());
  ASSERT_EQ(output.rows.size(), 2u);
  for (const auto& row : output.rows) {
    ASSERT_EQ(row.size(), 2u);
    EXPECT_EQ(row[0].output_index, 0u);
    EXPECT_EQ(row[1].output_index, 1u);
    EXPECT_EQ(row[0].value, nullptr);
    EXPECT_EQ(row[1].value, nullptr);
  }

  hosts = {{"one"}, {"two"}};
  const auto retried = runtime_->Process(Inputs(&hosts), &output, &error);
  ASSERT_EQ(retried, 0) << error;
  std::vector<void*> addresses;
  for (size_t row = 0; row < output.rows.size(); ++row) {
    for (const auto& slot : output.rows[row]) {
      ASSERT_NE(slot.value, nullptr);
      EXPECT_EQ(OutputText(slot), hosts[row].text);
      addresses.push_back(slot.value.get());
    }
  }
  output.rows.clear();
  output = OutputSelection(true, true);
  hosts = {{"three"}, {"four"}};
  const auto reused = runtime_->Process(Inputs(&hosts), &output, &error);
  ASSERT_EQ(reused, 0) << error;
  std::vector<void*> reused_addresses;
  for (size_t row = 0; row < output.rows.size(); ++row) {
    for (const auto& slot : output.rows[row]) {
      EXPECT_EQ(OutputText(slot), hosts[row].text);
      reused_addresses.push_back(slot.value.get());
    }
  }
  // 比较各池归还的两个内存块，不要求它们按特定先进先出顺序返回。
  for (size_t index = 0; index < 2; ++index) {
    EXPECT_TRUE(addresses[index] == reused_addresses[index] ||
                addresses[index] == reused_addresses[index + 2]);
    EXPECT_TRUE(addresses[index + 2] == reused_addresses[index] ||
                addresses[index + 2] == reused_addresses[index + 2]);
  }
  output.rows.clear();
  EXPECT_EQ(runtime_->Close(&error), 0) << error;
}

TEST_F(RuntimeFacadeTest, InvalidOutputSelectionsLeaveBatchAndPoolsUntouched) {
  std::string error;
  std::vector<FacadeHostInput> hosts = {{"one"}, {"two"}};
  auto occupied = std::make_shared<FacadeHostOutput>();
  occupied->text = "existing";
  std::vector<RuntimeOutputBatch> invalid_selections(
      4, OutputSelection(false, false));
  invalid_selections[0].rows[0].push_back({0, nullptr});
  invalid_selections[1].rows[0][0].output_index = 2;
  invalid_selections[2].rows[0][0].output_index = 1;
  invalid_selections[3].rows[0][0].value = occupied;
  for (size_t index = 0; index < invalid_selections.size(); ++index) {
    SCOPED_TRACE(index);
    auto output = invalid_selections[index];
    const auto before = output;
    const auto status = runtime_->Process(Inputs(&hosts), &output, &error);
    EXPECT_EQ(status, -4);
    ASSERT_EQ(output.rows.size(), before.rows.size());
    for (size_t row = 0; row < output.rows.size(); ++row) {
      ASSERT_EQ(output.rows[row].size(), before.rows[row].size());
      for (size_t slot = 0; slot < output.rows[row].size(); ++slot) {
        EXPECT_EQ(output.rows[row][slot].output_index,
                  before.rows[row][slot].output_index);
        EXPECT_EQ(output.rows[row][slot].value, before.rows[row][slot].value);
      }
    }
    EXPECT_EQ(occupied->text, "existing");
    // 每个池的深度均为二。合法的两行请求应能成功，证明此前的预校验
    // 未占用任何内存块，包括传入非空输出值的情况。
    auto valid = OutputSelection(true, true);
    const auto retried = runtime_->Process(Inputs(&hosts), &valid, &error);
    ASSERT_EQ(retried, 0) << error;
    for (size_t row = 0; row < valid.rows.size(); ++row)
      for (const auto& slot : valid.rows[row])
        EXPECT_EQ(OutputText(slot), hosts[row].text);
  }
}

TEST_F(RuntimeFacadeTest, InputReadersComeFromValidatedPlanSnapshot) {
  std::string error;
  std::vector<FacadeHostInput> hosts = {{"one"}, {"two"}};
  auto input = Inputs(&hosts);
  auto replacement = *input.views[0].binding;
  int validator_calls = 0;
  int reader_calls = 0;
  replacement.validate_external = [&](const void*, const InputLimits&,
                                      std::string*) -> int {
    ++validator_calls;
    throw std::runtime_error("Caller must not replace plan validator");
  };
  replacement.read_value = [&](const void*) -> std::any {
    ++reader_calls;
    return TextInputValue{"tampered"};
  };
  input.views[0].binding = &replacement;
  auto output = OutputSelection(false, false);
  const auto status = runtime_->Process(input, &output, &error);
  ASSERT_EQ(status, 0) << error;
  EXPECT_EQ(validator_calls, 0);
  EXPECT_EQ(reader_calls, 0);
  EXPECT_EQ(OutputText(output.rows[0][0]), "one");
  EXPECT_EQ(OutputText(output.rows[1][0]), "two");
  output.rows.clear();

  replacement.external_c_type_name = "OtherCarrier";
  output = OutputSelection(false, false);
  EXPECT_EQ(runtime_->Process(input, &output, &error), -3);
  EXPECT_EQ(output.rows[0][0].value, nullptr);
  EXPECT_EQ(output.rows[1][0].value, nullptr);
}

TEST_F(RuntimeFacadeTest,
       InvalidBatchAndClosedRuntimeKeepExistingErrorSemantics) {
  std::string error;
  std::vector<FacadeHostInput> hosts = {{"one"}, {"two"}};
  auto output = OutputSelection(false, false);
  output.rows.pop_back();
  const auto invalid = runtime_->Process(Inputs(&hosts), &output, &error);
  EXPECT_EQ(invalid, -4);
  ASSERT_EQ(output.rows.size(), 1u);
  EXPECT_EQ(output.rows[0][0].value, nullptr);
  output = OutputSelection(false, false);
  auto malformed = Inputs(&hosts);
  malformed.views[0].count = 1;
  EXPECT_EQ(runtime_->Process(malformed, &output, &error), -3);
  malformed = Inputs(&hosts);
  malformed.views.clear();
  EXPECT_EQ(runtime_->Process(malformed, &output, &error), -3);
  ControlFailureStage control_stage = ControlFailureStage::kNone;
  EXPECT_NE(runtime_->ExecuteControl({9876, "{}"}, &error, &control_stage), 0);
  EXPECT_EQ(control_stage, ControlFailureStage::kUnsupported);
  EXPECT_FALSE(error.empty());
  ASSERT_EQ(runtime_->Close(&error), 0) << error;
  output = OutputSelection(false, false);
  EXPECT_EQ(runtime_->Process(Inputs(&hosts), &output, &error), -1);
}

TEST(RuntimeFacadeOptionsTest, CreationCannotIncreaseValidatedPoolBudget) {
  std::unique_ptr<ValidatedIoPlan> plan;
  std::string error;
  ASSERT_EQ(IoPlanResolver::ResolveFromPipelineJson(FacadeDocument(), "", &plan,
                                                    &error, nullptr, 1),
            0)
      << error;
  RuntimeCreateOptions options;
  options.output_pool_depth = 2;
  std::unique_ptr<SharedAlgorithmRuntime> runtime;
  const auto status = SharedAlgorithmRuntime::CreateFromIoPlan(
      std::move(plan), options, &runtime, &error);
  EXPECT_EQ(status, -2);
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(runtime, nullptr);
}

TEST(RuntimeFacadeOptionsTest, InvalidCreateOptionsReturnDiagnostic) {
  std::unique_ptr<SharedAlgorithmRuntime> runtime;
  std::string error;
  const auto status =
      SharedAlgorithmRuntime::Create(RuntimeCreateOptions{}, &runtime, &error);
  EXPECT_EQ(status, -2);
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(runtime, nullptr);
}

}  // namespace
}  // namespace llm_edgeflow
