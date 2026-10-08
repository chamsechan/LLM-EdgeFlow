#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include "contracts/inference_payloads.h"
#include "contracts/parameters.h"

namespace llm_edgeflow {

// LLM Node 共享的生成参数。默认值只在 GenerateOptions 中写一次。
// 只有生成参数的 Node 直接使用；还有自有参数时用
// Parameters<Params>({...}).Include(&Params::generation,
// GenerateParameters())。
inline Parameters<GenerateOptions> GenerateParameters() {
  const GenerateOptions defaults;
  Parameters<GenerateOptions> params({
      Field("temperature", &GenerateOptions::temperature)
          .Default(defaults.temperature)
          .Range(0.0, 2.0)
          .Description(
              "生成采样温度；0 用于贪心生成，具体采样由绑定模型执行。"),
      Field("max_tokens", &GenerateOptions::max_tokens)
          .Default(defaults.max_tokens)
          .Range(1.0, 32768.0)
          .Description("每条输入最多生成的 token "
                       "数，不包含输入提示词；还受模型上下文容量"
                       "限制。"),
      Field("top_k", &GenerateOptions::top_k)
          .Default(defaults.top_k)
          .Range(0.0, static_cast<double>(std::numeric_limits<int32_t>::max()))
          .Description(
              "采样时保留的候选 token 数；0 表示不按数量截断，区别于检索返回条"
              "数。"),
      Field("top_p", &GenerateOptions::top_p)
          .Default(defaults.top_p)
          .Range(1.0e-9, 1.0)
          .Description("核采样的累计概率阈值；1 表示不按累计概率截断。"),
      Field("repetition_penalty", &GenerateOptions::repetition_penalty)
          .Default(defaults.repetition_penalty)
          .Range(1.0e-9, 100.0)
          .Description(
              "已出现 token 的重复惩罚系数；1 不调整，大于 1 抑制重复。"),
      Field("stop_words", &GenerateOptions::stop_words)
          .Default(defaults.stop_words)
          .Description("生成停止文本数组，例如 [\"结束\", "
                       "\"<END>\"]；命中后输出不包含停止"
                       "文本。"),
  });
  params.Validate([](const GenerateOptions& options, std::string* error) {
    for (const auto& word : options.stop_words) {
      if (word.empty()) {
        if (error) *error = "stop_words must contain non-empty strings";
        return false;
      }
    }
    return true;
  });
  return params;
}

}  // namespace llm_edgeflow
