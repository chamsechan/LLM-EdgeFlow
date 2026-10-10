#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "contracts/config_schema.h"
#include "contracts/diagnostic.h"
#include "contracts/inference_payloads.h"
#include "contracts/parameters.h"
#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/pipeline_catalog.h"
#include "core/pipeline_validator.h"
#include "core/session_context.h"
#include "dev_support/inference/test_causal_lm_backend.h"
#include "dev_support/inference/test_tensor_backend.h"
#include "engine/backend_interface.h"
#include "engine/backend_registry.h"
#include "engine/fixed_batch_executor.h"
#include "engine/inference_definition.h"
#include "engine/model_interface.h"
#include "engine/model_registry.h"
#include "engine/model_runtime_factory.h"
#include "engine/models/bge_embedding/bge_embedding_model.h"
#include "engine/models/bge_reranker/bge_reranker_model.h"
#include "engine/models/common/from_model.h"
#include "engine/models/generated_text_embedding/generated_text_embedding_model.h"
#include "engine/models/vision_document/image_preprocess.h"
#include "engine/models/vision_document/vision_document_model.h"
#include "engine/models/whisper_asr/whisper_asr_model.h"
#include "tests/support/pipeline_test_utils.h"
#include "tests/support/scoped_allocation_failure.h"

#if defined(LLM_EDGEFLOW_TEST_WRAP_POSIX_MEMALIGN)
namespace {
thread_local bool fail_next_aligned_allocation = false;
}
extern "C" int __real_posix_memalign(void**, size_t, size_t);
extern "C" int __wrap_posix_memalign(void** pointer, size_t alignment,
                                     size_t size) {
  if (fail_next_aligned_allocation) {
    fail_next_aligned_allocation = false;
    return ENOMEM;
  }
  return __real_posix_memalign(pointer, alignment, size);
}
#endif

using namespace llm_edgeflow;

static_assert(!std::is_default_constructible_v<BackendLoadSpec>);
static_assert(std::is_constructible_v<BackendLoadSpec, ExecutionProtocol>);

namespace {

class UnitEmbeddingFixtureBackend final : public IInferenceBackend {
 public:
  static constexpr const char* kBackendType = "unit_embedding_fixture_backend";
  inline static std::optional<ExecutionProtocol> requested_protocol;
  class Session final : public test::TestTensorSession {
   public:
    using test::TestTensorSession::TestTensorSession;
    const std::string& BackendType() const noexcept override {
      static const std::string type = kBackendType;
      return type;
    }
    ExecutionProtocol Protocol() const noexcept override {
      return ExecutionProtocol::kFixture;
    }
  };
  static BackendDefinition MakeDefinition() {
    BackendDefinition definition;
    definition.backend_type = kBackendType;
    definition.supported_protocols = {ExecutionProtocol::kFixture};
    definition.concurrency = InferenceConcurrency::kConcurrent;
    return definition;
  }
  static void ResetRequestedProtocol() noexcept { requested_protocol.reset(); }
  static std::optional<ExecutionProtocol> RequestedProtocol() noexcept {
    return requested_protocol;
  }
  const std::string& BackendType() const noexcept override {
    static const std::string type = kBackendType;
    return type;
  }
  std::shared_ptr<IBackendSession> Load(const BackendLoadSpec& spec,
                                        std::string*) noexcept override {
    requested_protocol = spec.requested_protocol;
    try {
      return std::make_shared<Session>(spec.model_file);
    } catch (...) {
      return nullptr;
    }
  }
};

class UnitTextGenerationOnlyBackend final : public IInferenceBackend {
 public:
  static constexpr const char* kBackendType =
      "unit_text_generation_only_backend";
  inline static int load_calls = 0;
  static BackendDefinition MakeDefinition() {
    BackendDefinition definition;
    definition.backend_type = kBackendType;
    definition.supported_protocols = {ExecutionProtocol::kTextGeneration};
    definition.concurrency = InferenceConcurrency::kConcurrent;
    return definition;
  }
  const std::string& BackendType() const noexcept override {
    static const std::string type = kBackendType;
    return type;
  }
  std::shared_ptr<IBackendSession> Load(const BackendLoadSpec&,
                                        std::string*) noexcept override {
    ++load_calls;
    return nullptr;
  }
};

/**
 * @brief 测试专用的 Fake Embedding 模型实现
 */
class TestEmbeddingModel : public IEmbeddingModel {
 public:
  struct Params {
    int dimension = 384;
  };
  static constexpr const char* kImplName = "test_embedding_model";
  static constexpr const char* kCategory = "embedding";

  static ModelDefinition MakeDefinition() {
    ModelDefinition def;
    def.impl_name = kImplName;
    def.model_type = kCategory;
    def.description = "Test Embedding Model for Unit Tests";
    def.required_protocol = ExecutionProtocol::kFixture;
    def.fixture_backends = {UnitEmbeddingFixtureBackend::kBackendType,
                            "declared_concurrent_test_backend"};
    def.concurrency = InferenceConcurrency::kConcurrent;
    def.params = Parameters<Params>{Field("dimension", &Params::dimension)
                                        .Default(384)
                                        .Range(1, 4096)
                                        .Description("Embedding dimension")};
    return def;
  }

  static std::shared_ptr<IModel> Create(
      const ModelCreateContext& context,
      std::string* diagnostic = nullptr) noexcept {
    try {
      (void)context.Params<Params>();
      auto graph_sess = std::dynamic_pointer_cast<ITensorGraphSession>(
          context.backend_session);
      if (!graph_sess) {
        if (diagnostic) {
          *diagnostic = "TestEmbeddingModel requires ITensorGraphSession";
        }
        return nullptr;
      }
      return std::make_shared<TestEmbeddingModel>(std::move(graph_sess));
    } catch (...) {
      try {
        if (diagnostic) *diagnostic = "Exception creating TestEmbeddingModel";
      } catch (...) {
      }
      return nullptr;
    }
  }

  explicit TestEmbeddingModel(std::shared_ptr<ITensorGraphSession> session)
      : session_(std::move(session)) {}

  ~TestEmbeddingModel() override = default;

  const std::string& ImplName() const noexcept override {
    static const std::string type = kImplName;
    return type;
  }

  const std::string& ModelType() const noexcept override {
    static const std::string cap = kCategory;
    return cap;
  }

  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }

  int Embed(const TextBatch& inputs, EmbeddingBatch* outputs,
            std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
    if (!outputs) return -1;
    outputs->clear();
    if (inputs.empty()) return 0;

    const BatchPolicy policy =
        session_ ? session_->GetBatchPolicy() : BatchPolicy{1, 0};
    return FixedBatchExecutor::Execute<std::string, std::vector<float>>(
        inputs, policy,
        [](const BatchSlice& slice,
           std::vector<std::vector<float>>* batch_out) {
          batch_out->resize(slice.execution_count);
          for (size_t i = 0; i < slice.execution_count; ++i) {
            std::vector<float> vec(384, 0.1f);
            const float norm = std::sqrt(384.0f * 0.1f * 0.1f);
            for (auto& v : vec) v /= norm;
            (*batch_out)[i] = std::move(vec);
          }
          return 0;
        },
        outputs);
  }

 private:
  std::shared_ptr<ITensorGraphSession> session_;
};

bool EnsureTestModelAndFixtureBackendRegistered() {
  auto& backend_registry = BackendRegistry::Instance();
  if (!backend_registry.Has(UnitEmbeddingFixtureBackend::kBackendType) &&
      !backend_registry.Register(
          UnitEmbeddingFixtureBackend::MakeDefinition(),
          []() { return std::make_unique<UnitEmbeddingFixtureBackend>(); })) {
    return false;
  }

  auto& model_registry = ModelRegistry::Instance();
  if (!model_registry.Has(TestEmbeddingModel::kImplName) &&
      !model_registry.Register(TestEmbeddingModel::MakeDefinition(),
                               TestEmbeddingModel::Create)) {
    return false;
  }
  return true;
}

bool EnsureTextGenerationOnlyBackendRegistered() {
  auto& backend_registry = BackendRegistry::Instance();
  return backend_registry.Has(UnitTextGenerationOnlyBackend::kBackendType) ||
         backend_registry.Register(
             UnitTextGenerationOnlyBackend::MakeDefinition(), []() {
               return std::make_unique<UnitTextGenerationOnlyBackend>();
             });
}

class SmallAlignedTensorBuffer final : public ITensorBuffer {
 public:
  const void* Data() const noexcept override { return storage_.data(); }
  void* MutableData() noexcept override { return storage_.data(); }
  size_t ByteSize() const noexcept override { return storage_.size(); }

 private:
  alignas(64) std::array<std::byte, 64> storage_{};
};

class SerializedTensorSession final : public ITensorGraphSession {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string type = "declared_concurrent_test_backend";
    return type;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kFixture;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return {1, 0}; }
  const std::vector<TensorSpec>& Inputs() const noexcept override {
    static const std::vector<TensorSpec> inputs;
    return inputs;
  }
  const std::vector<TensorSpec>& Outputs() const noexcept override {
    static const std::vector<TensorSpec> outputs;
    return outputs;
  }
  int Run(const TensorMap&, TensorMap*, std::string*) noexcept override {
    return 0;
  }
};

class DeclaredConcurrentTestBackend final : public IInferenceBackend {
 public:
  static constexpr const char* kBackendType =
      "declared_concurrent_test_backend";

  static BackendDefinition MakeDefinition() {
    BackendDefinition definition;
    definition.backend_type = kBackendType;
    definition.supported_protocols = {ExecutionProtocol::kFixture};
    definition.concurrency = InferenceConcurrency::kConcurrent;
    return definition;
  }

  const std::string& BackendType() const noexcept override {
    static const std::string type = kBackendType;
    return type;
  }

  std::shared_ptr<IBackendSession> Load(const BackendLoadSpec&,
                                        std::string*) noexcept override {
    try {
      return std::make_shared<SerializedTensorSession>();
    } catch (...) {
      return nullptr;
    }
  }
};

class ModelValidationBackend final : public IInferenceBackend {
 public:
  struct Params {
    int threads = 1;
    std::string validation;
  };
  static constexpr const char* kBackendType = "model_validation_probe_backend";
  inline static int provider_calls = 0;
  inline static int load_calls = 0;
  inline static int validation_calls = 0;
  inline static nlohmann::json validated_config;
  inline static nlohmann::json loaded_config;
  inline static bool return_session = false;
  inline static ExecutionProtocol session_protocol =
      ExecutionProtocol::kTensorGraph;

  class Session final : public test::TestTensorSession {
   public:
    using test::TestTensorSession::TestTensorSession;
    const std::string& BackendType() const noexcept override {
      static const std::string type = kBackendType;
      return type;
    }
    ExecutionProtocol Protocol() const noexcept override {
      return session_protocol;
    }
  };

  static BackendDefinition MakeDefinition() {
    BackendDefinition definition;
    definition.backend_type = kBackendType;
    definition.supported_protocols = {ExecutionProtocol::kTensorGraph,
                                      ExecutionProtocol::kImageTextGeneration};
    definition.concurrency = InferenceConcurrency::kConcurrent;
    auto params = Parameters<Params>{
        Field("threads", &Params::threads).Default(1).Range(1, 8),
        Field("validation", &Params::validation).Default("accept")};
    params.Validate([](const Params& params, std::string* diagnostic) {
      ++validation_calls;
      validated_config = {{"threads", params.threads},
                          {"validation", params.validation}};
      if (params.validation == "reject") {
        SetDiagnosticNoexcept(diagnostic, "backend validator probe rejection");
        return false;
      }
      return true;
    });
    definition.params = std::move(params);
    return definition;
  }

  const std::string& BackendType() const noexcept override {
    static const std::string type = kBackendType;
    return type;
  }

  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec, std::string* diagnostic) noexcept override {
    try {
      ++load_calls;
      const auto& params = spec.Params<Params>();
      loaded_config = {{"threads", params.threads},
                       {"validation", params.validation}};
      if (return_session) return std::make_shared<Session>(spec.model_file);
      SetDiagnosticNoexcept(diagnostic, "validation probe stopped after Load");
      return nullptr;
    } catch (const std::exception& exception) {
      SetDiagnosticNoexcept(diagnostic, exception.what());
      return nullptr;
    }
  }
};

class FixtureParameterValidationBackend final : public IInferenceBackend {
 public:
  struct Params {
    int threads = 1;
    std::string validation;
  };
  static constexpr const char* kBackendType =
      "fixture_parameter_validation_backend";
  inline static int provider_calls = 0;
  inline static int load_calls = 0;
  inline static int validation_calls = 0;
  inline static nlohmann::json validated_config;
  inline static nlohmann::json loaded_config;
  inline static bool return_session = false;
  inline static std::optional<ExecutionProtocol> requested_protocol;
  inline static ExecutionProtocol session_protocol =
      ExecutionProtocol::kFixture;

  class Session final : public test::TestTensorSession {
   public:
    using test::TestTensorSession::TestTensorSession;
    const std::string& BackendType() const noexcept override {
      static const std::string type = kBackendType;
      return type;
    }
    ExecutionProtocol Protocol() const noexcept override {
      return session_protocol;
    }
  };

  static BackendDefinition MakeDefinition() {
    BackendDefinition definition;
    definition.backend_type = kBackendType;
    definition.supported_protocols = {ExecutionProtocol::kFixture};
    definition.concurrency = InferenceConcurrency::kConcurrent;
    auto params = Parameters<Params>{
        Field("threads", &Params::threads).Default(1).Range(1, 8),
        Field("validation", &Params::validation).Default("accept")};
    params.Validate([](const Params& params, std::string* diagnostic) {
      ++validation_calls;
      validated_config = {{"threads", params.threads},
                          {"validation", params.validation}};
      if (params.validation == "reject") {
        SetDiagnosticNoexcept(diagnostic, "backend validator probe rejection");
        return false;
      }
      return true;
    });
    definition.params = std::move(params);
    return definition;
  }

  const std::string& BackendType() const noexcept override {
    static const std::string type = kBackendType;
    return type;
  }

  std::shared_ptr<IBackendSession> Load(
      const BackendLoadSpec& spec, std::string* diagnostic) noexcept override {
    try {
      ++load_calls;
      requested_protocol = spec.requested_protocol;
      const auto& params = spec.Params<Params>();
      loaded_config = {{"threads", params.threads},
                       {"validation", params.validation}};
      if (return_session) return std::make_shared<Session>(spec.model_file);
      SetDiagnosticNoexcept(diagnostic, "validation probe stopped after Load");
      return nullptr;
    } catch (const std::exception& exception) {
      SetDiagnosticNoexcept(diagnostic, exception.what());
      return nullptr;
    }
  }
};

class DiagnosticAllocationException final : public std::exception {
 public:
  inline static constexpr char kReason[] =
      "Model validator sentinel with enough detail to exceed the Factory "
      "diagnostic prefix capacity and require another allocation when the "
      "exception reason is appended";

  explicit DiagnosticAllocationException(
      std::optional<test_support::ScopedAllocationFailure>* failure)
      : failure_(failure) {}

  const char* what() const noexcept override {
    // 测试在首次 append 及其回退期间都持有该作用域。
    if (failure_ && !failure_->has_value()) failure_->emplace(0);
    return kReason;
  }

 private:
  std::optional<test_support::ScopedAllocationFailure>* failure_;
};

class ConfigValidatedEmbeddingModel final : public TestEmbeddingModel {
 public:
  struct Params {
    int dimension = 384;
    std::string validation;
  };
  using TestEmbeddingModel::TestEmbeddingModel;
  static constexpr const char* kImplName = "config_validated_embedding_probe";
  inline static int validation_calls = 0;
  inline static int create_calls = 0;
  inline static nlohmann::json validated_config;
  inline static nlohmann::json created_config;
  inline static std::optional<test_support::ScopedAllocationFailure>*
      diagnostic_failure = nullptr;

  static ModelDefinition MakeDefinition() {
    auto definition = TestEmbeddingModel::MakeDefinition();
    definition.impl_name = kImplName;
    definition.fixture_backends = {
        FixtureParameterValidationBackend::kBackendType};
    auto params = Parameters<Params>{
        Field("dimension", &Params::dimension).Default(384).Range(1, 4096),
        Field("validation", &Params::validation).Default("accept")};
    params.Validate([](const Params& params, std::string* diagnostic) {
      ++validation_calls;
      validated_config = {{"dimension", params.dimension},
                          {"validation", params.validation}};
      const auto& behavior = params.validation;
      if (behavior == "throw_standard")
        throw std::runtime_error("model validator probe exception");
      if (behavior == "throw_unknown") throw 7;
      if (behavior == "throw_diagnostic_allocation")
        throw DiagnosticAllocationException(diagnostic_failure);
      if (behavior == "reject_silent") return false;
      if (behavior == "reject") {
        SetDiagnosticNoexcept(diagnostic, "model validator probe rejection");
        return false;
      }
      return true;
    });
    definition.params = std::move(params);
    return definition;
  }

  static std::shared_ptr<IModel> Create(const ModelCreateContext& context,
                                        std::string*) {
    ++create_calls;
    const auto& params = context.Params<Params>();
    created_config = {{"dimension", params.dimension},
                      {"validation", params.validation}};
    auto session =
        std::dynamic_pointer_cast<ITensorGraphSession>(context.backend_session);
    if (!session) return nullptr;
    return std::make_shared<ConfigValidatedEmbeddingModel>(std::move(session));
  }

  const std::string& ImplName() const noexcept override {
    static const std::string type = kImplName;
    return type;
  }
};

class ModelConfigValidationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(EnsureTestModelAndFixtureBackendRegistered());
    auto& backends = BackendRegistry::Instance();
    if (!backends.Has(ModelValidationBackend::kBackendType)) {
      ASSERT_TRUE(
          backends.Register(ModelValidationBackend::MakeDefinition(), [] {
            ++ModelValidationBackend::provider_calls;
            return std::make_unique<ModelValidationBackend>();
          }));
    }
    if (!backends.Has(FixtureParameterValidationBackend::kBackendType)) {
      ASSERT_TRUE(backends.Register(
          FixtureParameterValidationBackend::MakeDefinition(), [] {
            ++FixtureParameterValidationBackend::provider_calls;
            return std::make_unique<FixtureParameterValidationBackend>();
          }));
    }
    auto& models = ModelRegistry::Instance();
    if (!models.Has(ConfigValidatedEmbeddingModel::kImplName)) {
      ASSERT_TRUE(
          models.Register(ConfigValidatedEmbeddingModel::MakeDefinition(),
                          ConfigValidatedEmbeddingModel::Create));
    }
    ResetObservations();
  }

  static void ResetObservations() {
    ModelValidationBackend::provider_calls = 0;
    ModelValidationBackend::load_calls = 0;
    ModelValidationBackend::validation_calls = 0;
    ModelValidationBackend::validated_config = nullptr;
    ModelValidationBackend::loaded_config = nullptr;
    ModelValidationBackend::return_session = false;
    ModelValidationBackend::session_protocol = ExecutionProtocol::kTensorGraph;
    FixtureParameterValidationBackend::provider_calls = 0;
    FixtureParameterValidationBackend::load_calls = 0;
    FixtureParameterValidationBackend::validation_calls = 0;
    FixtureParameterValidationBackend::validated_config = nullptr;
    FixtureParameterValidationBackend::loaded_config = nullptr;
    FixtureParameterValidationBackend::return_session = false;
    FixtureParameterValidationBackend::session_protocol =
        ExecutionProtocol::kFixture;
    FixtureParameterValidationBackend::requested_protocol.reset();
    ConfigValidatedEmbeddingModel::validation_calls = 0;
    ConfigValidatedEmbeddingModel::create_calls = 0;
    ConfigValidatedEmbeddingModel::validated_config = nullptr;
    ConfigValidatedEmbeddingModel::created_config = nullptr;
    ConfigValidatedEmbeddingModel::diagnostic_failure = nullptr;
    UnitEmbeddingFixtureBackend::ResetRequestedProtocol();
  }

  static nlohmann::json Document(const ModelLoadSpec& spec) {
    const auto model_type =
        ModelRegistry::Instance().Find(spec.impl_name).value().model_type;
    nlohmann::json node = {{"name", "consumer"},
                           {"type", "text_embedding"},
                           {"inputs", {{"text", "input.sentence_text"}}},
                           {"params", {{"bind_model", "validation_model"}}}};
    if (model_type == "rerank") {
      node["type"] = "text_rerank";
      node["inputs"] = {{"queries", "input.query_text"},
                        {"candidates", "input.candidates"}};
    } else if (model_type == "ocr") {
      node["type"] = "ocr_detect";
      node["inputs"] = {{"images", "input.image"}};
    }
    return {{"models",
             {{{"name", "validation_model"},
               {"type", model_type},
               {"backend",
                {{"type", spec.backend_type}, {"params", spec.backend_params}}},
               {"file", spec.model_file},
               {"params", spec.model_params}}}},
            {"pipeline", nlohmann::json::array({node})}};
  }

  static PipelineIoBoundary Boundary(const ModelLoadSpec& spec) {
    const auto model_type =
        ModelRegistry::Instance().Find(spec.impl_name).value().model_type;
    if (model_type == "rerank")
      return MakeTestBoundary({{"input.query_text", "TextBatch"},
                               {"input.candidates", "RankedTextBatch", true,
                                "1:N", "generate_sub_id"}},
                              {{"consumer.ranked", "RankedTextBatch", true,
                                "1:N", "generate_sub_id"}});
    if (model_type == "ocr")
      return MakeTestBoundary({{"input.image", "ImageFrameBatch"}},
                              {{"consumer.text", "TextBatch"}});
    return MakeTestBoundary({{"input.sentence_text", "TextBatch"}},
                            {{"consumer.embedding", "EmbeddingBatch"}});
  }
};

}  // namespace

// ==============================================================================
// 1. 中性值契约与类型同一性测试
// ==============================================================================

TEST(ModelBackendDecouplingTest, NeutralContractsAndBlackboardTypeIdentity) {
  // 校验 Blackboard Key 与 contracts 定义类型严格一致
  static_assert(
      std::is_same_v<TextBatch, std::vector<TraceableItem<std::string>>>,
      "TextBatch must be std::vector<TraceableItem<std::string>>");
  static_assert(
      std::is_same_v<EmbeddingBatch,
                     std::vector<TraceableItem<std::vector<float>>>>,
      "EmbeddingBatch must be std::vector<TraceableItem<std::vector<float>>>");

  AlgContext ctx;
  constexpr BlackboardKey<TextBatch> kNeutralTexts{"neutral_texts",
                                                   "TextBatch"};
  TextBatch texts = {{1, 0, "test prompt"}};
  ctx.Publish(kNeutralTexts, texts);

  const auto* retrieved = ctx.Read(kNeutralTexts);
  ASSERT_NE(retrieved, nullptr);
  ASSERT_EQ(retrieved->size(), 1U);
  EXPECT_EQ((*retrieved)[0].data, "test prompt");
}

// ==============================================================================
// 2. 中性 Tensor 加固与安全访问测试
// ==============================================================================

TEST(ModelBackendDecouplingTest, HostTensorCreationAndTypedAccess) {
  static_assert(!std::is_copy_constructible_v<HostTensorBuffer>);
  static_assert(!std::is_copy_assignable_v<HostTensorBuffer>);

  TensorDesc desc{ElementType::kFloat32, {2, 384}};
  Tensor tensor;
  std::string diag;

  EXPECT_TRUE(CreateHostTensor(desc, &tensor, &diag));
  EXPECT_NE(tensor.buffer, nullptr);
  EXPECT_EQ(tensor.buffer->ByteSize(), 2 * 384 * sizeof(float));

  // 校验安全类型访问器
  diag.clear();
  const float* data_ptr = GetTensorData<float>(tensor, &diag);
  EXPECT_NE(data_ptr, nullptr);
  EXPECT_TRUE(diag.empty());

  float* mutable_ptr = GetMutableTensorData<float>(&tensor, &diag);
  EXPECT_NE(mutable_ptr, nullptr);
  EXPECT_TRUE(diag.empty());

  // 校验类型不匹配访问拒绝
  const int32_t* bad_type_ptr = GetTensorData<int32_t>(tensor, &diag);
  EXPECT_EQ(bad_type_ptr, nullptr);
  EXPECT_FALSE(diag.empty());
}

TEST(ModelBackendDecouplingTest, EmptyHostTensorIsValid) {
  Tensor tensor;
  std::string diagnostic;
  ASSERT_TRUE(
      CreateHostTensor({ElementType::kFloat32, {0, 4}}, &tensor, &diagnostic));
  ASSERT_NE(tensor.buffer, nullptr);
  EXPECT_EQ(tensor.buffer->ByteSize(), 0U);
  EXPECT_EQ(tensor.buffer->Data(), nullptr);
  EXPECT_TRUE(diagnostic.empty());
}

#if defined(LLM_EDGEFLOW_TEST_WRAP_POSIX_MEMALIGN)
TEST(ModelBackendDecouplingTest, AllocationFailureClearsTensorAndAllowsRetry) {
  Tensor tensor;
  std::string diagnostic;
  const TensorDesc descriptor{ElementType::kFloat32, {2, 4}};
  ASSERT_TRUE(CreateHostTensor(descriptor, &tensor, &diagnostic));
  fail_next_aligned_allocation = true;
  const bool created = CreateHostTensor(descriptor, &tensor, &diagnostic);
  const bool injected = !fail_next_aligned_allocation;
  fail_next_aligned_allocation = false;
  EXPECT_TRUE(injected);
  EXPECT_FALSE(created);
  EXPECT_EQ(tensor.buffer, nullptr);
  EXPECT_TRUE(tensor.desc.shape.empty());
  EXPECT_NE(diagnostic.find("allocate"), std::string::npos);
  diagnostic.clear();
  ASSERT_TRUE(CreateHostTensor(descriptor, &tensor, &diagnostic));
  EXPECT_NE(GetMutableTensorData<float>(&tensor), nullptr);
  EXPECT_EQ(tensor.buffer->ByteSize(), 8 * sizeof(float));
}
#endif

TEST(ModelBackendDecouplingTest, HostTensorFailClosedValidation) {
  Tensor tensor;
  std::string diag;

  // 1. 未知 ElementType (必须返回失败，不得默认按 1 字节处理)
  TensorDesc invalid_type_desc{static_cast<ElementType>(999), {2, 10}};
  EXPECT_FALSE(CreateHostTensor(invalid_type_desc, &tensor, &diag));
  EXPECT_EQ(tensor.buffer, nullptr);

  // 2. 负维度与运行时未解析维度 (-1) 拒绝
  diag.clear();
  TensorDesc negative_dim_desc{ElementType::kFloat32, {-1, 384}};
  EXPECT_FALSE(CreateHostTensor(negative_dim_desc, &tensor, &diag));
  EXPECT_EQ(tensor.buffer, nullptr);

  // 3. 乘法溢出与 byte size 溢出拒绝
  diag.clear();
  TensorDesc overflow_desc{ElementType::kFloat32, {INT64_MAX / 2, 4}};
  EXPECT_FALSE(CreateHostTensor(overflow_desc, &tensor, &diag));
  EXPECT_EQ(tensor.buffer, nullptr);

  // 4. 手工构造的 Tensor 也必须在 typed accessor 处再次 fail-closed。
  Tensor unchecked_negative{{ElementType::kFloat32, {-1, 4}},
                            std::make_shared<SmallAlignedTensorBuffer>()};
  diag.clear();
  EXPECT_EQ(GetTensorData<float>(unchecked_negative, &diag), nullptr);
  EXPECT_FALSE(diag.empty());

  Tensor unchecked_overflow{
      {ElementType::kFloat32, {INT64_MAX, INT64_MAX, INT64_MAX}},
      std::make_shared<SmallAlignedTensorBuffer>()};
  diag.clear();
  EXPECT_EQ(GetMutableTensorData<float>(&unchecked_overflow, &diag), nullptr);
  EXPECT_FALSE(diag.empty());
}

// ==============================================================================
// 3. Registry 动态注册测试；冲突场景在独立进程测试中执行
// ==============================================================================

TEST(ModelBackendDecouplingTest, TestModelAndBackendDynamicRegistration) {
  ASSERT_TRUE(EnsureTestModelAndFixtureBackendRegistered());

  auto& backend_reg = BackendRegistry::Instance();
  auto& model_reg = ModelRegistry::Instance();

  // 验证查询
  auto backend_def =
      backend_reg.Find(UnitEmbeddingFixtureBackend::kBackendType);
  ASSERT_TRUE(backend_def.has_value());
  EXPECT_EQ(backend_def->backend_type,
            UnitEmbeddingFixtureBackend::kBackendType);

  auto model_def = model_reg.Find(TestEmbeddingModel::kImplName);
  ASSERT_TRUE(model_def.has_value());
  EXPECT_EQ(model_def->impl_name, TestEmbeddingModel::kImplName);
  EXPECT_EQ(model_def->model_type, TestEmbeddingModel::kCategory);
}

// ==============================================================================
// 4. PipelineCatalog 并发快照安全性测试
// ==============================================================================

TEST(ModelBackendDecouplingTest, PipelineCatalogConcurrentSnapshotSafety) {
  ASSERT_TRUE(EnsureTestModelAndFixtureBackendRegistered());

  std::vector<std::thread> threads;
  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([]() {
      for (int j = 0; j < 50; ++j) {
        auto models = PipelineCatalog::Models();
        auto backends = PipelineCatalog::Backends();
        EXPECT_FALSE(models.empty());
        EXPECT_FALSE(backends.empty());
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  auto catalog_json = PipelineCatalog::ToJson();
  EXPECT_TRUE(catalog_json.contains("models"));
  EXPECT_TRUE(catalog_json.contains("backends"));
}

// ==============================================================================
// 5. ModelRuntimeFactory 规范物化与错误诊断测试
// ==============================================================================

TEST(ModelBackendDecouplingTest, TextGenerationSessionOwnsOutputLifetime) {
  auto concrete =
      std::make_shared<test::TestCausalLmSession>("/tmp/test-model.bin");
  std::shared_ptr<ITextGenerationSession> session = concrete;
  concrete.reset();

  GenerateOptions options;
  options.max_tokens = 8;
  std::string output;
  std::string diag;
  EXPECT_EQ(
      session->Generate("formatted prompt", false, options, 7, &output, &diag),
      0)
      << diag;
  EXPECT_EQ(output, "test-generation");
}

TEST(ModelBackendDecouplingTest, ModelRuntimeFactoryEndToEnd) {
  ASSERT_TRUE(EnsureTestModelAndFixtureBackendRegistered());
  UnitEmbeddingFixtureBackend::ResetRequestedProtocol();

  ModelLoadSpec spec;
  spec.impl_name = TestEmbeddingModel::kImplName;
  spec.backend_type = UnitEmbeddingFixtureBackend::kBackendType;
  spec.model_file = "/tmp/test_models/model.bin";
  spec.model_params = {{"dimension", 384}};
  spec.backend_params = nlohmann::json::object();

  std::string diag;
  auto model = ModelRuntimeFactory::Create(spec, &diag);
  ASSERT_NE(model, nullptr) << diag;
  ASSERT_TRUE(UnitEmbeddingFixtureBackend::RequestedProtocol().has_value());
  EXPECT_EQ(*UnitEmbeddingFixtureBackend::RequestedProtocol(),
            ExecutionProtocol::kFixture);
  EXPECT_EQ(model->ImplName(), TestEmbeddingModel::kImplName);
  EXPECT_EQ(model->ModelType(), "embedding");

  auto typed_embed = std::dynamic_pointer_cast<IEmbeddingModel>(model);
  ASSERT_NE(typed_embed, nullptr);

  // 校验推理调用
  TextBatch inputs = {{100, 0, "test query"}};
  EmbeddingBatch outputs;
  EXPECT_EQ(typed_embed->Embed(inputs, &outputs), 0);
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].req_id, 100U);
  EXPECT_EQ(outputs[0].data.size(), 384U);
}

TEST(ModelBackendDecouplingTest, ResolveFromModelSelectsDeclaredValueSources) {
  struct Case {
    std::optional<int64_t> configured;
    std::optional<int64_t> detected;
    std::optional<int64_t> fallback;
    int64_t expected;
  };
  const Case cases[] = {{256, std::nullopt, 512, 256},
                        {256, 256, 512, 256},
                        {std::nullopt, 384, 512, 384},
                        {std::nullopt, std::nullopt, 512, 512}};
  for (const auto& entry : cases) {
    int64_t resolved = -1;
    std::string diagnostic;
    ASSERT_TRUE(ResolveFromModel("embedding_dim", entry.configured,
                                 entry.detected, entry.fallback, &resolved,
                                 &diagnostic))
        << diagnostic;
    EXPECT_EQ(resolved, entry.expected);
  }

  int64_t resolved = -1;
  std::string diagnostic;
  EXPECT_FALSE(
      ResolveFromModel("embedding_dim", 256, 384, 512, &resolved, &diagnostic));
  EXPECT_EQ(resolved, -1);
  EXPECT_NE(diagnostic.find("does not match model value"), std::string::npos);
  diagnostic.clear();
  EXPECT_FALSE(ResolveFromModel("embedding_dim", std::nullopt, std::nullopt,
                                std::nullopt, &resolved, &diagnostic));
  EXPECT_EQ(resolved, -1);
  EXPECT_NE(diagnostic.find("specify it in params"), std::string::npos);
  EXPECT_FALSE(ResolveFromModel("embedding_dim", std::nullopt, std::nullopt,
                                std::nullopt, &resolved, nullptr));
}

TEST_F(ModelConfigValidationTest,
       ProductionSemanticErrorsFailBeforeBackendCreationOrLoading) {
  struct InvalidConfig {
    const char* impl_name;
    nlohmann::json config;
    const char* field;
  };
  const InvalidConfig cases[] = {
      {"vision_document", {{"prompt", ""}}, "prompt"},
      {"vision_document", {{"prompt", std::string("a\0b", 3)}}, "prompt"},
      {"bge_embedding",
       {{"embedding_dim", 384}, {"tokenizer_file", ""}},
       "tokenizer_file"},
      {"bge_embedding",
       {{"embedding_dim", 384},
        {"tokenizer_file", "/tmp/validation.vocab"},
        {"output_name", ""}},
       "output_name"},
      {"bge_reranker", {{"tokenizer_file", ""}}, "tokenizer_file"},
      {"bge_reranker",
       {{"tokenizer_file", "/tmp/validation.vocab"}, {"output_name", ""}},
       "output_name"},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(std::string(test_case.impl_name) + test_case.config.dump());
    ResetObservations();
    ModelLoadSpec spec;
    spec.impl_name = test_case.impl_name;
    spec.backend_type = ModelValidationBackend::kBackendType;
    spec.model_file = "validation.fixture";
    spec.model_params = test_case.config;

    const auto plan =
        PipelineValidator::ValidateAndPlan(Document(spec), Boundary(spec));
    ASSERT_FALSE(plan.report.ok);
    ASSERT_EQ(plan.report.diagnostics.size(), 1U)
        << plan.report.ToJson().dump(2);
    const auto& rejected = plan.report.diagnostics.front();
    EXPECT_EQ(rejected.code, DiagnosticCode::kInvalidCombination);
    EXPECT_EQ(rejected.path, "/models/0/params");
    EXPECT_NE(rejected.message.find(test_case.field), std::string::npos);
    EXPECT_EQ(ModelValidationBackend::provider_calls, 0);
    EXPECT_EQ(ModelValidationBackend::load_calls, 0);

    std::string diagnostic;
    EXPECT_EQ(ModelRuntimeFactory::Create(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find(test_case.field), std::string::npos);
    EXPECT_EQ(ModelValidationBackend::provider_calls, 0);
    EXPECT_EQ(ModelValidationBackend::load_calls, 0);
  }
}

TEST_F(ModelConfigValidationTest,
       ProductionDefaultsReachBackendAfterPreflight) {
  for (const char* impl_name :
       {"vision_document", "bge_embedding", "bge_reranker"}) {
    SCOPED_TRACE(impl_name);
    ResetObservations();
    ModelLoadSpec spec;
    spec.impl_name = impl_name;
    spec.backend_type = ModelValidationBackend::kBackendType;
    spec.model_file = "validation.fixture";
    if (spec.impl_name != "vision_document")
      spec.model_params["tokenizer_file"] = "/tmp/validation.vocab";
    if (spec.impl_name == "bge_embedding")
      spec.model_params["embedding_dim"] = 384;

    const auto plan =
        PipelineValidator::ValidateAndPlan(Document(spec), Boundary(spec));
    ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
    EXPECT_EQ(ModelValidationBackend::provider_calls, 0);
    EXPECT_EQ(ModelValidationBackend::load_calls, 0);

    // 探针有意止步于 Load，不涉及权重或资源 I/O。
    std::string diagnostic;
    EXPECT_EQ(ModelRuntimeFactory::Create(spec, &diagnostic), nullptr);
    EXPECT_EQ(ModelValidationBackend::provider_calls, 1);
    EXPECT_EQ(ModelValidationBackend::load_calls, 1);
    EXPECT_NE(diagnostic.find("validation probe stopped after Load"),
              std::string::npos);
  }
}

TEST_F(ModelConfigValidationTest, NormalizedDefaultsReachValidatorAndCreator) {
  for (const auto& config :
       {nlohmann::json::object(), nlohmann::json{{"dimension", 256}}}) {
    SCOPED_TRACE(config.dump());
    ResetObservations();
    ModelLoadSpec spec;
    spec.impl_name = ConfigValidatedEmbeddingModel::kImplName;
    spec.backend_type = FixtureParameterValidationBackend::kBackendType;
    spec.model_file = "validation.fixture";
    spec.model_params = config;
    const nlohmann::json expected = {
        {"dimension", config.value("dimension", 384)},
        {"validation", "accept"}};

    const auto plan =
        PipelineValidator::ValidateAndPlan(Document(spec), Boundary(spec));
    ASSERT_TRUE(plan.report.ok) << plan.report.ToJson().dump(2);
    EXPECT_EQ(ConfigValidatedEmbeddingModel::validation_calls, 1);
    EXPECT_EQ(ConfigValidatedEmbeddingModel::validated_config, expected);
    EXPECT_EQ(ConfigValidatedEmbeddingModel::create_calls, 0);
    EXPECT_FALSE(
        FixtureParameterValidationBackend::requested_protocol.has_value());

    ResetObservations();
    FixtureParameterValidationBackend::return_session = true;
    std::string diagnostic;
    auto model = ModelRuntimeFactory::Create(spec, &diagnostic);
    ASSERT_NE(model, nullptr) << diagnostic;
    EXPECT_EQ(ConfigValidatedEmbeddingModel::validation_calls, 1);
    EXPECT_EQ(ConfigValidatedEmbeddingModel::validated_config, expected);
    EXPECT_EQ(ConfigValidatedEmbeddingModel::create_calls, 1);
    EXPECT_EQ(ConfigValidatedEmbeddingModel::created_config, expected);
    EXPECT_EQ(FixtureParameterValidationBackend::requested_protocol,
              ExecutionProtocol::kFixture);
  }
}

TEST_F(ModelConfigValidationTest, FactoryParsesEachParameterGroupExactlyOnce) {
  for (bool override_defaults : {false, true}) {
    SCOPED_TRACE(override_defaults);
    ResetObservations();
    FixtureParameterValidationBackend::return_session = true;
    ModelLoadSpec spec;
    spec.impl_name = ConfigValidatedEmbeddingModel::kImplName;
    spec.backend_type = FixtureParameterValidationBackend::kBackendType;
    spec.model_file = "validation.fixture";
    if (override_defaults) {
      spec.model_params = {{"dimension", 256}};
      spec.backend_params = {{"threads", 4}};
    }
    const nlohmann::json expected_model = {
        {"dimension", override_defaults ? 256 : 384}, {"validation", "accept"}};
    const nlohmann::json expected_backend = {
        {"threads", override_defaults ? 4 : 1}, {"validation", "accept"}};

    std::string diagnostic;
    ASSERT_NE(ModelRuntimeFactory::Create(spec, &diagnostic), nullptr)
        << diagnostic;
    EXPECT_EQ(ConfigValidatedEmbeddingModel::validation_calls, 1);
    EXPECT_EQ(FixtureParameterValidationBackend::validation_calls, 1);
    EXPECT_EQ(ConfigValidatedEmbeddingModel::validated_config, expected_model);
    EXPECT_EQ(ConfigValidatedEmbeddingModel::created_config, expected_model);
    EXPECT_EQ(FixtureParameterValidationBackend::validated_config,
              expected_backend);
    EXPECT_EQ(FixtureParameterValidationBackend::loaded_config,
              expected_backend);
    EXPECT_EQ(FixtureParameterValidationBackend::provider_calls, 1);
    EXPECT_EQ(FixtureParameterValidationBackend::load_calls, 1);
    EXPECT_EQ(ConfigValidatedEmbeddingModel::create_calls, 1);
  }
}

TEST_F(ModelConfigValidationTest,
       FactoryRejectsInvalidBackendParametersBeforeProviderSideEffects) {
  for (const auto& config :
       std::vector<nlohmann::json>{{{"threads", 0}},
                                   {{"threads", 9}},
                                   {{"threads", "invalid"}},
                                   {{"unknown_field", true}},
                                   {{"validation", "reject"}}}) {
    SCOPED_TRACE(config.dump());
    ResetObservations();
    ModelLoadSpec spec;
    spec.impl_name = ConfigValidatedEmbeddingModel::kImplName;
    spec.backend_type = FixtureParameterValidationBackend::kBackendType;
    spec.model_file = "validation.fixture";
    spec.backend_params = config;
    std::string diagnostic;
    EXPECT_EQ(ModelRuntimeFactory::Create(spec, &diagnostic), nullptr);
    EXPECT_NE(diagnostic.find("Invalid backend configuration"),
              std::string::npos)
        << diagnostic;
    EXPECT_EQ(ConfigValidatedEmbeddingModel::validation_calls, 1);
    EXPECT_EQ(FixtureParameterValidationBackend::validation_calls,
              config.contains("validation") ? 1 : 0);
    EXPECT_EQ(FixtureParameterValidationBackend::provider_calls, 0);
    EXPECT_EQ(FixtureParameterValidationBackend::load_calls, 0);
    EXPECT_EQ(ConfigValidatedEmbeddingModel::create_calls, 0);
  }
}

TEST_F(ModelConfigValidationTest,
       FactoryRejectsLoadedSessionProtocolBeforeModelCreation) {
  FixtureParameterValidationBackend::return_session = true;
  FixtureParameterValidationBackend::session_protocol =
      ExecutionProtocol::kImageTextGeneration;
  ModelLoadSpec spec;
  spec.impl_name = ConfigValidatedEmbeddingModel::kImplName;
  spec.backend_type = FixtureParameterValidationBackend::kBackendType;
  spec.model_file = "validation.fixture";
  std::string diagnostic;
  EXPECT_EQ(ModelRuntimeFactory::Create(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("Backend session protocol"), std::string::npos)
      << diagnostic;
  EXPECT_EQ(ConfigValidatedEmbeddingModel::validation_calls, 1);
  EXPECT_EQ(FixtureParameterValidationBackend::validation_calls, 1);
  EXPECT_EQ(FixtureParameterValidationBackend::provider_calls, 1);
  EXPECT_EQ(FixtureParameterValidationBackend::load_calls, 1);
  EXPECT_EQ(ConfigValidatedEmbeddingModel::create_calls, 0);
}

TEST_F(ModelConfigValidationTest, FieldErrorsAndSemanticFailuresFailClosed) {
  struct InvalidConfig {
    nlohmann::json config;
    bool calls_validator;
    const char* semantic_diagnostic;
    bool throws = false;
  };
  const InvalidConfig cases[] = {
      {{{"dimension", "invalid"}}, false, ""},
      {{{"dimension", 0}}, false, ""},
      {{{"unknown_field", true}}, false, ""},
      {{{"validation", "reject"}}, true, "model validator probe rejection"},
      {{{"validation", "reject_silent"}}, true, "Semantic validation failed"},
      {{{"validation", "throw_standard"}},
       true,
       "model validator probe exception",
       true},
      {{{"validation", "throw_unknown"}}, true, "Unknown exception", true},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.config.dump());
    ResetObservations();
    ModelLoadSpec spec;
    spec.impl_name = ConfigValidatedEmbeddingModel::kImplName;
    spec.backend_type = FixtureParameterValidationBackend::kBackendType;
    spec.model_file = "validation.fixture";
    spec.model_params = test_case.config;

    const auto plan =
        PipelineValidator::ValidateAndPlan(Document(spec), Boundary(spec));
    ASSERT_FALSE(plan.report.ok);
    ASSERT_EQ(plan.report.diagnostics.size(), 1U)
        << plan.report.ToJson().dump(2);
    const auto& rejected = plan.report.diagnostics.front();
    if (test_case.calls_validator) {
      EXPECT_EQ(rejected.code, DiagnosticCode::kInvalidCombination);
      EXPECT_EQ(rejected.path, "/models/0/params");
      EXPECT_NE(rejected.message.find(test_case.semantic_diagnostic),
                std::string::npos);
    } else {
      EXPECT_EQ(rejected.path.find("/models/0/params/"), 0U);
    }
    if (test_case.calls_validator) {
      EXPECT_EQ(ConfigValidatedEmbeddingModel::validation_calls, 1);
    } else {
      EXPECT_EQ(ConfigValidatedEmbeddingModel::validation_calls, 0);
    }
    EXPECT_EQ(FixtureParameterValidationBackend::provider_calls, 0);
    EXPECT_EQ(FixtureParameterValidationBackend::load_calls, 0);

    ResetObservations();
    std::string diagnostic;
    EXPECT_EQ(ModelRuntimeFactory::Create(spec, &diagnostic), nullptr);
    EXPECT_FALSE(diagnostic.empty());
    if (test_case.calls_validator) {
      EXPECT_EQ(ConfigValidatedEmbeddingModel::validation_calls, 1);
    } else {
      EXPECT_EQ(ConfigValidatedEmbeddingModel::validation_calls, 0);
    }
    EXPECT_EQ(ConfigValidatedEmbeddingModel::create_calls, 0);
    EXPECT_EQ(FixtureParameterValidationBackend::validation_calls, 0);
    EXPECT_EQ(FixtureParameterValidationBackend::provider_calls, 0);
    EXPECT_EQ(FixtureParameterValidationBackend::load_calls, 0);
    if (test_case.throws) {
      EXPECT_NE(diagnostic.find(test_case.semantic_diagnostic),
                std::string::npos)
          << diagnostic;
      ResetObservations();
      EXPECT_EQ(ModelRuntimeFactory::Create(spec, nullptr), nullptr);
      EXPECT_EQ(ConfigValidatedEmbeddingModel::validation_calls, 1);
      EXPECT_EQ(ConfigValidatedEmbeddingModel::create_calls, 0);
      EXPECT_EQ(FixtureParameterValidationBackend::validation_calls, 0);
      EXPECT_EQ(FixtureParameterValidationBackend::provider_calls, 0);
      EXPECT_EQ(FixtureParameterValidationBackend::load_calls, 0);
    }
  }
}

TEST_F(ModelConfigValidationTest,
       FactorySurvivesExceptionDiagnosticAllocationFailure) {
  ModelLoadSpec spec;
  spec.impl_name = ConfigValidatedEmbeddingModel::kImplName;
  spec.backend_type = FixtureParameterValidationBackend::kBackendType;
  spec.model_file = "validation.fixture";
  spec.model_params = {{"validation", "throw_diagnostic_allocation"}};
  std::optional<test_support::ScopedAllocationFailure> failure;
  ConfigValidatedEmbeddingModel::diagnostic_failure = &failure;
  std::string diagnostic;

  // what() 在参数校验进入 catch 块后才启用下一次分配失败；较长的原因需分配
  // 诊断字符串。查找 Definition、归一化和执行校验器期间未启用分配失败。
  auto model = ModelRuntimeFactory::Create(spec, &diagnostic);
  const bool injected = failure.has_value() && failure->Triggered();
  failure.reset();
  ConfigValidatedEmbeddingModel::diagnostic_failure = nullptr;

  EXPECT_TRUE(injected);
  EXPECT_EQ(model, nullptr);
  EXPECT_NE(diagnostic.find(DiagnosticAllocationException::kReason),
            std::string::npos);
  EXPECT_EQ(ConfigValidatedEmbeddingModel::create_calls, 0);
  EXPECT_EQ(FixtureParameterValidationBackend::provider_calls, 0);
  EXPECT_EQ(FixtureParameterValidationBackend::load_calls, 0);
}

TEST_F(ModelConfigValidationTest,
       BgeParameterSchemaRejectsInvalidStringFields) {
  const nlohmann::json invalid_values[] = {
      42, true, nullptr, nlohmann::json::array(), nlohmann::json::object(), ""};
  for (const char* impl_name :
       {BgeEmbeddingModel::kImplName, BgeRerankerModel::kImplName}) {
    SCOPED_TRACE(impl_name);
    const auto definition = ModelRegistry::Instance().Find(impl_name);
    ASSERT_TRUE(definition.has_value());
    for (const char* field : {"tokenizer_file", "output_name"}) {
      SCOPED_TRACE(field);
      for (const auto& value : invalid_values) {
        SCOPED_TRACE(value.dump());
        // 参数错误在创建会话或打开模型资源前拒绝。
        std::shared_ptr<const ParameterValues> params;
        std::string diagnostic;
        EXPECT_FALSE(definition->params.Parse(
            field == std::string("tokenizer_file")
                ? nlohmann::json{{field, value}}
                : nlohmann::json{{"tokenizer_file", "/tmp/validation.vocab"},
                                 {field, value}},
            &params, &diagnostic));
        EXPECT_EQ(params, nullptr);
        EXPECT_NE(diagnostic.find(field), std::string::npos) << diagnostic;
      }
    }
  }
}

TEST(ModelBackendDecouplingTest, ModelRuntimeFactoryProtocolMismatchRejection) {
  ASSERT_TRUE(EnsureTestModelAndFixtureBackendRegistered());
  ASSERT_TRUE(EnsureTextGenerationOnlyBackendRegistered());
  UnitTextGenerationOnlyBackend::load_calls = 0;

  // 将 Fixture 模型绑定到只声明生产文本生成协议的后端，必须在 Load 前失败。
  ModelLoadSpec mismatch_spec;
  mismatch_spec.impl_name = TestEmbeddingModel::kImplName;
  mismatch_spec.backend_type = UnitTextGenerationOnlyBackend::kBackendType;
  mismatch_spec.model_file = "/tmp/dummy.bin";

  std::string diag;
  auto model = ModelRuntimeFactory::Create(mismatch_spec, &diag);
  EXPECT_EQ(model, nullptr);
  EXPECT_NE(diag.find("required protocol (fixture)"), std::string::npos)
      << diag;
  EXPECT_EQ(UnitTextGenerationOnlyBackend::load_calls, 0)
      << "Protocol mismatch must fail before Backend::Load side effects";
}

TEST(ModelBackendDecouplingTest,
     ModelRuntimeFactoryRejectsStricterSessionConcurrency) {
  ASSERT_TRUE(EnsureTestModelAndFixtureBackendRegistered());

  auto& backend_registry = BackendRegistry::Instance();
  if (!backend_registry.Has(DeclaredConcurrentTestBackend::kBackendType)) {
    ASSERT_TRUE(backend_registry.Register(
        DeclaredConcurrentTestBackend::MakeDefinition(),
        []() { return std::make_unique<DeclaredConcurrentTestBackend>(); }));
  }

  ModelLoadSpec spec;
  spec.impl_name = TestEmbeddingModel::kImplName;
  spec.backend_type = DeclaredConcurrentTestBackend::kBackendType;
  spec.model_file = "/tmp/dummy.bin";

  std::string diagnostic;
  EXPECT_EQ(ModelRuntimeFactory::Create(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("stricter"), std::string::npos);
}

// ==============================================================================
// 6. ModelManager 原子批量注册与冲突隔离测试
// ==============================================================================

TEST(ModelBackendDecouplingTest, ModelManagerAtomicCommitAndCollision) {
  ASSERT_TRUE(EnsureTestModelAndFixtureBackendRegistered());

  ModelManager manager;

  ModelLoadSpec spec{TestEmbeddingModel::kImplName,
                     UnitEmbeddingFixtureBackend::kBackendType,
                     "/tmp/test.bin",
                     nlohmann::json::object(),
                     nlohmann::json::object(),
                     {}};
  std::string diagnostic;
  auto m1 = ModelRuntimeFactory::Create(spec, &diagnostic);
  ASSERT_NE(m1, nullptr) << diagnostic;

  // 1. 成功原子注册
  std::vector<ModelRegistration> batch1 = {
      {"model_a",
       TestEmbeddingModel::kImplName,
       "embedding",
       UnitEmbeddingFixtureBackend::kBackendType,
       "rev_1",
       m1,
       {},
       {},
       {}},
  };
  EXPECT_TRUE(manager.RegisterBatch(batch1));
  EXPECT_TRUE(manager.HasModel("model_a"));
  EXPECT_EQ(manager.GetModelRevision("model_a"), "rev_1");

  // 2. 冲突批次 (同已有 ID 冲突) 必须全量回滚且不破坏状态
  std::vector<ModelRegistration> bad_batch = {
      {"model_b",
       TestEmbeddingModel::kImplName,
       "embedding",
       UnitEmbeddingFixtureBackend::kBackendType,
       "rev_2",
       m1,
       {},
       {},
       {}},
      {"model_a",
       TestEmbeddingModel::kImplName,
       "embedding",
       UnitEmbeddingFixtureBackend::kBackendType,
       "rev_3",
       m1,
       {},
       {},
       {}},  // 冲突项
  };
  EXPECT_FALSE(manager.RegisterBatch(bad_batch));
  EXPECT_FALSE(manager.HasModel("model_b"));  // 确保原子性：新项未被提交
  EXPECT_TRUE(manager.HasModel("model_a"));  // 原有项保持原样
  EXPECT_EQ(manager.GetModelRevision("model_a"), "rev_1");

  EXPECT_EQ(manager.GetModel<IModel>("model_a"), m1);

  // 3. 自动 revision 必须包含完整物化输入，不能退化为模型名拼接。
  ModelManager revision_manager;
  ModelRegistration generated_revision;
  generated_revision.model_name = "model_with_generated_revision";
  generated_revision.impl_name = TestEmbeddingModel::kImplName;
  generated_revision.model_type = TestEmbeddingModel::kCategory;
  generated_revision.backend_type = UnitEmbeddingFixtureBackend::kBackendType;
  generated_revision.model = m1;
  generated_revision.model_file = "/models/embedding/model.onnx";
  generated_revision.model_params = {{"dimension", 384}};
  generated_revision.backend_params = {{"threads", 4}};
  ASSERT_TRUE(revision_manager.RegisterBatch({generated_revision}));
  const std::string revision =
      revision_manager.GetModelRevision(generated_revision.model_name);
  EXPECT_NE(revision.find(generated_revision.model_file), std::string::npos);
  EXPECT_NE(revision.find("\"dimension\":384"), std::string::npos);
  EXPECT_NE(revision.find("\"threads\":4"), std::string::npos);
}

// ==============================================================================
// 7. FixedBatchExecutor 严格输出与全量回滚测试
// ==============================================================================

namespace {
ImageFrame DocumentImageFrame() { return {2, 1, 6, {255, 0, 0, 0, 255, 0}}; }

class DocumentImageSession final : public IImageTextGenerationSession {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string name = "test_image";
    return name;
  }
  ExecutionProtocol Protocol() const noexcept override {
    return ExecutionProtocol::kImageTextGeneration;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return {1, 0}; }
  int Generate(const ImageTextInput& input, const GenerateOptions&,
               std::string* output, std::string* diagnostic) noexcept override {
    if (diagnostic) diagnostic->clear();
    ++calls;
    EXPECT_EQ(input.width, 2);
    EXPECT_EQ(input.height, 2);
    EXPECT_FALSE(input.prompt.empty());
    *output = "TOTAL 12.50";
    if (fail) {
      SetDiagnosticNoexcept(diagnostic, "document image backend failed");
      return -1;
    }
    return 0;
  }
  int calls = 0;
  bool fail = false;
};
}  // namespace

TEST(ModelBackendDecouplingTest, DocumentImagePadsAndConvertsRgbPlanes) {
  ImageTextInput image;
  std::string error;
  ASSERT_TRUE(PrepareDocumentImage(DocumentImageFrame(), 2, 4, &image, &error))
      << error;
  EXPECT_EQ(image.width, 2);
  EXPECT_EQ(image.height, 2);
  EXPECT_EQ(image.patch_size, 2);
  EXPECT_EQ(image.rgb_chw, (std::vector<uint8_t>{255, 0, 255, 255, 0, 255, 255,
                                                 255, 0, 0, 255, 255}));

  // 行末填充字节不能成为像素；第二行必须按 stride 读取。
  const ImageFrame strided{1, 2, 5, {10, 20, 30, 99, 99, 40, 50, 60, 99, 99}};
  ASSERT_TRUE(PrepareDocumentImage(strided, 2, 4, &image, &error)) << error;
  EXPECT_EQ(image.rgb_chw, (std::vector<uint8_t>{10, 255, 40, 255, 20, 255, 50,
                                                 255, 30, 255, 60, 255}));
}

TEST(ModelBackendDecouplingTest, DocumentImageRejectsInvalidFramesAndLimits) {
  const ImageFrame valid = DocumentImageFrame();
  for (const auto& frame :
       std::vector<ImageFrame>{{0, 1, 3, {1, 2, 3}},
                               {-1, 1, 3, {1, 2, 3}},
                               {1, 0, 3, {1, 2, 3}},
                               {1, -1, 3, {1, 2, 3}},
                               {2, 1, 5, {1, 2, 3, 4, 5}},
                               {2, 1, 6, {1, 2, 3}},
                               {1, 2, 5, {1, 2, 3, 4, 5, 6, 7, 8, 9}}}) {
    ImageTextInput image;
    image.rgb_chw = {99};
    std::string error;
    EXPECT_FALSE(PrepareDocumentImage(frame, 2, 4, &image, &error));
    EXPECT_TRUE(image.rgb_chw.empty());
    EXPECT_FALSE(error.empty());
  }
  ImageTextInput image;
  std::string error;
  ASSERT_TRUE(PrepareDocumentImage(valid, 2, 4, &image, &error));
  EXPECT_FALSE(PrepareDocumentImage(valid, 2, 3, &image, &error));
  EXPECT_TRUE(image.rgb_chw.empty());
  ASSERT_TRUE(PrepareDocumentImage(valid, 2, 4, &image, &error));
  EXPECT_FALSE(PrepareDocumentImage(valid, 1, 1, &image, &error));
  EXPECT_TRUE(image.rgb_chw.empty());
  EXPECT_FALSE(PrepareDocumentImage(valid, 0, 4, &image, &error));
  EXPECT_FALSE(PrepareDocumentImage(valid, 2, 4, nullptr, &error));
}

TEST(ModelBackendDecouplingTest,
     VisionDocumentPreservesIdsAndDoesNotInventBoxes) {
  auto session = std::make_shared<DocumentImageSession>();
  ModelCreateContext context;
  context.backend_session = session;
  std::string error;
  const auto definition =
      ModelRegistry::Instance().Find(VisionDocumentModel::kImplName);
  ASSERT_TRUE(definition.has_value());
  ASSERT_TRUE(
      definition->params.Parse({{"patch_size", 2}}, &context.params, &error))
      << error;
  auto model = std::dynamic_pointer_cast<IOcrModel>(
      VisionDocumentModel::Create(context, &error));
  ASSERT_NE(model, nullptr) << error;
  ImageFrameBatch images{{123, 4, DocumentImageFrame()},
                         {987, 6, DocumentImageFrame()}};
  OcrDocumentBatch outputs;
  ASSERT_EQ(model->Recognize(images, &outputs), 0);
  ASSERT_EQ(outputs.size(), 2U);
  EXPECT_EQ(outputs[0].req_id, 123U);
  EXPECT_EQ(outputs[0].sub_id, 4U);
  EXPECT_EQ(outputs[1].req_id, 987U);
  EXPECT_EQ(outputs[1].sub_id, 6U);
  EXPECT_EQ(outputs[0].data.combined_text, "TOTAL 12.50");
  EXPECT_TRUE(outputs[0].data.boxes.empty());
  images[1].data.data.clear();
  EXPECT_NE(model->Recognize(images, &outputs), 0);
  EXPECT_TRUE(outputs.empty());
  session->fail = true;
  std::string diagnostic = "stale error";
  EXPECT_NE(model->Recognize({images[0]}, &outputs, &diagnostic), 0);
  EXPECT_EQ(diagnostic, "document image backend failed");
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(model->Recognize({}, &outputs, &diagnostic), 0);
  EXPECT_TRUE(diagnostic.empty());
  EXPECT_NE(model->Recognize({}, nullptr), 0);
  EXPECT_FALSE(
      definition->params.Parse({{"patch_size", 0}}, &context.params, &error));
  EXPECT_FALSE(error.empty());
  ASSERT_TRUE(
      definition->params.Parse({{"patch_size", 2}}, &context.params, &error))
      << error;
  context.backend_session.reset();
  EXPECT_EQ(VisionDocumentModel::Create(context, &error), nullptr);
}

TEST(ModelBackendDecouplingTest,
     VisionDocumentRejectsTensorBackendBeforeLoading) {
  ModelLoadSpec spec;
  spec.impl_name = "vision_document";
  spec.backend_type = "onnxruntime";
  spec.model_file = "does-not-exist.gguf";
  std::string error;
  if (!BackendRegistry::Instance().Find("onnxruntime")) GTEST_SKIP();
  EXPECT_EQ(ModelRuntimeFactory::Create(spec, &error), nullptr);
  EXPECT_NE(error.find("image_text_generation"), std::string::npos);
}

namespace llm_edgeflow {
namespace {
class GeneratedEmbeddingSession final : public IGeneratedTokenEmbeddingSession {
 public:
  const std::string& BackendType() const noexcept override {
    static const std::string name = "test_generated_embedding";
    return name;
  }
  ExecutionProtocol Protocol() const noexcept override { return protocol; }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kSerialized;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return policy; }
  int GenerateEmbeddings(const std::string& prompt, bool bos, int limit,
                         GeneratedTokenEmbeddings* output,
                         std::string* diagnostic) noexcept override {
    if (diagnostic) diagnostic->clear();
    prompts.push_back(prompt);
    EXPECT_TRUE(bos);
    EXPECT_EQ(limit, 3);
    *output = response;
    if (prompts.size() == fail_call) {
      SetDiagnosticNoexcept(diagnostic, "generated embedding backend failed");
      return -7;
    }
    return 0;
  }
  ExecutionProtocol protocol = ExecutionProtocol::kGeneratedTokenEmbedding;
  BatchPolicy policy{1, 0};
  GeneratedTokenEmbeddings response{{10, 11}, {{3, 0}, {0, 4}}};
  std::vector<std::string> prompts;
  size_t fail_call = 0;
};
ModelCreateContext EmbeddingContext(
    const std::shared_ptr<GeneratedEmbeddingSession>& session,
    const char* pooling = "mean", bool normalize = true) {
  ModelCreateContext context;
  context.backend_session = session;
  const auto definition =
      ModelRegistry::Instance().Find(GeneratedTextEmbeddingModel::kImplName);
  EXPECT_TRUE(definition.has_value());
  if (definition) {
    std::string error;
    EXPECT_TRUE(definition->params.Parse({{"embedding_dim", 2},
                                          {"max_tokens", 3},
                                          {"prompt_prefix", "prefix:"},
                                          {"prompt_suffix", ":suffix"},
                                          {"add_bos", true},
                                          {"pooling", pooling},
                                          {"normalize", normalize}},
                                         &context.params, &error))
        << error;
  }
  return context;
}
}  // namespace

TEST(ModelBackendDecouplingTest,
     GeneratedEmbeddingPoolsActualRowsAndPreservesProvenance) {
  auto session = std::make_shared<GeneratedEmbeddingSession>();
  auto context = EmbeddingContext(session);
  std::string error;
  auto model = std::dynamic_pointer_cast<IEmbeddingModel>(
      GeneratedTextEmbeddingModel::Create(context, &error));
  ASSERT_NE(model, nullptr) << error;
  EmbeddingBatch output;
  const TextBatch inputs{{41, 7, "one"}, {82, 3, "two"}};
  ASSERT_EQ(model->Embed(inputs, &output), 0);
  ASSERT_EQ(output.size(), 2U);
  EXPECT_EQ(output[0].req_id, 41U);
  EXPECT_EQ(output[0].sub_id, 7U);
  EXPECT_EQ(output[1].req_id, 82U);
  EXPECT_EQ(output[1].sub_id, 3U);
  EXPECT_EQ(session->prompts, (std::vector<std::string>{"prefix:one:suffix",
                                                        "prefix:two:suffix"}));
  EXPECT_NEAR(output[0].data[0], .6f, 1e-6f);
  EXPECT_NEAR(output[0].data[1], .8f, 1e-6f);
  EXPECT_NEAR(std::hypot(output[0].data[0], output[0].data[1]), 1.0f, 1e-6f);
  context = EmbeddingContext(session, "mean", false);
  model = std::dynamic_pointer_cast<IEmbeddingModel>(
      GeneratedTextEmbeddingModel::Create(context, &error));
  ASSERT_NE(model, nullptr) << error;
  ASSERT_EQ(model->Embed({inputs[0]}, &output), 0);
  EXPECT_EQ(output[0].data, (std::vector<float>{1.5f, 2.0f}));
  const auto raw_vector = output[0].data;
  ASSERT_EQ(model->Embed({inputs[0]}, &output), 0);
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(output[0].data, raw_vector);
  EXPECT_NEAR(std::hypot(output[0].data[0], output[0].data[1]), 2.5f, 1e-6f);
  context = EmbeddingContext(session, "last", false);
  model = std::dynamic_pointer_cast<IEmbeddingModel>(
      GeneratedTextEmbeddingModel::Create(context, &error));
  ASSERT_NE(model, nullptr) << error;
  ASSERT_EQ(model->Embed({inputs[0]}, &output), 0);
  EXPECT_EQ(output[0].data, (std::vector<float>{0, 4}));
  session->prompts.clear();
  session->fail_call = 2;
  std::string diagnostic = "stale error";
  EXPECT_EQ(model->Embed(inputs, &output, &diagnostic), -7);
  EXPECT_EQ(diagnostic, "generated embedding backend failed");
  EXPECT_TRUE(output.empty());
  EXPECT_EQ(model->Embed({}, &output, &diagnostic), 0);
  EXPECT_TRUE(diagnostic.empty());
  EXPECT_TRUE(output.empty());
  EXPECT_NE(model->Embed({}, nullptr), 0);
}

TEST(ModelBackendDecouplingTest,
     GeneratedEmbeddingRejectsInvalidFeaturesWithoutPartialOutputs) {
  auto session = std::make_shared<GeneratedEmbeddingSession>();
  std::string error;
  auto model = std::dynamic_pointer_cast<IEmbeddingModel>(
      GeneratedTextEmbeddingModel::Create(EmbeddingContext(session), &error));
  ASSERT_NE(model, nullptr) << error;
  const std::vector<GeneratedTokenEmbeddings> invalid{
      {},
      {{1}, {}},
      {{1}, {{1}}},
      {{1}, {{1, 2, 3}}},
      {{1}, {{std::numeric_limits<float>::quiet_NaN(), 2}}},
      {{1}, {{1, std::numeric_limits<float>::infinity()}}},
      {{1}, {{0, 0}}},
      {{1, 2, 3, 4}, {{1, 2}, {1, 2}, {1, 2}, {1, 2}}}};
  for (const auto& response : invalid) {
    session->response = response;
    EmbeddingBatch output{{99, 9, {42}}};
    EXPECT_NE(model->Embed({{1, 0, "input"}}, &output), 0);
    EXPECT_TRUE(output.empty());
  }
  EmbeddingBatch output;
  session->response = {{1}, {{1, 2}}};
  EXPECT_NE(model->Embed({{1, 0, "valid"}, {2, 0, ""}}, &output), 0);
  EXPECT_TRUE(output.empty());
}

TEST(ModelBackendDecouplingTest,
     GeneratedEmbeddingValidatesSessionAndConfiguration) {
  auto session = std::make_shared<GeneratedEmbeddingSession>();
  auto context = EmbeddingContext(session);
  std::string error;
  const auto definition =
      ModelRegistry::Instance().Find(GeneratedTextEmbeddingModel::kImplName);
  ASSERT_TRUE(definition.has_value());
  for (const auto& entry :
       std::vector<nlohmann::json>{{{"embedding_dim", 0}},
                                   {{"embedding_dim", 65537}},
                                   {{"embedding_dim", uint64_t{4294967298ULL}}},
                                   {{"embedding_dim", 2.5}},
                                   {{"max_tokens", 0}},
                                   {{"max_tokens", 65}},
                                   {{"pooling", "cls"}}}) {
    auto invalid = nlohmann::json{{"embedding_dim", 2}};
    invalid.update(entry);
    std::shared_ptr<const ParameterValues> params;
    EXPECT_FALSE(definition->params.Parse(invalid, &params, &error));
    EXPECT_EQ(params, nullptr);
    EXPECT_FALSE(error.empty());
  }
  EXPECT_FALSE(definition->params.Parse(nlohmann::json::object(),
                                        &context.params, &error));
  EXPECT_FALSE(error.empty());
  context = EmbeddingContext(session);
  session->policy = {1, 1};
  EXPECT_EQ(GeneratedTextEmbeddingModel::Create(context, &error), nullptr);
  session->policy = {2, 0};
  EXPECT_EQ(GeneratedTextEmbeddingModel::Create(context, &error), nullptr);
  session->policy = {1, 0};
  context.backend_session =
      std::make_shared<test::TestTensorSession>("wrong-interface.fixture");
  EXPECT_EQ(GeneratedTextEmbeddingModel::Create(context, &error), nullptr);
  context.backend_session.reset();
  EXPECT_EQ(GeneratedTextEmbeddingModel::Create(context, &error), nullptr);
  if (BackendRegistry::Instance().Find("onnxruntime")) {
    ModelLoadSpec spec;
    spec.impl_name = "generated_text_embedding";
    spec.backend_type = "onnxruntime";
    spec.model_file = "does-not-exist";
    spec.model_params = {{"embedding_dim", 2}};
    EXPECT_EQ(ModelRuntimeFactory::Create(spec, &error), nullptr);
    EXPECT_NE(error.find("generated_token_embedding"), std::string::npos);
  }
}

class FakeAudioTranscriptionSession : public IAudioTranscriptionSession {
 public:
  std::string backend_type = "fake_whisper";
  ExecutionProtocol protocol = ExecutionProtocol::kAudioTranscription;
  InferenceConcurrency concurrency = InferenceConcurrency::kSerialized;
  BatchPolicy policy{1, 0};
  std::set<std::string> supported_languages{"zh", "en", "auto"};
  std::string transcript_to_return = "你好世界";
  int return_code = 0;
  bool return_embedded_nul = false;
  bool return_invalid_utf8 = false;
  size_t transcribe_call_count = 0;
  std::optional<size_t> fail_on_call_index;

  const std::string& BackendType() const noexcept override {
    return backend_type;
  }
  ExecutionProtocol Protocol() const noexcept override { return protocol; }
  InferenceConcurrency Concurrency() const noexcept override {
    return concurrency;
  }
  BatchPolicy GetBatchPolicy() const noexcept override { return policy; }

  bool SupportsLanguage(std::string_view language) const noexcept override {
    return supported_languages.count(std::string(language)) > 0;
  }

  int Transcribe(const AudioPcmPayload& /*audio*/,
                 const AudioTranscriptionOptions& /*options*/,
                 std::string* output,
                 std::string* diagnostic = nullptr) noexcept override {
    ++transcribe_call_count;
    if (fail_on_call_index.has_value() &&
        transcribe_call_count == *fail_on_call_index) {
      SetDiagnosticNoexcept(diagnostic,
                            "Fake session error on designated call");
      return -1;
    }
    if (return_code != 0) {
      SetDiagnosticNoexcept(diagnostic, "Fake session error");
      return return_code;
    }
    if (return_embedded_nul) {
      if (output) *output = std::string("abc\0def", 7);
      return 0;
    }
    if (return_invalid_utf8) {
      if (output) *output = "\xFF\xFE bad utf8";
      return 0;
    }
    if (output) {
      *output = transcript_to_return;
    }
    return 0;
  }
};

TEST(ModelBackendDecouplingTest,
     WhisperAsrModelValidatesSessionAndConfiguration) {
  auto session = std::make_shared<FakeAudioTranscriptionSession>();
  ModelCreateContext context;
  context.backend_session = session;
  std::string error;
  const auto definition =
      ModelRegistry::Instance().Find(WhisperAsrModel::kImplName);
  ASSERT_TRUE(definition.has_value());
  ASSERT_TRUE(definition->params.Parse(nlohmann::json::object(),
                                       &context.params, &error))
      << error;

  // 1. 使用默认值成功创建
  auto model = WhisperAsrModel::Create(context, &error);
  ASSERT_NE(model, nullptr) << error;
  EXPECT_EQ(model->ImplName(), "whisper_asr");
  EXPECT_EQ(model->ModelType(), "asr");
  EXPECT_EQ(model->Concurrency(), InferenceConcurrency::kConcurrent);

  // 2. 会话为空
  auto null_ctx = context;
  null_ctx.backend_session.reset();
  EXPECT_EQ(WhisperAsrModel::Create(null_ctx, &error), nullptr);

  // 3. 会话不实现所需接口（协议由 Factory 统一核对）
  context.backend_session =
      std::make_shared<test::TestTensorSession>("wrong-interface.fixture");
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
  context.backend_session = session;

  // 4. 批策略不兼容
  session->policy = {2, 0};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
  session->policy = {1, 1};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
  session->policy = {1, 0};

  // 会话语言支持由模型查询委托，创建不选择请求语言。
  session->supported_languages = {"en"};
  auto asr_model = std::dynamic_pointer_cast<IAsrModel>(
      WhisperAsrModel::Create(context, &error));
  ASSERT_NE(asr_model, nullptr) << error;
  EXPECT_TRUE(asr_model->SupportsLanguage("en"));
  EXPECT_FALSE(asr_model->SupportsLanguage("zh"));
  session->supported_languages = {"zh", "en", "auto"};

  // 配置校验：max_audio_seconds 范围 [1, 60]
  EXPECT_FALSE(definition->params.Parse({{"max_audio_seconds", 0}},
                                        &context.params, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(definition->params.Parse({{"max_audio_seconds", 61}},
                                        &context.params, &error));
  EXPECT_FALSE(error.empty());

  // 8. 配置校验：max_output_bytes 范围 [1, 65536]
  EXPECT_FALSE(definition->params.Parse({{"max_output_bytes", 0}},
                                        &context.params, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(definition->params.Parse({{"max_output_bytes", 65537}},
                                        &context.params, &error));
  EXPECT_FALSE(error.empty());
}

TEST(ModelBackendDecouplingTest, WhisperAsrModelTranscribeInputsAndBatching) {
  auto session = std::make_shared<FakeAudioTranscriptionSession>();
  session->transcript_to_return = "  你好世界  \n";
  ModelCreateContext context;
  context.backend_session = session;
  std::string error;
  const auto definition =
      ModelRegistry::Instance().Find(WhisperAsrModel::kImplName);
  ASSERT_TRUE(definition.has_value());
  ASSERT_TRUE(definition->params.Parse(
      {{"max_audio_seconds", 30}, {"max_output_bytes", 1024}}, &context.params,
      &error))
      << error;
  auto model = std::dynamic_pointer_cast<IAsrModel>(
      WhisperAsrModel::Create(context, &error));
  ASSERT_NE(model, nullptr) << error;

  TranscribeOptions options;
  options.language = "zh";

  // 1. outputs 指针为空时返回 -1
  AudioPcmBatch audio;
  EXPECT_EQ(model->Transcribe(audio, options, nullptr), -1);

  // 2. 空批次返回 0，且不调用会话
  TextBatch outputs;
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), 0);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 3. PCM 为空的条目返回空字符串，并保留 req_id 和 sub_id
  audio.emplace_back(10, 1, AudioPcmPayload({}, 16000));
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), 0);
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].req_id, 10U);
  EXPECT_EQ(outputs[0].sub_id, 1U);
  EXPECT_EQ(outputs[0].data, "");
  EXPECT_EQ(session->transcribe_call_count,
            0U);  // 空 PCM 跳过 Backend 调用

  // 4. 采样率 != 16000 时，在调用会话前 fail-closed
  audio.clear();
  outputs.clear();
  audio.emplace_back(11, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.0f), 8000));
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 5. 音频少于 1600 个采样点 (100 毫秒) 时，在调用会话前 fail-closed
  audio.clear();
  audio.emplace_back(12, 0,
                     AudioPcmPayload(std::vector<float>(1599, 0.0f), 16000));
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 6. 音频超过 max_audio_seconds 时，在调用会话前 fail-closed
  audio.clear();
  audio.emplace_back(
      13, 0, AudioPcmPayload(std::vector<float>(30 * 16000 + 1, 0.0f), 16000));
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 7. 含非有限采样值时 fail-closed
  audio.clear();
  std::vector<float> nan_pcm(1600, 0.0f);
  nan_pcm[10] = std::numeric_limits<float>::quiet_NaN();
  audio.emplace_back(14, 0, AudioPcmPayload(std::move(nan_pcm), 16000));
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 8. 采样值超出 [-1, 1] 时 fail-closed
  audio.clear();
  std::vector<float> overflow_pcm(1600, 0.0f);
  overflow_pcm[5] = 1.05f;
  audio.emplace_back(15, 0, AudioPcmPayload(std::move(overflow_pcm), 16000));
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 9. 合法音频完成转写并去除首尾空白
  audio.clear();
  audio.emplace_back(20, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.1f), 16000));
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), 0);
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].req_id, 20U);
  EXPECT_EQ(outputs[0].sub_id, 0U);
  EXPECT_EQ(outputs[0].data, "你好世界");
  EXPECT_EQ(session->transcribe_call_count, 1U);

  // 10. 输出中嵌入 NUL 字节时拒绝并清空
  session->return_embedded_nul = true;
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  session->return_embedded_nul = false;

  // 11. 输出含非法 UTF-8 时拒绝并清空
  session->return_invalid_utf8 = true;
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  session->return_invalid_utf8 = false;

  // 12. 输出超过 max_output_bytes 时拒绝并清空
  session->transcript_to_return = std::string(2000, 'A');
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  session->transcript_to_return = "你好世界";

  // 13. 多条目批次保持顺序和来源
  audio.clear();
  audio.emplace_back(100, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.1f), 16000));
  audio.emplace_back(100, 1,
                     AudioPcmPayload(std::vector<float>(16000, 0.2f), 16000));
  audio.emplace_back(101, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.3f), 16000));
  session->transcribe_call_count = 0;
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), 0);
  ASSERT_EQ(outputs.size(), 3U);
  EXPECT_EQ(outputs[0].req_id, 100U);
  EXPECT_EQ(outputs[0].sub_id, 0U);
  EXPECT_EQ(outputs[1].req_id, 100U);
  EXPECT_EQ(outputs[1].sub_id, 1U);
  EXPECT_EQ(outputs[2].req_id, 101U);
  EXPECT_EQ(outputs[2].sub_id, 0U);
  EXPECT_EQ(session->transcribe_call_count, 3U);

  // 14. 第二个条目推理失败 -> 清空所有输出 (回滚)
  session->fail_on_call_index = 2;
  session->transcribe_call_count = 0;
  outputs.clear();
  std::string diagnostic = "stale error";
  EXPECT_EQ(model->Transcribe(audio, options, &outputs, &diagnostic), -1);
  EXPECT_EQ(diagnostic, "Fake session error on designated call");
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 2U);
  session->fail_on_call_index.reset();
  EXPECT_EQ(model->Transcribe(audio, options, &outputs, &diagnostic), 0);
  EXPECT_TRUE(diagnostic.empty());
  ASSERT_EQ(outputs.size(), audio.size());

  // 15. 第 3 个条目预校验失败 -> 会话调用 0 次
  audio[2].data.sample_rate = 8000;
  session->transcribe_call_count = 0;
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, options, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);
}

TEST(ModelBackendDecouplingTest,
     GeneratedEmbeddingHandlesFiniteExtremesAndZero) {
  auto session = std::make_shared<GeneratedEmbeddingSession>();
  for (const char* pooling : {"mean", "last"}) {
    for (float value : {1e20f, std::numeric_limits<float>::max(), 0.0f}) {
      session->response = {{1, 2}, {{value, value}, {value, value}}};
      for (bool normalize : {false, true}) {
        auto context = EmbeddingContext(session, pooling, normalize);
        auto model = std::dynamic_pointer_cast<IEmbeddingModel>(
            GeneratedTextEmbeddingModel::Create(context, nullptr));
        ASSERT_NE(model, nullptr);
        EmbeddingBatch output{{99, 0, {42}}};
        int code = model->Embed({{7, 4, "hello"}}, &output);
        if (normalize && value == 0) {
          EXPECT_NE(code, 0);
          EXPECT_TRUE(output.empty());
        } else {
          ASSERT_EQ(code, 0);
          ASSERT_EQ(output.size(), 1U);
          EXPECT_EQ(output[0].req_id, 7U);
          EXPECT_EQ(output[0].sub_id, 4U);
          for (float component : output[0].data) {
            EXPECT_TRUE(std::isfinite(component));
            EXPECT_FLOAT_EQ(component, normalize ? std::sqrt(0.5f) : value);
          }
        }
      }
    }
  }
}

TEST(ModelBackendDecouplingTest,
     BackendLoadSurvivesDiagnosticAllocationFailure) {
  for (const char* type :
       {"onnxruntime", "llama_cpp", "whisper_cpp", "kite_llm"}) {
    if (!BackendRegistry::Instance().Has(type)) continue;
    auto backend = BackendRegistry::Instance().Create(type);
    ASSERT_NE(backend, nullptr);
    BackendLoadSpec spec{static_cast<ExecutionProtocol>(999)};
    const auto definition = BackendRegistry::Instance().Find(type);
    ASSERT_TRUE(definition.has_value());
    std::string config_diagnostic;
    ASSERT_TRUE(definition->params.Parse(nlohmann::json::object(), &spec.params,
                                         &config_diagnostic))
        << type << ": " << config_diagnostic;
    bool injected = false;
    for (int step = 0; step < 4; ++step) {
      std::string diagnostic;
      std::shared_ptr<IBackendSession> session;
      {
        test_support::ScopedAllocationFailure fail(step);
        session = backend->Load(spec, &diagnostic);
        injected |= fail.Triggered();
      }
      EXPECT_EQ(session, nullptr) << type;
    }
    EXPECT_TRUE(injected) << type;
  }
}

}  // namespace llm_edgeflow
