#include <gtest/gtest.h>

#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "engine/backend_registry.h"
#include "engine/backends/onnxruntime/onnxruntime_backend.h"

namespace llm_edgeflow {
namespace {

#ifdef HAVE_ONNXRUNTIME
BackendLoadSpec OnnxSpec(const std::string& file,
                         const nlohmann::json& params = {
                             {"max_batch_size", 2}}) {
  BackendLoadSpec spec(ExecutionProtocol::kTensorGraph);
  const auto definition = BackendRegistry::Instance().Find("onnxruntime");
  EXPECT_TRUE(definition.has_value());
  std::string diagnostic;
  if (definition)
    EXPECT_TRUE(definition->params.Parse(params, &spec.params, &diagnostic))
        << diagnostic;
  spec.model_file = file;
  spec.execution_target = {0, "cpu"};
  return spec;
}

Tensor Ints(std::vector<int64_t> shape, std::vector<int64_t> values) {
  Tensor tensor;
  EXPECT_TRUE(
      CreateHostTensor({ElementType::kInt64, std::move(shape)}, &tensor));
  if (tensor.buffer &&
      tensor.buffer->ByteSize() == values.size() * sizeof(int64_t))
    std::memcpy(tensor.buffer->MutableData(), values.data(),
                tensor.buffer->ByteSize());
  else
    ADD_FAILURE() << "Invalid test tensor literal";
  return tensor;
}

TensorMap RerankInputs() {
  return {{"input_ids", Ints({2, 3}, {2, 4, 9, 3, 5, 7})},
          {"attention_mask", Ints({2, 3}, {1, 1, 0, 1, 1, 1})},
          {"token_type_ids", Ints({2, 3}, {0, 1, 1, 0, 1, 1})}};
}

TEST(OnnxRuntimeBackendTest,
     ActualGraphMetadataAndOwnedOutputsSurviveNextRunAndDestruction) {
  auto provider = BackendRegistry::Instance().Create("onnxruntime");
  ASSERT_NE(provider, nullptr);
  std::string diagnostic;
  auto base =
      provider->Load(OnnxSpec(EDGEFLOW_RERANK_ONNX_FIXTURE), &diagnostic);
  auto session = std::dynamic_pointer_cast<ITensorGraphSession>(base);
  ASSERT_NE(session, nullptr) << diagnostic;
  EXPECT_EQ(session->BackendType(), "onnxruntime");
  EXPECT_EQ(session->Protocol(), ExecutionProtocol::kTensorGraph);
  EXPECT_EQ(session->Concurrency(), InferenceConcurrency::kConcurrent);
  EXPECT_EQ(session->GetBatchPolicy().max_batch_size, 2u);
  EXPECT_EQ(session->GetBatchPolicy().fixed_batch_size, 0u);
  ASSERT_EQ(session->Inputs().size(), 3u);
  const std::vector<std::string> names{"input_ids", "attention_mask",
                                       "token_type_ids"};
  for (size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(session->Inputs()[i].name, names[i]);
    EXPECT_EQ(session->Inputs()[i].element_type, ElementType::kInt64);
    EXPECT_EQ(session->Inputs()[i].shape, (std::vector<int64_t>{-1, -1}));
  }
  ASSERT_EQ(session->Outputs().size(), 1u);
  EXPECT_EQ(session->Outputs()[0].name, "logits");
  EXPECT_EQ(session->Outputs()[0].element_type, ElementType::kFloat32);
  EXPECT_EQ(session->Outputs()[0].shape, (std::vector<int64_t>{-1, 1}));

  auto inputs = RerankInputs();
  TensorMap outputs;
  ASSERT_EQ(session->Run(inputs, &outputs, &diagnostic), 0) << diagnostic;
  ASSERT_EQ(outputs.size(), 1u);
  auto retained = outputs.at("logits");
  EXPECT_EQ(retained.desc.shape, (std::vector<int64_t>{2, 1}));
  const auto* values = GetTensorData<float>(retained);
  ASSERT_NE(values, nullptr);
  // Row 0: candidate 4 * .01 + all 6 * .001; row 1: 12 * .01 + 15 * .001.
  EXPECT_NEAR(values[0], 0.046f, 1e-7f);
  EXPECT_NEAR(values[1], 0.135f, 1e-7f);
  std::memset(inputs.at("input_ids").buffer->MutableData(), 0,
              inputs.at("input_ids").buffer->ByteSize());
  TensorMap next;
  ASSERT_EQ(session->Run(inputs, &next, &diagnostic), 0) << diagnostic;
  EXPECT_FLOAT_EQ(GetTensorData<float>(next.at("logits"))[0], 0.0f);
  EXPECT_NE(retained.buffer.get(), next.at("logits").buffer.get());
  provider.reset();
  session.reset();
  base.reset();
  inputs.clear();
  outputs.clear();
  next.clear();
  EXPECT_NEAR(GetTensorData<float>(retained)[0], 0.046f, 1e-7f);
  EXPECT_NEAR(GetTensorData<float>(retained)[1], 0.135f, 1e-7f);
}

TEST(OnnxRuntimeBackendTest, RunRejectsGraphInputMismatchAndClearsPriorOutput) {
  OnnxRuntimeBackend provider;
  std::string diagnostic;
  auto session = std::dynamic_pointer_cast<ITensorGraphSession>(
      provider.Load(OnnxSpec(EDGEFLOW_RERANK_ONNX_FIXTURE), &diagnostic));
  ASSERT_NE(session, nullptr) << diagnostic;
  const auto check = [&](TensorMap inputs, const std::string& expected) {
    TensorMap outputs{{"previous", Ints({1}, {8})}};
    diagnostic.clear();
    EXPECT_EQ(session->Run(inputs, &outputs, &diagnostic), -1);
    EXPECT_TRUE(outputs.empty());
    EXPECT_NE(diagnostic.find(expected), std::string::npos) << diagnostic;
  };
  auto missing = RerankInputs();
  missing.erase("attention_mask");
  check(std::move(missing), "Missing required input tensor: attention_mask");
  auto wrong_type = RerankInputs();
  wrong_type.at("input_ids").desc.element_type = ElementType::kInt32;
  check(std::move(wrong_type), "element type mismatch");
  auto wrong_rank = RerankInputs();
  wrong_rank["input_ids"] = Ints({6}, {1, 2, 3, 4, 5, 6});
  check(std::move(wrong_rank), "rank mismatch");
  auto wrong_batch = RerankInputs();
  wrong_batch["attention_mask"] = Ints({1, 3}, {1, 1, 1});
  check(std::move(wrong_batch), "batch dimensions are inconsistent");
  auto too_many = RerankInputs();
  too_many["input_ids"] = Ints({3, 2}, {1, 2, 3, 4, 5, 6});
  check(std::move(too_many), "exceeds max_batch_size");
  auto incompatible_sequence = RerankInputs();
  incompatible_sequence["attention_mask"] = Ints({2, 2}, {1, 1, 1, 1});
  check(std::move(incompatible_sequence), "broadcast");
  EXPECT_EQ(session->Run(RerankInputs(), nullptr, &diagnostic), -1);
  EXPECT_NE(diagnostic.find("pointer is null"), std::string::npos);
  TensorMap recovered;
  EXPECT_EQ(session->Run(RerankInputs(), &recovered, &diagnostic), 0)
      << diagnostic;
  EXPECT_EQ(recovered.size(), 1u);
}

TEST(OnnxRuntimeBackendTest, ConcurrentCallsHaveIndependentOutputStorage) {
  OnnxRuntimeBackend provider;
  std::string diagnostic;
  auto session = std::dynamic_pointer_cast<ITensorGraphSession>(
      provider.Load(OnnxSpec(EDGEFLOW_RERANK_ONNX_FIXTURE), &diagnostic));
  ASSERT_NE(session, nullptr) << diagnostic;
  std::vector<std::future<TensorMap>> calls;
  for (int i = 0; i < 4; ++i)
    calls.push_back(std::async(std::launch::async, [session] {
      TensorMap output;
      std::string error;
      EXPECT_EQ(session->Run(RerankInputs(), &output, &error), 0) << error;
      return output;
    }));
  std::vector<TensorMap> results;
  for (auto& call : calls) results.push_back(call.get());
  for (size_t i = 0; i < results.size(); ++i) {
    ASSERT_EQ(results[i].count("logits"), 1u);
    const auto& result = results[i].at("logits");
    EXPECT_NEAR(GetTensorData<float>(result)[1], 0.135f, 1e-7f);
    for (size_t j = 0; j < i; ++j)
      EXPECT_NE(result.buffer.get(), results[j].at("logits").buffer.get());
  }
}

TEST(OnnxRuntimeBackendTest,
     LoadRejectsMissingResourcesNonTensorMetadataAndUnsupportedTarget) {
  OnnxRuntimeBackend provider;
  std::string diagnostic;
  auto spec = OnnxSpec("");
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_EQ(diagnostic, "Model path is empty");
  spec.model_file = std::string(EDGEFLOW_RERANK_ONNX_FIXTURE) + ".missing";
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("not a regular file"), std::string::npos);
  spec.model_file = EDGEFLOW_NON_TENSOR_ONNX_FIXTURE;
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("output is not a tensor"), std::string::npos);
  spec.model_file = EDGEFLOW_RERANK_ONNX_FIXTURE;
  spec.execution_target.platform = "CUDA";
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("only supports CPU"), std::string::npos);
  spec.execution_target = {1, "CPU"};
  EXPECT_EQ(provider.Load(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("device_id 0"), std::string::npos);
  spec.execution_target = {0, "CPU"};
  EXPECT_NE(provider.Load(spec, &diagnostic), nullptr) << diagnostic;
}
#endif

TEST(OnnxRuntimeBackendTest, ProtocolAndBuildAvailabilityAreExplicit) {
  OnnxRuntimeBackend provider;
  EXPECT_EQ(provider.BackendType(), "onnxruntime");
  BackendLoadSpec wrong(ExecutionProtocol::kTextGeneration);
  std::string diagnostic;
  EXPECT_EQ(provider.Load(wrong, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("does not support requested protocol"),
            std::string::npos);
#ifdef HAVE_ONNXRUNTIME
  ASSERT_TRUE(BackendRegistry::Instance().Has("onnxruntime"));
  const auto definition = BackendRegistry::Instance().Find("onnxruntime");
  ASSERT_TRUE(definition);
  EXPECT_EQ(definition->supported_protocols,
            (std::vector<ExecutionProtocol>{ExecutionProtocol::kTensorGraph}));
  EXPECT_EQ(definition->concurrency, InferenceConcurrency::kConcurrent);
#else
  EXPECT_FALSE(BackendRegistry::Instance().Has("onnxruntime"));
  EXPECT_FALSE(BackendRegistry::Instance().Find("onnxruntime"));
  EXPECT_EQ(BackendRegistry::Instance().Create("onnxruntime"), nullptr);
  BackendLoadSpec disabled(ExecutionProtocol::kTensorGraph);
  EXPECT_EQ(provider.Load(disabled, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("not compiled"), std::string::npos);
#endif
}

TEST(OnnxRuntimeBackendTest, BatchPolicyComesFromCompatibleGraphDimensions) {
  using onnxruntime_detail::InferBatchPolicy;
  const TensorSpec dynamic{"tokens", ElementType::kInt64, {-1, 8}};
  const TensorSpec fixed{"result", ElementType::kFloat32, {3, 2}};
  BatchPolicy policy;
  std::string diagnostic;
  ASSERT_TRUE(InferBatchPolicy({dynamic}, {fixed}, 2, &policy, &diagnostic));
  EXPECT_EQ(policy.max_batch_size, 3u);
  EXPECT_EQ(policy.fixed_batch_size, 3u);
  ASSERT_TRUE(InferBatchPolicy({dynamic}, {}, 7, &policy, &diagnostic));
  EXPECT_EQ(policy.max_batch_size, 7u);
  EXPECT_EQ(policy.fixed_batch_size, 0u);
  EXPECT_FALSE(InferBatchPolicy({{"other", ElementType::kInt64, {2, 8}}},
                                {fixed}, 7, &policy, &diagnostic));
  EXPECT_NE(diagnostic.find("Conflicting static"), std::string::npos);
  EXPECT_FALSE(InferBatchPolicy({{"scalar", ElementType::kInt64, {}}}, {}, 7,
                                &policy, &diagnostic));
  EXPECT_NE(diagnostic.find("rank 0"), std::string::npos);
  EXPECT_FALSE(InferBatchPolicy({{"zero", ElementType::kInt64, {0, 8}}}, {}, 7,
                                &policy, &diagnostic));
  EXPECT_NE(diagnostic.find("cannot be 0"), std::string::npos);
  EXPECT_FALSE(InferBatchPolicy({dynamic}, {}, 0, &policy, &diagnostic));
  EXPECT_FALSE(InferBatchPolicy({dynamic}, {}, 7, nullptr, &diagnostic));
}

TEST(OnnxRuntimeBackendTest, RuntimeOutputMetadataMustMatchGraphAndInvocation) {
  using onnxruntime_detail::ValidateOutputMetadata;
  const TensorSpec spec{"scores", ElementType::kFloat32, {-1, 2}};
  std::string diagnostic;
  EXPECT_TRUE(ValidateOutputMetadata(ElementType::kFloat32, {3, 2}, 6, spec, 3,
                                     &diagnostic));
  struct InvalidOutput {
    const char* label;
    ElementType type;
    std::vector<int64_t> shape;
    size_t elements;
    size_t batch;
    const char* error;
  };
  const std::vector<InvalidOutput> cases{
      {"dtype", ElementType::kInt64, {3, 2}, 6, 3, "element type mismatch"},
      {"rank", ElementType::kFloat32, {6}, 6, 3, "rank mismatch"},
      {"static extent",
       ElementType::kFloat32,
       {3, 1},
       3,
       3,
       "static dimension mismatch"},
      {"invocation batch",
       ElementType::kFloat32,
       {2, 2},
       4,
       3,
       "batch dimension mismatch"},
      {"element count",
       ElementType::kFloat32,
       {3, 2},
       5,
       3,
       "element count mismatch"},
      {"unresolved extent",
       ElementType::kFloat32,
       {-1, 2},
       6,
       3,
       "negative runtime dimension"},
  };
  for (const auto& invalid : cases) {
    SCOPED_TRACE(invalid.label);
    diagnostic.clear();
    EXPECT_FALSE(ValidateOutputMetadata(invalid.type, invalid.shape,
                                        invalid.elements, spec, invalid.batch,
                                        &diagnostic));
    EXPECT_NE(diagnostic.find(invalid.error), std::string::npos) << diagnostic;
  }
}

}  // namespace
}  // namespace llm_edgeflow
