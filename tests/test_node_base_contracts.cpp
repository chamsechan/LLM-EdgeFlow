#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/alg_context.h"
#include "core/blackboard_key.h"
#include "core/session_context.h"
#include "engine/engine_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_support.h"
#include "nodes/traceable_unary_inference_node.h"

namespace alg_framework {

// 1. Exception Test Node
class ExceptionThrowingNode : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "ExceptionThrowingNode";
  explicit ExceptionThrowingNode(bool throw_in_init = false)
      : NodeBase(kNodeType), throw_in_init_(throw_in_init) {}

 protected:
  bool InitNode(const nlohmann::json&, SessionContext&) override {
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
  EXPECT_FALSE(node.Init(nlohmann::json::object(), nullptr));
}

TEST(NodeBaseContractsTest, InitAndProcessExceptionSafety) {
  ExceptionThrowingNode fail_init_node(true);
  SessionContext session_ctx;
  EXPECT_FALSE(fail_init_node.Init(nlohmann::json::object(), &session_ctx));

  ExceptionThrowingNode fail_proc_node(false);
  EXPECT_TRUE(fail_proc_node.Init(nlohmann::json::object(), &session_ctx));

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
  ASSERT_TRUE(node.Init(nlohmann::json::object(), &session_ctx));

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
    ctx.Set(kTestInputKey, std::string("hello"));
    int ret = node.Process(&ctx);
    EXPECT_EQ(ret, 0);
    const auto* out_val = ctx.Get(kTestOutputKey);
    ASSERT_NE(out_val, nullptr);
    EXPECT_EQ(*out_val, "hello_processed");
  }

  // Existing key with an incompatible runtime type.
  {
    AlgContext ctx;
    ctx.Set(std::string(kTestInputKey.name), 42);
    int ret = node.Process(&ctx);
    EXPECT_EQ(ret, -9901);
    EXPECT_EQ(ctx.GetErrorCode(), -9901);
    EXPECT_TRUE(ctx.GetErrorMessage().find("type mismatch") !=
                std::string::npos);
    EXPECT_TRUE(ctx.GetErrorMessage().find(kTestInputKey.type_id) !=
                std::string::npos);
  }
}

// 3. Mock Model Engine for ModelBoundNode and TraceableUnaryInferenceNode
class MockAsrEngine : public IAudioAsrEngine {
 public:
  bool Load(const std::string&, const nlohmann::json&) override { return true; }
  size_t GetMaxBatchSize() const override { return 16; }
  const std::string& EngineType() const override {
    static const std::string t = "mock_asr";
    return t;
  }
  int InferTraceableBatch(
      const std::vector<TraceableItem<AudioPcmData>>& inputs,
      std::vector<TraceableItem<std::string>>* outputs) override {
    if (!outputs) return -1;
    outputs->clear();
    for (const auto& item : inputs) {
      outputs->emplace_back(item.req_id, item.sub_id, "mock_transcription");
    }
    return 0;
  }
};

inline constexpr BlackboardKey<
    std::vector<TraceableItem<IAudioAsrEngine::AudioPcmData>>>
    kTestAudioInputs{"test_audio_inputs", "traceable<pcm>[]"};
inline constexpr BlackboardKey<std::vector<TraceableItem<std::string>>>
    kTestTranscripts{"test_transcripts", "traceable<string>[]"};

class MockTraceableAsrNode
    : public TraceableUnaryInferenceNode<
          IAudioAsrEngine, IAudioAsrEngine::AudioPcmData, std::string> {
 public:
  inline static constexpr char kNodeType[] = "MockTraceableAsrNode";
  MockTraceableAsrNode()
      : TraceableUnaryInferenceNode(kNodeType, "test_asr_model",
                                    kTestAudioInputs, kTestTranscripts, -6201) {
  }

 protected:
  int InferBatch(const InputBatch& input, OutputBatch* output) override {
    return engine()->InferTraceableBatch(input, output);
  }
};

TEST(NodeBaseContractsTest, TraceableUnaryInferenceNodeWorkflow) {
  SessionContext session_ctx;
  session_ctx.GetModelManager().RegisterModel(
      "test_asr_model", std::make_shared<MockAsrEngine>());

  MockTraceableAsrNode node;
  ASSERT_TRUE(node.Init(nlohmann::json::object(), &session_ctx));

  AlgContext ctx;
  std::vector<TraceableItem<IAudioAsrEngine::AudioPcmData>> audios;
  audios.emplace_back(0, 0, IAudioAsrEngine::AudioPcmData{});
  ctx.Set(kTestAudioInputs, std::move(audios));

  int ret = node.Process(&ctx);
  EXPECT_EQ(ret, 0);

  const auto* results = ctx.Get(kTestTranscripts);
  ASSERT_NE(results, nullptr);
  ASSERT_EQ(results->size(), 1u);
  EXPECT_EQ((*results)[0].data, "mock_transcription");
}

}  // namespace alg_framework
