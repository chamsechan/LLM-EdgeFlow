#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "contracts/config_schema.h"
#include "contracts/inference_payloads.h"
#include "contracts/traceable_item.h"
#include "core/alg_context.h"
#include "core/common_contracts.h"
#include "core/pipeline_catalog.h"
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
#include "engine/models/generated_text_embedding/generated_text_embedding_model.h"
#include "engine/models/vision_document/image_decode.h"
#include "engine/models/vision_document/vision_document_model.h"
#include "engine/models/whisper_asr/whisper_asr_model.h"

using namespace llm_edgeflow;

namespace {

/**
 * @brief 测试专用的 Fake Embedding 模型实现
 */
class TestEmbeddingModel : public IEmbeddingModel {
 public:
  static constexpr const char* kModelType = "test_embedding_model";
  static constexpr const char* kCapability = "embedding";

  static ModelDefinition MakeDefinition() {
    ModelDefinition def;
    def.model_type = kModelType;
    def.capability = kCapability;
    def.description = "Test Embedding Model for Unit Tests";
    def.required_protocol = ExecutionProtocol::kTensorGraph;
    def.concurrency = InferenceConcurrency::kConcurrent;
    def.config_fields = {
        ConfigFieldDefinition("dimension", ConfigValueKind::kInteger, false,
                              384, 1, 4096, {}, "Embedding dimension"),
    };
    return def;
  }

  static std::shared_ptr<IModel> Create(
      const ModelCreateContext& context,
      std::string* diagnostic = nullptr) noexcept {
    try {
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

  const std::string& ModelType() const noexcept override {
    static const std::string type = kModelType;
    return type;
  }

  const std::string& Capability() const noexcept override {
    static const std::string cap = kCapability;
    return cap;
  }

  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }

  size_t GetMaxBatchSize() const noexcept override {
    return session_ ? session_->GetBatchPolicy().max_batch_size : 1;
  }

  int Embed(const TextBatch& inputs, const EmbeddingOptions& options,
            EmbeddingBatch* outputs) noexcept override {
    if (!outputs) return -1;
    outputs->clear();
    if (inputs.empty()) return 0;

    BatchPolicy policy{session_ ? session_->GetBatchPolicy().max_batch_size : 1,
                       0};
    return FixedBatchExecutor::Execute<std::string, std::vector<float>>(
        inputs, policy,
        [&options](const BatchSlice& slice,
                   std::vector<std::vector<float>>* batch_out) {
          batch_out->resize(slice.valid_count);
          for (size_t i = 0; i < slice.valid_count; ++i) {
            std::vector<float> vec(384, 0.1f);
            if (options.normalize) {
              float norm = std::sqrt(384.0f * 0.1f * 0.1f);
              if (norm > 0.0f) {
                for (auto& v : vec) v /= norm;
              }
            }
            (*batch_out)[i] = std::move(vec);
          }
          return 0;
        },
        outputs);
  }

 private:
  std::shared_ptr<ITensorGraphSession> session_;
};

bool EnsureTestModelAndTensorBackendRegistered() {
  auto& backend_registry = BackendRegistry::Instance();
  if (!backend_registry.Has(test::TestTensorBackend::kBackendType) &&
      !backend_registry.Register(
          test::TestTensorBackend::MakeDefinition(),
          []() { return std::make_unique<test::TestTensorBackend>(); })) {
    return false;
  }

  auto& model_registry = ModelRegistry::Instance();
  if (!model_registry.Has(TestEmbeddingModel::kModelType) &&
      !model_registry.Register(TestEmbeddingModel::MakeDefinition(),
                               TestEmbeddingModel::Create)) {
    return false;
  }
  return true;
}

bool EnsureTestCausalBackendRegistered() {
  auto& backend_registry = BackendRegistry::Instance();
  return backend_registry.Has(test::TestCausalLmBackend::kBackendType) ||
         backend_registry.Register(
             test::TestCausalLmBackend::MakeDefinition(),
             []() { return std::make_unique<test::TestCausalLmBackend>(); });
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
    return ExecutionProtocol::kTensorGraph;
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
    definition.supported_protocols = {ExecutionProtocol::kTensorGraph};
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
  ASSERT_TRUE(EnsureTestModelAndTensorBackendRegistered());

  auto& backend_reg = BackendRegistry::Instance();
  auto& model_reg = ModelRegistry::Instance();

  // 验证查询
  auto backend_def = backend_reg.Find(test::TestTensorBackend::kBackendType);
  ASSERT_TRUE(backend_def.has_value());
  EXPECT_EQ(backend_def->backend_type, test::TestTensorBackend::kBackendType);

  auto model_def = model_reg.Find(TestEmbeddingModel::kModelType);
  ASSERT_TRUE(model_def.has_value());
  EXPECT_EQ(model_def->model_type, TestEmbeddingModel::kModelType);
  EXPECT_EQ(model_def->capability, TestEmbeddingModel::kCapability);
}

// ==============================================================================
// 4. PipelineCatalog 并发快照安全性测试
// ==============================================================================

TEST(ModelBackendDecouplingTest, PipelineCatalogConcurrentSnapshotSafety) {
  ASSERT_TRUE(EnsureTestModelAndTensorBackendRegistered());

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
  ASSERT_TRUE(EnsureTestModelAndTensorBackendRegistered());
  test::TestTensorBackend::ResetRequestedProtocol();

  ModelLoadSpec spec;
  spec.model_type = TestEmbeddingModel::kModelType;
  spec.backend_type = test::TestTensorBackend::kBackendType;
  spec.model_path = "/tmp/test_models/model.bin";
  spec.model_config = {{"dimension", 384}};
  spec.backend_config = {};

  std::string diag;
  auto model = ModelRuntimeFactory::Create(spec, &diag);
  ASSERT_NE(model, nullptr) << diag;
  ASSERT_TRUE(test::TestTensorBackend::RequestedProtocol().has_value());
  EXPECT_EQ(*test::TestTensorBackend::RequestedProtocol(),
            ExecutionProtocol::kTensorGraph);
  EXPECT_EQ(model->ModelType(), TestEmbeddingModel::kModelType);
  EXPECT_EQ(model->Capability(), "embedding");

  auto typed_embed = std::dynamic_pointer_cast<IEmbeddingModel>(model);
  ASSERT_NE(typed_embed, nullptr);

  // 校验推理调用
  TextBatch inputs = {{100, 0, "test query"}};
  EmbeddingBatch outputs;
  EXPECT_EQ(typed_embed->Embed(inputs, {true}, &outputs), 0);
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].req_id, 100U);
  EXPECT_EQ(outputs[0].data.size(), 384U);
}

TEST(ModelBackendDecouplingTest, ModelRuntimeFactoryProtocolMismatchRejection) {
  ASSERT_TRUE(EnsureTestModelAndTensorBackendRegistered());
  ASSERT_TRUE(EnsureTestCausalBackendRegistered());
  test::TestCausalLmBackend::ResetLoadCount();

  // 尝试将要求 tensor_graph 的 TestEmbeddingModel 绑定到 causal_lm 后端
  ModelLoadSpec mismatch_spec;
  mismatch_spec.model_type = TestEmbeddingModel::kModelType;
  mismatch_spec.backend_type = test::TestCausalLmBackend::kBackendType;
  mismatch_spec.model_path = "/tmp/dummy.bin";

  std::string diag;
  auto model = ModelRuntimeFactory::Create(mismatch_spec, &diag);
  EXPECT_EQ(model, nullptr);
  EXPECT_FALSE(diag.empty());
  EXPECT_EQ(test::TestCausalLmBackend::LoadCount(), 0)
      << "Protocol mismatch must fail before Backend::Load side effects";
}

TEST(ModelBackendDecouplingTest,
     ModelRuntimeFactoryRejectsStricterSessionConcurrency) {
  ASSERT_TRUE(EnsureTestModelAndTensorBackendRegistered());

  auto& backend_registry = BackendRegistry::Instance();
  if (!backend_registry.Has(DeclaredConcurrentTestBackend::kBackendType)) {
    ASSERT_TRUE(backend_registry.Register(
        DeclaredConcurrentTestBackend::MakeDefinition(),
        []() { return std::make_unique<DeclaredConcurrentTestBackend>(); }));
  }

  ModelLoadSpec spec;
  spec.model_type = TestEmbeddingModel::kModelType;
  spec.backend_type = DeclaredConcurrentTestBackend::kBackendType;
  spec.model_path = "/tmp/dummy.bin";

  std::string diagnostic;
  EXPECT_EQ(ModelRuntimeFactory::Create(spec, &diagnostic), nullptr);
  EXPECT_NE(diagnostic.find("stricter"), std::string::npos);
}

// ==============================================================================
// 6. ModelManager 原子批量注册与冲突隔离测试
// ==============================================================================

TEST(ModelBackendDecouplingTest, ModelManagerAtomicCommitAndCollision) {
  ASSERT_TRUE(EnsureTestModelAndTensorBackendRegistered());

  ModelManager manager;

  ModelLoadSpec spec{TestEmbeddingModel::kModelType,
                     test::TestTensorBackend::kBackendType,
                     "/tmp/test.bin",
                     {},
                     {},
                     {}};
  std::string diagnostic;
  auto m1 = ModelRuntimeFactory::Create(spec, &diagnostic);
  ASSERT_NE(m1, nullptr) << diagnostic;

  // 1. 成功原子注册
  std::vector<ModelRegistration> batch1 = {
      {"model_a",
       TestEmbeddingModel::kModelType,
       "embedding",
       test::TestTensorBackend::kBackendType,
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
       TestEmbeddingModel::kModelType,
       "embedding",
       test::TestTensorBackend::kBackendType,
       "rev_2",
       m1,
       {},
       {},
       {}},
      {"model_a",
       TestEmbeddingModel::kModelType,
       "embedding",
       test::TestTensorBackend::kBackendType,
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

  // 3. 自动 revision 必须包含完整物化输入，不能退化为 model_id 拼接。
  ModelManager revision_manager;
  ModelRegistration generated_revision;
  generated_revision.model_id = "model_with_generated_revision";
  generated_revision.model_type = TestEmbeddingModel::kModelType;
  generated_revision.capability = TestEmbeddingModel::kCapability;
  generated_revision.backend_type = test::TestTensorBackend::kBackendType;
  generated_revision.model = m1;
  generated_revision.resolved_model_path = "/models/embedding/model.onnx";
  generated_revision.normalized_model_config = {{"dimension", 384}};
  generated_revision.normalized_backend_config = {{"threads", 4}};
  ASSERT_TRUE(revision_manager.RegisterBatch({generated_revision}));
  const std::string revision =
      revision_manager.GetModelRevision(generated_revision.model_id);
  EXPECT_NE(revision.find(generated_revision.resolved_model_path),
            std::string::npos);
  EXPECT_NE(revision.find("\"dimension\":384"), std::string::npos);
  EXPECT_NE(revision.find("\"threads\":4"), std::string::npos);
}

// ==============================================================================
// 7. FixedBatchExecutor 严格输出与全量回滚测试
// ==============================================================================

TEST(ModelBackendDecouplingTest, FixedBatchExecutorStrictOutputsAndRollback) {
  std::vector<TraceableItem<int>> inputs = {
      {1, 0, 10}, {1, 1, 20}, {2, 0, 30}, {2, 1, 40}, {3, 0, 50}};
  BatchPolicy policy{2, 0};  // dynamic max batch size = 2
  std::vector<TraceableItem<int>> outputs;

  // 1. 正常执行
  int ret = FixedBatchExecutor::Execute<int, int>(
      inputs, policy,
      [](const BatchSlice& slice, std::vector<int>* batch_out) {
        batch_out->resize(slice.valid_count);
        for (size_t i = 0; i < slice.valid_count; ++i) {
          (*batch_out)[i] = static_cast<int>(slice.offset + i);
        }
        return 0;
      },
      &outputs);
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(outputs.size(), 5U);
  EXPECT_EQ(outputs[0].req_id, 1U);
  EXPECT_EQ(outputs[4].req_id, 3U);

  // 2. 回调返回多于预期数量时拒绝并回滚
  outputs.clear();
  ret = FixedBatchExecutor::Execute<int, int>(
      inputs, policy,
      [](const BatchSlice& slice, std::vector<int>* batch_out) {
        batch_out->resize(slice.valid_count + 1, 0);  // 多返回一项
        return 0;
      },
      &outputs);
  EXPECT_EQ(ret, -3);
  EXPECT_TRUE(outputs.empty());

  // 3. 回调返回少于预期数量时拒绝并回滚
  outputs.clear();
  ret = FixedBatchExecutor::Execute<int, int>(
      inputs, policy,
      [](const BatchSlice& slice, std::vector<int>* batch_out) {
        batch_out->resize(slice.valid_count - 1, 0);  // 少返回一项
        return 0;
      },
      &outputs);
  EXPECT_EQ(ret, -3);
  EXPECT_TRUE(outputs.empty());

  // 4. 回调抛异常时硬捕获并回滚
  outputs.clear();
  ret = FixedBatchExecutor::Execute<int, int>(
      inputs, policy,
      [](const BatchSlice&, std::vector<int>*) -> int {
        throw std::runtime_error("Simulated inference exception");
      },
      &outputs);
  EXPECT_EQ(ret, -4);
  EXPECT_TRUE(outputs.empty());
}

namespace {
class DocumentImageFixture {
 public:
  DocumentImageFixture() {
    directory =
        std::filesystem::temp_directory_path() /
        ("edgeflow-image-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(directory))
      throw std::runtime_error("Cannot create image fixture");
    image = directory / "image.ppm";
    std::ofstream file(image, std::ios::binary);
    file << "P6\n2 1\n255\n";
    const char pixels[] = {static_cast<char>(255), 0, 0, 0,
                           static_cast<char>(255), 0};
    file.write(pixels, sizeof(pixels));
  }
  ~DocumentImageFixture() {
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
  }
  std::filesystem::path directory, image;
};

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
               std::string* output, std::string*) noexcept override {
    ++calls;
    EXPECT_EQ(input.width, 2);
    EXPECT_EQ(input.height, 2);
    EXPECT_FALSE(input.prompt.empty());
    *output = "TOTAL 12.50";
    return fail ? -1 : 0;
  }
  int calls = 0;
  bool fail = false;
};
}  // namespace

TEST(ModelBackendDecouplingTest, DocumentImageDecodesPngSample) {
  ImageTextInput image;
  std::string error;
  ASSERT_TRUE(DecodeDocumentImage("data/kite_invoice_sample.png", 16, 4194304,
                                  &image, &error))
      << error;
  EXPECT_EQ(image.width, 640);
  EXPECT_EQ(image.height, 320);
  EXPECT_EQ(image.rgb_chw.size(), 640U * 320U * 3U);
}

TEST(ModelBackendDecouplingTest, DocumentImageDecodePadsAndConvertsRgbPlanes) {
  DocumentImageFixture fixture;
  ImageTextInput image;
  std::string error;
  ASSERT_TRUE(DecodeDocumentImage(fixture.image.string(), 2, 4, &image, &error))
      << error;
  EXPECT_EQ(image.width, 2);
  EXPECT_EQ(image.height, 2);
  EXPECT_EQ(image.patch_size, 2);
  EXPECT_EQ(image.rgb_chw, (std::vector<uint8_t>{255, 0, 255, 255, 0, 255, 255,
                                                 255, 0, 0, 255, 255}));
  EXPECT_FALSE(
      DecodeDocumentImage(fixture.image.string(), 2, 3, &image, &error));
  EXPECT_TRUE(image.rgb_chw.empty());
  EXPECT_FALSE(
      DecodeDocumentImage(fixture.image.string(), 0, 4, &image, &error));
  EXPECT_FALSE(DecodeDocumentImage((fixture.directory / "missing").string(), 2,
                                   4, &image, &error));
  std::ofstream(fixture.image, std::ios::binary)
      << "P6\n2000000 2000000\n255\n";
  EXPECT_FALSE(
      DecodeDocumentImage(fixture.image.string(), 2, 4, &image, &error));
  std::ofstream(fixture.image, std::ios::binary) << "not an image";
  EXPECT_FALSE(
      DecodeDocumentImage(fixture.image.string(), 2, 4, &image, &error));
}

TEST(ModelBackendDecouplingTest,
     VisionDocumentPreservesIdsAndDoesNotInventBoxes) {
  DocumentImageFixture fixture;
  auto session = std::make_shared<DocumentImageSession>();
  ModelCreateContext context;
  context.backend_session = session;
  context.model_config = {{"patch_size", 2}};
  std::string error;
  auto model = std::dynamic_pointer_cast<IOcrModel>(
      VisionDocumentModel::Create(context, &error));
  ASSERT_NE(model, nullptr) << error;
  ImageRefBatch images{{123, 4, fixture.image.string()},
                       {987, 6, fixture.image.string()}};
  OcrDocumentBatch outputs;
  ASSERT_EQ(model->Recognize(images, &outputs), 0);
  ASSERT_EQ(outputs.size(), 2U);
  EXPECT_EQ(outputs[0].req_id, 123U);
  EXPECT_EQ(outputs[0].sub_id, 4U);
  EXPECT_EQ(outputs[1].req_id, 987U);
  EXPECT_EQ(outputs[1].sub_id, 6U);
  EXPECT_EQ(outputs[0].data.combined_text, "TOTAL 12.50");
  EXPECT_TRUE(outputs[0].data.boxes.empty());
  images[1].data = (fixture.directory / "missing").string();
  EXPECT_NE(model->Recognize(images, &outputs), 0);
  EXPECT_TRUE(outputs.empty());
  session->fail = true;
  EXPECT_NE(model->Recognize({images[0]}, &outputs), 0);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(model->Recognize({}, &outputs), 0);
  EXPECT_NE(model->Recognize({}, nullptr), 0);
  context.model_config = {{"patch_size", 0}};
  EXPECT_EQ(VisionDocumentModel::Create(context, &error), nullptr);
  context.backend_session.reset();
  EXPECT_EQ(VisionDocumentModel::Create(context, &error), nullptr);
}

TEST(ModelBackendDecouplingTest,
     VisionDocumentRejectsTensorBackendBeforeLoading) {
  ModelLoadSpec spec;
  spec.model_type = "vision_document";
  spec.backend_type = "onnxruntime";
  spec.model_path = "does-not-exist.gguf";
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
                         std::string*) noexcept override {
    prompts.push_back(prompt);
    EXPECT_TRUE(bos);
    EXPECT_EQ(limit, 3);
    *output = response;
    return prompts.size() == fail_call ? -7 : 0;
  }
  ExecutionProtocol protocol = ExecutionProtocol::kGeneratedTokenEmbedding;
  BatchPolicy policy{1, 0};
  GeneratedTokenEmbeddings response{{10, 11}, {{3, 0}, {0, 4}}};
  std::vector<std::string> prompts;
  size_t fail_call = 0;
};
ModelCreateContext EmbeddingContext(
    const std::shared_ptr<GeneratedEmbeddingSession>& session) {
  ModelCreateContext context;
  context.backend_session = session;
  context.model_config = {{"embedding_dim", 2},  {"max_tokens", 3},
                          {"prefix", "prefix:"}, {"suffix", ":suffix"},
                          {"add_bos", true},     {"pooling", "mean"}};
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
  ASSERT_EQ(model->Embed(inputs, {}, &output), 0);
  ASSERT_EQ(output.size(), 2U);
  EXPECT_EQ(output[0].req_id, 41U);
  EXPECT_EQ(output[0].sub_id, 7U);
  EXPECT_EQ(output[1].req_id, 82U);
  EXPECT_EQ(output[1].sub_id, 3U);
  EXPECT_EQ(session->prompts, (std::vector<std::string>{"prefix:one:suffix",
                                                        "prefix:two:suffix"}));
  EXPECT_NEAR(output[0].data[0], .6f, 1e-6f);
  EXPECT_NEAR(output[0].data[1], .8f, 1e-6f);
  ASSERT_EQ(model->Embed({inputs[0]}, {false}, &output), 0);
  EXPECT_EQ(output[0].data, (std::vector<float>{1.5f, 2.0f}));
  context.model_config["pooling"] = "last";
  model = std::dynamic_pointer_cast<IEmbeddingModel>(
      GeneratedTextEmbeddingModel::Create(context, &error));
  ASSERT_NE(model, nullptr) << error;
  ASSERT_EQ(model->Embed({inputs[0]}, {false}, &output), 0);
  EXPECT_EQ(output[0].data, (std::vector<float>{0, 4}));
  session->prompts.clear();
  session->fail_call = 2;
  EXPECT_EQ(model->Embed(inputs, {}, &output), -7);
  EXPECT_TRUE(output.empty());
  EXPECT_EQ(model->Embed({}, {}, &output), 0);
  EXPECT_TRUE(output.empty());
  EXPECT_NE(model->Embed({}, {}, nullptr), 0);
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
    EXPECT_NE(model->Embed({{1, 0, "input"}}, {}, &output), 0);
    EXPECT_TRUE(output.empty());
  }
  EmbeddingBatch output;
  session->response = {{1}, {{1, 2}}};
  EXPECT_NE(model->Embed({{1, 0, "valid"}, {2, 0, ""}}, {}, &output), 0);
  EXPECT_TRUE(output.empty());
}

TEST(ModelBackendDecouplingTest,
     GeneratedEmbeddingValidatesSessionAndConfiguration) {
  auto session = std::make_shared<GeneratedEmbeddingSession>();
  auto context = EmbeddingContext(session);
  std::string error;
  for (const auto& entry :
       std::vector<nlohmann::json>{{{"embedding_dim", 0}},
                                   {{"embedding_dim", 65537}},
                                   {{"embedding_dim", uint64_t{4294967298ULL}}},
                                   {{"embedding_dim", 2.5}},
                                   {{"max_tokens", 0}},
                                   {{"max_tokens", 65}},
                                   {{"pooling", "cls"}}}) {
    auto invalid = context;
    invalid.model_config.update(entry);
    EXPECT_EQ(GeneratedTextEmbeddingModel::Create(invalid, &error), nullptr);
  }
  context.model_config.erase("embedding_dim");
  EXPECT_EQ(GeneratedTextEmbeddingModel::Create(context, &error), nullptr);
  context = EmbeddingContext(session);
  session->policy = {1, 1};
  EXPECT_EQ(GeneratedTextEmbeddingModel::Create(context, &error), nullptr);
  session->policy = {2, 0};
  EXPECT_EQ(GeneratedTextEmbeddingModel::Create(context, &error), nullptr);
  session->policy = {1, 0};
  session->protocol = ExecutionProtocol::kTextGeneration;
  EXPECT_EQ(GeneratedTextEmbeddingModel::Create(context, &error), nullptr);
  context.backend_session.reset();
  EXPECT_EQ(GeneratedTextEmbeddingModel::Create(context, &error), nullptr);
  if (BackendRegistry::Instance().Find("onnxruntime")) {
    ModelLoadSpec spec;
    spec.model_type = "generated_text_embedding";
    spec.backend_type = "onnxruntime";
    spec.model_path = "does-not-exist";
    spec.model_config = {{"embedding_dim", 2}};
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
      inference_detail::SetDiagnostic(diagnostic,
                                      "Fake session error on designated call");
      return -1;
    }
    if (return_code != 0) {
      inference_detail::SetDiagnostic(diagnostic, "Fake session error");
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

  // 1. Successful creation with defaults
  auto model = WhisperAsrModel::Create(context, &error);
  ASSERT_NE(model, nullptr) << error;
  EXPECT_EQ(model->ModelType(), "whisper_asr");
  EXPECT_EQ(model->Capability(), "asr");
  EXPECT_EQ(model->Concurrency(), InferenceConcurrency::kConcurrent);
  EXPECT_EQ(model->GetMaxBatchSize(), 1U);

  // 2. Null session
  ModelCreateContext null_ctx;
  EXPECT_EQ(WhisperAsrModel::Create(null_ctx, &error), nullptr);

  // 3. Wrong protocol
  session->protocol = ExecutionProtocol::kTextGeneration;
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
  session->protocol = ExecutionProtocol::kAudioTranscription;

  // 4. Incompatible batch policy
  session->policy = {2, 0};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
  session->policy = {1, 1};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
  session->policy = {1, 0};

  // 5. Config validation: invalid language
  context.model_config = {{"language", "fr"}};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
  context.model_config = {{"language", ""}};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);

  // 6. Unsupported language by session
  session->supported_languages = {"en"};
  context.model_config = {{"language", "zh"}};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
  session->supported_languages = {"zh", "en", "auto"};

  // 7. Config validation: max_audio_seconds bounds [1, 60]
  context.model_config = {{"max_audio_seconds", 0}};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
  context.model_config = {{"max_audio_seconds", 61}};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);

  // 8. Config validation: max_output_bytes bounds [1, 65536]
  context.model_config = {{"max_output_bytes", 0}};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
  context.model_config = {{"max_output_bytes", 65537}};
  EXPECT_EQ(WhisperAsrModel::Create(context, &error), nullptr);
}

TEST(ModelBackendDecouplingTest, WhisperAsrModelTranscribeInputsAndBatching) {
  auto session = std::make_shared<FakeAudioTranscriptionSession>();
  session->transcript_to_return = "  你好世界  \n";
  ModelCreateContext context;
  context.backend_session = session;
  context.model_config = {{"language", "zh"},
                          {"max_audio_seconds", 30},
                          {"max_output_bytes", 1024}};

  std::string error;
  auto model = std::dynamic_pointer_cast<IAsrModel>(
      WhisperAsrModel::Create(context, &error));
  ASSERT_NE(model, nullptr) << error;

  // 1. Null outputs pointer returns -1
  AudioPcmBatch audio;
  EXPECT_EQ(model->Transcribe(audio, nullptr), -1);

  // 2. Empty batch returns 0, no session calls
  TextBatch outputs;
  EXPECT_EQ(model->Transcribe(audio, &outputs), 0);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 3. Item with empty PCM returns empty string, preserving req_id and sub_id
  audio.emplace_back(10, 1, AudioPcmPayload({}, 16000));
  EXPECT_EQ(model->Transcribe(audio, &outputs), 0);
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].req_id, 10U);
  EXPECT_EQ(outputs[0].sub_id, 1U);
  EXPECT_EQ(outputs[0].data, "");
  EXPECT_EQ(session->transcribe_call_count,
            0U);  // empty pcm skips backend call

  // 4. Sample rate != 16000 fails closed before session call
  audio.clear();
  outputs.clear();
  audio.emplace_back(11, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.0f), 8000));
  EXPECT_EQ(model->Transcribe(audio, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 5. Audio < 1600 samples (100ms) fails closed before session call
  audio.clear();
  audio.emplace_back(12, 0,
                     AudioPcmPayload(std::vector<float>(1599, 0.0f), 16000));
  EXPECT_EQ(model->Transcribe(audio, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 6. Audio > max_audio_seconds fails closed before session call
  audio.clear();
  audio.emplace_back(
      13, 0, AudioPcmPayload(std::vector<float>(30 * 16000 + 1, 0.0f), 16000));
  EXPECT_EQ(model->Transcribe(audio, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 7. Non-finite sample fails closed
  audio.clear();
  std::vector<float> nan_pcm(1600, 0.0f);
  nan_pcm[10] = std::numeric_limits<float>::quiet_NaN();
  audio.emplace_back(14, 0, AudioPcmPayload(std::move(nan_pcm), 16000));
  EXPECT_EQ(model->Transcribe(audio, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 8. Sample outside [-1, 1] fails closed
  audio.clear();
  std::vector<float> overflow_pcm(1600, 0.0f);
  overflow_pcm[5] = 1.05f;
  audio.emplace_back(15, 0, AudioPcmPayload(std::move(overflow_pcm), 16000));
  EXPECT_EQ(model->Transcribe(audio, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);

  // 9. Valid audio transcribes and trims whitespace
  audio.clear();
  audio.emplace_back(20, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.1f), 16000));
  EXPECT_EQ(model->Transcribe(audio, &outputs), 0);
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].req_id, 20U);
  EXPECT_EQ(outputs[0].sub_id, 0U);
  EXPECT_EQ(outputs[0].data, "你好世界");
  EXPECT_EQ(session->transcribe_call_count, 1U);

  // 10. Embedded NUL byte in output rejected and cleared
  session->return_embedded_nul = true;
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  session->return_embedded_nul = false;

  // 11. Invalid UTF-8 in output rejected and cleared
  session->return_invalid_utf8 = true;
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  session->return_invalid_utf8 = false;

  // 12. Output exceeds max_output_bytes rejected and cleared
  session->transcript_to_return = std::string(2000, 'A');
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  session->transcript_to_return = "你好世界";

  // 13. Multi-item batch preserves ordering and provenance
  audio.clear();
  audio.emplace_back(100, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.1f), 16000));
  audio.emplace_back(100, 1,
                     AudioPcmPayload(std::vector<float>(16000, 0.2f), 16000));
  audio.emplace_back(101, 0,
                     AudioPcmPayload(std::vector<float>(16000, 0.3f), 16000));
  session->transcribe_call_count = 0;
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, &outputs), 0);
  ASSERT_EQ(outputs.size(), 3U);
  EXPECT_EQ(outputs[0].req_id, 100U);
  EXPECT_EQ(outputs[0].sub_id, 0U);
  EXPECT_EQ(outputs[1].req_id, 100U);
  EXPECT_EQ(outputs[1].sub_id, 1U);
  EXPECT_EQ(outputs[2].req_id, 101U);
  EXPECT_EQ(outputs[2].sub_id, 0U);
  EXPECT_EQ(session->transcribe_call_count, 3U);

  // 14. Second item fails during inference -> all outputs cleared (rollback)
  session->fail_on_call_index = 2;
  session->transcribe_call_count = 0;
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 2U);
  session->fail_on_call_index.reset();

  // 15. Pre-validation on 3rd item failure -> session called 0 times
  audio[2].data.sample_rate = 8000;
  session->transcribe_call_count = 0;
  outputs.clear();
  EXPECT_EQ(model->Transcribe(audio, &outputs), -1);
  EXPECT_TRUE(outputs.empty());
  EXPECT_EQ(session->transcribe_call_count, 0U);
}

}  // namespace llm_edgeflow
