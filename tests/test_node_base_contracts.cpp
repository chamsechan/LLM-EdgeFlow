#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/pipeline_catalog.h"
#include "core/session_context.h"
#include "engine/model_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_error_codes.h"
#include "nodes/node_support.h"
#include "nodes/traceable_batch_validation.h"
#include "nodes/traceable_unary_inference_node.h"
#include "tests/support/node_test_utils.h"

namespace alg_framework {

TEST(NodeErrorCodesTest, UsesDistinctNodeDomains) {
  EXPECT_EQ(node_error::control::kInvalidRequest, -1);
  EXPECT_EQ(node_error::vector_top_k::kMissingInput, -3101);
  EXPECT_EQ(node_error::text_chunk::kMissingInput, -4001);
  EXPECT_EQ(node_error::text_embedding::kMissingInput, -4101);
  EXPECT_EQ(node_error::text_embedding::kOutputCountMismatch, -4102);
  EXPECT_EQ(node_error::text_embedding::kOutputProvenanceMismatch, -4103);
  EXPECT_EQ(node_error::text_embedding::kSessionInferenceFailed, -5101);
  EXPECT_EQ(node_error::llm_generate::kMissingInput, -4301);
  EXPECT_EQ(node_error::llm_generate::kOutputCountMismatch, -4302);
  EXPECT_EQ(node_error::llm_generate::kOutputProvenanceMismatch, -4303);
  EXPECT_EQ(node_error::text_rule_match::kMissingInput, -5001);
  EXPECT_EQ(node_error::structured_json_parse::kMissingInput, -6101);
  EXPECT_EQ(node_error::structured_json_parse::kParseFailed, -6102);
  EXPECT_EQ(node_error::text_template::kRenderedOutputTooLong, -6201);
  EXPECT_EQ(node_error::text_template::kMissingVariable, -6202);
  EXPECT_EQ(node_error::text_rerank::kMissingInput, -7001);
  EXPECT_EQ(node_error::text_rerank::kModelOutputMismatch, -7002);
  EXPECT_EQ(node_error::ocr_detect::kMissingInput, -7101);
  EXPECT_EQ(node_error::ocr_detect::kOutputCountMismatch, -7102);
  EXPECT_EQ(node_error::ocr_detect::kOutputProvenanceMismatch, -7103);
  EXPECT_EQ(node_error::asr_transcribe::kMissingInput, -7201);
  EXPECT_EQ(node_error::asr_transcribe::kOutputCountMismatch, -7202);
  EXPECT_EQ(node_error::asr_transcribe::kOutputProvenanceMismatch, -7203);

  const std::vector<int> codes = {
      node_error::control::kInvalidRequest,
      node_error::vector_top_k::kMissingInput,
      node_error::text_chunk::kMissingInput,
      node_error::text_chunk::kInvalidUtf8,
      node_error::text_embedding::kMissingInput,
      node_error::text_embedding::kOutputCountMismatch,
      node_error::text_embedding::kOutputProvenanceMismatch,
      node_error::text_embedding::kSessionInferenceFailed,
      node_error::llm_generate::kMissingInput,
      node_error::llm_generate::kOutputCountMismatch,
      node_error::llm_generate::kOutputProvenanceMismatch,
      node_error::text_rule_match::kMissingInput,
      node_error::text_rule_match::kRegexExecutionFailed,
      node_error::structured_json_parse::kMissingInput,
      node_error::structured_json_parse::kParseFailed,
      node_error::text_template::kRenderedOutputTooLong,
      node_error::text_template::kMissingVariable,
      node_error::text_template::kInvalidUtf8,
      node_error::text_rerank::kMissingInput,
      node_error::text_rerank::kModelOutputMismatch,
      node_error::ocr_detect::kMissingInput,
      node_error::ocr_detect::kOutputCountMismatch,
      node_error::ocr_detect::kOutputProvenanceMismatch,
      node_error::asr_transcribe::kMissingInput,
      node_error::asr_transcribe::kOutputCountMismatch,
      node_error::asr_transcribe::kOutputProvenanceMismatch,
  };
  EXPECT_EQ(std::unordered_set<int>(codes.begin(), codes.end()).size(),
            codes.size());
}

// 1. Exception Test Node
class ExceptionThrowingNode : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "ExceptionThrowingNode";
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
  EXPECT_FALSE(InitNodeForTest(node, nlohmann::json::object(), nullptr));
}

TEST(NodeBaseContractsTest, InitAndProcessExceptionSafety) {
  ExceptionThrowingNode fail_init_node(true);
  SessionContext session_ctx;
  EXPECT_FALSE(
      InitNodeForTest(fail_init_node, nlohmann::json::object(), &session_ctx));

  ExceptionThrowingNode fail_proc_node(false);
  EXPECT_TRUE(
      InitNodeForTest(fail_proc_node, nlohmann::json::object(), &session_ctx));

  AlgContext ctx;
  int ret = fail_proc_node.Process(&ctx);
  EXPECT_EQ(ret, static_cast<int>(NodeRuntimeCode::kUnhandledException));
  EXPECT_EQ(ctx.GetErrorCode(),
            static_cast<int>(NodeRuntimeCode::kUnhandledException));
  EXPECT_TRUE(ctx.GetErrorMessage().find("Simulated Process failure") !=
              std::string::npos);
}

// 2. Require / Publish Helper Test
inline constexpr BlackboardKey<std::string> kTestInputKey{"test_input_key",
                                                          "string"};
inline constexpr BlackboardKey<std::string> kTestOutputKey{"test_output_key",
                                                           "string"};

class HelperTestNode : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "HelperTestNode";
  HelperTestNode() : NodeBase(kNodeType) {}

 protected:
  int ProcessNode(AlgContext& ctx) override {
    const auto* in_val = Require(ctx, kTestInputKey, -9901, "test semantics");
    if (!in_val) return -9901;
    Publish(ctx, kTestOutputKey, *in_val + "_processed");
    return 0;
  }
};

TEST(NodeBaseContractsTest, RequireAndPublishHelpers) {
  HelperTestNode node;
  SessionContext session_ctx;
  ASSERT_TRUE(InitNodeForTest(node, nlohmann::json::object(), &session_ctx));

  // Missing input key
  {
    AlgContext ctx;
    int ret = node.Process(&ctx);
    EXPECT_EQ(ret, -9901);
    EXPECT_EQ(ctx.GetErrorCode(), -9901);
    EXPECT_TRUE(ctx.GetErrorMessage().find("missing required input key") !=
                std::string::npos);
  }

  // Success path
  {
    AlgContext ctx;
    ctx.Publish(kTestInputKey, std::string("hello"));
    int ret = node.Process(&ctx);
    EXPECT_EQ(ret, 0);
    const auto* out_val = ctx.Read(kTestOutputKey);
    ASSERT_NE(out_val, nullptr);
    EXPECT_EQ(*out_val, "hello_processed");

    // A second producer invocation in the same request cannot silently
    // overwrite the already published output.
    EXPECT_EQ(node.Process(&ctx),
              static_cast<int>(NodeRuntimeCode::kUnhandledException));
    EXPECT_EQ(*out_val, "hello_processed");
    EXPECT_TRUE(ctx.GetErrorMessage().find("Duplicate output publication") !=
                std::string::npos);
  }

  // Existing key with an incompatible runtime type.
  {
    AlgContext ctx;
    ctx.Publish(std::string(kTestInputKey.name), 42);
    int ret = node.Process(&ctx);
    EXPECT_EQ(ret, -9901);
    EXPECT_EQ(ctx.GetErrorCode(), -9901);
    EXPECT_TRUE(ctx.GetErrorMessage().find("type mismatch") !=
                std::string::npos);
    EXPECT_TRUE(ctx.GetErrorMessage().find(kTestInputKey.type_id) !=
                std::string::npos);
  }
}

class BoundPortProbeNode : public NodeBase {
 public:
  BoundPortProbeNode() : NodeBase("BoundPortProbeNode"), input_("input") {}

 protected:
  bool InitNode(const NodeInitContext& init_ctx, const nlohmann::json&,
                SessionContext&) override {
    BindPort(init_ctx, input_);
    return true;
  }
  int ProcessNode(AlgContext&) override { return 0; }

 private:
  BoundInput<std::string> input_;
};

TEST(NodeBaseContractsTest, BindingRejectsDefinitionRuntimeTypeDrift) {
  ValidatedNodePlan plan;
  plan.normalized_config = nlohmann::json::object();
  plan.ports.push_back({"input", "actual_input", "integer", "1:1", "preserve",
                        "request", PortDirection::kInput});
  SessionContext session_ctx;
  NodeInitContext init_ctx;
  init_ctx.plan = &plan;
  init_ctx.session_ctx = &session_ctx;

  BoundPortProbeNode node;
  EXPECT_FALSE(node.Init(init_ctx));
}

// 3. Mock Model Engine for ModelBoundNode and TraceableUnaryInferenceNode
class MockAsrModel : public IAsrModel {
 public:
  size_t GetMaxBatchSize() const noexcept override { return 16; }
  const std::string& ModelType() const noexcept override {
    static const std::string t = "mock_asr";
    return t;
  }
  const std::string& Capability() const noexcept override {
    static const std::string capability = "asr";
    return capability;
  }
  InferenceConcurrency Concurrency() const noexcept override {
    return InferenceConcurrency::kConcurrent;
  }
  int Transcribe(const AudioPcmBatch& inputs,
                 TextBatch* outputs) noexcept override {
    if (!outputs) return -1;
    ++infer_calls_;
    outputs->clear();
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

  int infer_calls_ = 0;
  bool return_wrong_count_ = false;
  bool corrupt_provenance_ = false;
};

inline constexpr BlackboardKey<AudioPcmBatch> kTestAudioInputs{
    "test_audio_inputs", "traceable<pcm>[]"};
inline constexpr BlackboardKey<TextBatch> kTestTranscripts{
    "test_transcripts", "traceable<string>[]"};

class MockTraceableAsrNode
    : public TraceableUnaryInferenceNode<IAsrModel, AudioPcmPayload,
                                         std::string> {
 public:
  inline static constexpr char kNodeType[] = "MockTraceableAsrNode";
  MockTraceableAsrNode()
      : TraceableUnaryInferenceNode(kNodeType, kTestAudioInputs,
                                    kTestTranscripts, -6201, -6202, -6203) {}

 protected:
  int InferBatch(const InputBatch& input, OutputBatch* output) override {
    return model()->Transcribe(input, output);
  }
};

TEST(NodeBaseContractsTest, TraceableUnaryInferenceNodeWorkflow) {
  NodeDefinition definition;
  definition.node_type = MockTraceableAsrNode::kNodeType;
  definition.model_capability = "asr";
  definition.model_config_field = "bind_model";
  definition.config_fields = {ConfigFieldDefinition{
      "bind_model", ConfigValueKind::kString, false, "test_asr_model"}};
  ASSERT_TRUE(PipelineCatalog::RegisterNodeDefinition(definition));

  SessionContext session_ctx;
  auto model = std::make_shared<MockAsrModel>();
  session_ctx.GetModelManager().RegisterModel("test_asr_model", model,
                                              "test-v1");

  MockTraceableAsrNode node;
  ASSERT_TRUE(InitNodeForTest(node, nlohmann::json::object(), &session_ctx));

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

  AudioPcmBatch invalid_audios;
  invalid_audios.emplace_back(7, 0, AudioPcmPayload{});
  invalid_audios.emplace_back(7, 1, AudioPcmPayload{});

  model->return_wrong_count_ = true;
  AlgContext count_ctx;
  count_ctx.Publish(kTestAudioInputs, invalid_audios);
  EXPECT_EQ(node.Process(&count_ctx), -6202);

  model->return_wrong_count_ = false;
  model->corrupt_provenance_ = true;
  AlgContext provenance_ctx;
  provenance_ctx.Publish(kTestAudioInputs, invalid_audios);
  EXPECT_EQ(node.Process(&provenance_ctx), -6203);
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

}  // namespace alg_framework
