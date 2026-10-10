#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "contracts/parameters.h"
#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/node_registry.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "engine/model_interface.h"
#include "nodes/authoring.h"
#include "nodes/node_base.h"
#include "nodes/node_error_codes.h"
#include "nodes/traceable_batch_validation.h"
#include "tests/support/model_registration.h"
#include "tests/support/node_test_utils.h"
#include "tests/support/registry_test_access.h"

namespace llm_edgeflow {

TEST(NodeErrorCodesTest, ActiveBusinessAndAuthoringErrorsAreDistinct) {
  EXPECT_EQ(node_error::control::kInvalidRequest, -1);
  EXPECT_EQ(node_error::text_embedding::kSessionInferenceFailed, -5101);
  EXPECT_EQ(node_error::structured_json_parse::kParseFailed, -6102);
  EXPECT_EQ(node_error::text_template::kRenderedOutputTooLong, -6201);

  const std::vector<int> codes = {
      node_error::control::kInvalidRequest,
      node_error::text_chunk::kInvalidUtf8,
      node_error::text_embedding::kSessionInferenceFailed,
      node_error::text_rule_match::kRegexExecutionFailed,
      node_error::structured_json_parse::kParseFailed,
      node_error::text_template::kRenderedOutputTooLong,
      node_error::text_template::kInvalidUtf8,
      node_error::author_node::kMissingInput,
      node_error::author_node::kBusinessError,
      node_error::author_node::kModelCallFailed,
      node_error::author_node::kOutputCountMismatch,
      node_error::author_node::kOutputProvenanceMismatch,
      node_error::author_node::kInternalError,
  };
  EXPECT_EQ(std::unordered_set<int>(codes.begin(), codes.end()).size(),
            codes.size());
}

// 1. 抛异常的测试 Node
class ExceptionThrowingNode : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "exception_throwing";
  explicit ExceptionThrowingNode(bool throw_in_init = false)
      : NodeBase(kNodeType), throw_in_init_(throw_in_init) {}

 protected:
  bool InitNode(const NodeInitContext&, const nlohmann::json&,
                SessionContext&) override {
    if (throw_in_init_) {
      throw std::runtime_error("Simulated Init failure");
    }
    return true;
  }

  int ProcessNode(AlgContext&) override {
    throw std::runtime_error("Simulated Process failure");
  }

 private:
  bool throw_in_init_ = false;
};

TEST(NodeBaseContractsTest, NullContextSafety) {
  ExceptionThrowingNode node;
  EXPECT_EQ(node.Process(nullptr),
            static_cast<int>(NodeRuntimeCode::kInvalidContext));
  EXPECT_FALSE(node.Init({}));
}

TEST(NodeBaseContractsTest, InitAndProcessExceptionSafety) {
  ExceptionThrowingNode fail_init_node(true);
  SessionContext session_ctx;

  std::string diagnostic = "stale";
  ValidatedNodePlan plan;
  plan.normalized_params = nlohmann::json::object();
  NodeInitContext init_ctx;
  init_ctx.plan = &plan;
  init_ctx.session_ctx = &session_ctx;
  init_ctx.diagnostic = &diagnostic;
  EXPECT_FALSE(fail_init_node.Init(init_ctx));
  EXPECT_EQ(diagnostic, "Simulated Init failure");
  init_ctx.session_ctx = nullptr;
  EXPECT_FALSE(fail_init_node.Init(init_ctx));
  EXPECT_NE(diagnostic.find("SessionContext"), std::string::npos);

  ExceptionThrowingNode fail_proc_node(false);
  init_ctx.session_ctx = &session_ctx;
  EXPECT_TRUE(fail_proc_node.Init(init_ctx));

  AlgContext ctx;
  int ret = fail_proc_node.Process(&ctx);
  EXPECT_EQ(ret, static_cast<int>(NodeRuntimeCode::kUnhandledException));
  EXPECT_EQ(ctx.GetErrorCode(),
            static_cast<int>(NodeRuntimeCode::kUnhandledException));
  EXPECT_TRUE(ctx.GetErrorMessage().find("Simulated Process failure") !=
              std::string::npos);
}

// 2. 使用模型能力的统一函数式编写
class MockAsrModel : public IAsrModel {
 public:
  const std::string& ImplName() const noexcept override {
    static const std::string t = "mock_asr";
    return t;
  }
  const std::string& ModelType() const noexcept override {
    static const std::string model_type = "asr";
    return model_type;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  bool SupportsLanguage(std::string_view) const noexcept override {
    return true;
  }
  int Transcribe(const AudioPcmBatch& inputs, const TranscribeOptions&,
                 TextBatch* outputs,
                 std::string* diagnostic = nullptr) noexcept override {
    if (diagnostic) diagnostic->clear();
    if (!outputs) return -1;
    ++infer_calls_;
    outputs->clear();
    if (fail_) {
      if (diagnostic) *diagnostic = "ASR device unavailable";
      return -6299;
    }
    for (const auto& item : inputs) {
      outputs->emplace_back(item.req_id, item.sub_id, "mock_transcription");
    }
    if (return_wrong_count_ && !outputs->empty()) {
      outputs->pop_back();
    }
    if (corrupt_provenance_ && outputs->size() > 1) {
      ++(*outputs)[1].sub_id;
    }
    return 0;
  }

  bool fail_ = false;
  int infer_calls_ = 0;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

inline constexpr BlackboardKey<AudioPcmBatch> kTestAudioInputs{
    "test_audio_inputs", "AudioPcmBatch"};
inline constexpr BlackboardKey<TextBatch> kTestTranscripts{"test_transcripts",
                                                           "TextBatch"};

struct MockAsrInputs {
  const AudioPcmBatch* audio = nullptr;
};
struct MockAsrModels {
  AsrCall transcriber;
};

auto MockAsrSpec() {
  return MakeNodeSpec(
      InputsOf<MockAsrInputs>{
          Required(kTestAudioInputs.name, &MockAsrInputs::audio)},
      PreservedOutput<TextBatch>(kTestTranscripts.name, kTestAudioInputs.name),
      ModelsOf<MockAsrModels>{
          Model("transcriber", "bind_model", &MockAsrModels::transcriber)},
      [](const MockAsrInputs& inputs, const MockAsrModels& models) {
        return models.transcriber.Transcribe(*inputs.audio,
                                             TranscribeOptions{});
      });
}
using MockAsrNode = AuthorNode<decltype(MockAsrSpec())>;

TEST(NodeBaseContractsTest, FunctionAsrWorkflow) {
  test_support::RegistryTestAccess::ScopedNodeState state_guard;
  auto definition = MockAsrSpec().BuildDefinition("mock_asr");
  ASSERT_TRUE(NodeRegistry::Instance().Register(
      definition.node_type,
      []() { return std::make_unique<MockAsrNode>("mock_asr", MockAsrSpec()); },
      definition));

  SessionContext session_ctx;
  auto model = std::make_shared<MockAsrModel>();
  RegisterTestModel(session_ctx.GetModelManager(), "test_asr_model", model,
                    "test-v1");

  MockAsrNode node("mock_asr", MockAsrSpec());
  ASSERT_TRUE(
      InitNodeForTest(node, {{"bind_model", "test_asr_model"}}, &session_ctx));

  AlgContext ctx;
  AudioPcmBatch audios;
  audios.emplace_back(0, 0, AudioPcmPayload{});
  audios.emplace_back(0, 1, AudioPcmPayload{});
  ctx.Publish(kTestAudioInputs, std::move(audios));

  int ret = node.Process(&ctx);
  EXPECT_EQ(ret, 0);

  const auto* results = ctx.Read(kTestTranscripts);
  ASSERT_NE(results, nullptr);
  ASSERT_EQ(results->size(), 2u);
  EXPECT_EQ((*results)[0].data, "mock_transcription");
  EXPECT_EQ(model->infer_calls_, 1);

  AlgContext empty_ctx;
  empty_ctx.Publish(kTestAudioInputs, AudioPcmBatch{});
  EXPECT_EQ(node.Process(&empty_ctx), 0);
  const auto* empty_results = empty_ctx.Read(kTestTranscripts);
  ASSERT_NE(empty_results, nullptr);
  EXPECT_TRUE(empty_results->empty());
  EXPECT_EQ(model->infer_calls_, 1);

  model->fail_ = true;
  AlgContext failure_ctx;
  failure_ctx.Publish(kTestAudioInputs,
                      AudioPcmBatch{{7, 0, AudioPcmPayload{}}});
  EXPECT_EQ(node.Process(&failure_ctx), -6299);
  EXPECT_EQ(failure_ctx.GetErrorCode(), -6299);
  EXPECT_NE(failure_ctx.GetErrorMessage().find("ASR device unavailable"),
            std::string::npos);
  EXPECT_FALSE(failure_ctx.Has(kTestTranscripts.name));
  model->fail_ = false;

  AudioPcmBatch invalid_audios;
  invalid_audios.emplace_back(7, 0, AudioPcmPayload{});
  invalid_audios.emplace_back(7, 1, AudioPcmPayload{});

  model->return_wrong_count_ = true;
  AlgContext count_ctx;
  count_ctx.Publish(kTestAudioInputs, invalid_audios);
  EXPECT_EQ(node.Process(&count_ctx),
            node_error::author_node::kOutputCountMismatch);
  EXPECT_FALSE(count_ctx.Has(kTestTranscripts.name));

  model->return_wrong_count_ = false;
  model->corrupt_provenance_ = true;
  AlgContext provenance_ctx;
  provenance_ctx.Publish(kTestAudioInputs, invalid_audios);
  EXPECT_EQ(node.Process(&provenance_ctx),
            node_error::author_node::kOutputProvenanceMismatch);
  EXPECT_FALSE(provenance_ctx.Has(kTestTranscripts.name));
}

TEST(NodeBaseContractsTest, TraceableAlignmentReportsFirstMismatch) {
  TextBatch inputs = {{1, 0, "first"}, {1, 1, "second"}};
  EmbeddingBatch outputs = {
      {1, 0, std::vector<float>{1.0F}},
      {1, 1, std::vector<float>{2.0F}},
  };

  auto result = ValidatePreservedTraceableAlignment(inputs, outputs);
  EXPECT_TRUE(result.IsAligned());
  EXPECT_EQ(result.mismatch_index, 2U);

  outputs.pop_back();
  result = ValidatePreservedTraceableAlignment(inputs, outputs);
  EXPECT_EQ(result.error, TraceableAlignmentError::kCountMismatch);
  EXPECT_EQ(result.mismatch_index, 1U);

  outputs.emplace_back(1, 9, std::vector<float>{2.0F});
  result = ValidatePreservedTraceableAlignment(inputs, outputs);
  EXPECT_EQ(result.error, TraceableAlignmentError::kProvenanceMismatch);
  EXPECT_EQ(result.mismatch_index, 1U);
}

}  // namespace llm_edgeflow

namespace llm_edgeflow {
TEST(NodeBaseContractsTest, UnconnectedPlannedInputCannotReadDefaultKey) {
  BoundInput<std::string> input("attributes");
  AlgContext ctx;
  ctx.Publish("attributes", std::string("unrelated"));
  input.Unbind();
  EXPECT_FALSE(input.IsBound());
  EXPECT_FALSE(input.Has(ctx));
  EXPECT_EQ(input.Get(ctx), nullptr);
  input.Resolve("attributes");
  EXPECT_EQ(*input.Get(ctx), "unrelated");
}
}  // namespace llm_edgeflow
