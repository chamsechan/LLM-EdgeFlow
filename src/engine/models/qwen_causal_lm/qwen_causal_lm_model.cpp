#include "engine/models/qwen_causal_lm/qwen_causal_lm_model.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "contracts/diagnostic.h"
#include "contracts/parameters.h"
#include "edgeflow/log.h"
#include "engine/fixed_batch_executor.h"
#include "engine/text/utf8.h"

namespace llm_edgeflow {

namespace {

struct Params {
  std::string system_prompt;
  bool add_bos = false;
  int64_t random_seed = -1;
};

Parameters<Params> ParamSpec() {
  return Parameters<Params>(
      {Field("system_prompt", &Params::system_prompt)
           .Default("")
           .Description(
               "Qwen ChatML 的 system 角色内容；空字符串时省略该角色。"),
       Field("add_bos", &Params::add_bos)
           .Default(false)
           .Description(
               "分词时请求添加 BOS 起始 token，须与所选权重的分词约定一致。"),
       Field("random_seed", &Params::random_seed)
           .Default(-1)
           .Range(-1, std::numeric_limits<int32_t>::max())
           .Description("-1 不显式指定随机种子；非负值结合 req_id/sub_id "
                        "派生每条输入的采样种子。")});
}

uint64_t MixSeed(uint64_t seed, uint32_t req_id, uint32_t sub_id) noexcept {
  seed ^= static_cast<uint64_t>(req_id) + 0x9e3779b97f4a7c15ULL + (seed << 6U) +
          (seed >> 2U);
  seed ^= static_cast<uint64_t>(sub_id) + 0x9e3779b97f4a7c15ULL + (seed << 6U) +
          (seed >> 2U);
  return seed;
}

}  // namespace

std::shared_ptr<IModel> QwenCausalLmModel::Create(const ModelCreateContext& ctx,
                                                  std::string* diagnostic) {
  try {
    const auto& params = ctx.Params<Params>();
    auto session =
        std::dynamic_pointer_cast<ITextGenerationSession>(ctx.backend_session);
    if (!session) {
      if (diagnostic) {
        *diagnostic =
            "Backend session does not implement the text-generation protocol";
      }
      return nullptr;
    }
    const BatchPolicy policy = session->GetBatchPolicy();
    if (policy.max_batch_size == 0 || policy.fixed_batch_size != 0) {
      if (diagnostic) {
        *diagnostic =
            "Text-generation session must expose a non-fixed batch policy";
      }
      return nullptr;
    }

    return std::make_shared<QwenCausalLmModel>(
        std::move(session), params.system_prompt, params.add_bos,
        params.random_seed);
  } catch (const std::exception& e) {
    if (diagnostic) {
      *diagnostic = std::string("Qwen model creation exception: ") + e.what();
    }
    return nullptr;
  } catch (...) {
    if (diagnostic) *diagnostic = "Unknown Qwen model creation exception";
    return nullptr;
  }
}

QwenCausalLmModel::QwenCausalLmModel(
    std::shared_ptr<ITextGenerationSession> session, std::string system_prompt,
    bool add_bos, int64_t random_seed)
    : session_(std::move(session)),
      system_prompt_(std::move(system_prompt)),
      add_bos_(add_bos),
      random_seed_(random_seed) {}

std::string QwenCausalLmModel::ApplyChatTemplate(
    const std::string& prompt) const {
  std::string formatted;
  if (!system_prompt_.empty()) {
    formatted += "<|im_start|>system\n";
    formatted += system_prompt_;
    formatted += "<|im_end|>\n";
  }
  formatted += "<|im_start|>user\n";
  formatted += prompt;
  formatted += "<|im_end|>\n<|im_start|>assistant\n";
  return formatted;
}

int QwenCausalLmModel::Generate(const TextBatch& prompts,
                                const GenerateOptions& options,
                                TextBatch* outputs,
                                std::string* diagnostic) noexcept {
  if (diagnostic) diagnostic->clear();
  if (!outputs) {
    SetDiagnosticNoexcept(diagnostic, "Model output pointer is null");
    return -1;
  }
  outputs->clear();
  if (prompts.empty()) return 0;
  if (!session_) {
    SetDiagnosticNoexcept(diagnostic, "Model session is null");
    return -1;
  }

  const BatchPolicy policy = session_->GetBatchPolicy();
  if (policy.max_batch_size == 0 || policy.fixed_batch_size != 0) {
    SetDiagnosticNoexcept(diagnostic,
                          "Text generation requires a non-fixed batch policy");
    return -1;
  }
  return FixedBatchExecutor::ExecuteItems<std::string, std::string>(
      prompts, policy,
      [this, &options, diagnostic](const TraceableItem<std::string>& prompt,
                                   std::string* output) {
        return GenerateOne(prompt, options, output, diagnostic);
      },
      outputs, diagnostic, -1);
}

int QwenCausalLmModel::GenerateOne(const TraceableItem<std::string>& prompt,
                                   const GenerateOptions& options,
                                   std::string* output,
                                   std::string* diagnostic) noexcept {
  if (!output) return -1;
  output->clear();
  if (!session_ || prompt.data.empty()) {
    SetDiagnosticNoexcept(
        diagnostic, "Text generation requires a session and non-empty prompt");
    return -1;
  }

  try {
    std::optional<uint64_t> seed;
    if (random_seed_ >= 0) {
      seed = MixSeed(static_cast<uint64_t>(random_seed_), prompt.req_id,
                     prompt.sub_id);
    }
    std::string reason;
    const int result =
        session_->Generate(ApplyChatTemplate(prompt.data), add_bos_, options,
                           seed, output, &reason);
    if (result != 0) {
      SetDiagnosticNoexcept(diagnostic, reason);
      output->clear();
      ALG_LOG_ERROR("[QwenCausalLmModel] Generate failed: %s\n",
                    reason.c_str());
      return result;
    }
    utf8::StripIncompleteSuffix(output);
    return 0;
  } catch (const std::exception& e) {
    SetDiagnosticNoexcept(diagnostic, e.what());
    ALG_LOG_ERROR("[QwenCausalLmModel] Generate exception: %s\n", e.what());
    output->clear();
    return -1;
  } catch (...) {
    SetDiagnosticNoexcept(diagnostic, "Unknown text generation exception");
    ALG_LOG_ERROR("[QwenCausalLmModel] Unknown Generate exception\n");
    output->clear();
    return -1;
  }
}

static const ModelDefinition& QwenCausalLmModelDefinition() {
  static const ModelDefinition definition = [] {
    auto definition = MakeModelDefinition<QwenCausalLmModel>();
    definition.description =
        "Qwen ChatML model using the unified text-generation protocol";
    definition.required_protocol = ExecutionProtocol::kTextGeneration;
    definition.params = ParamSpec();
    return definition;
  }();
  return definition;
}

REGISTER_MODEL_WITH_DEFINITION(QwenCausalLmModel,
                               QwenCausalLmModelDefinition());

}  // namespace llm_edgeflow
