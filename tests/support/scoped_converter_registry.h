#pragma once

#include <vector>

#include "adapter/io_converter_registry.h"

namespace llm_edgeflow::test_support {

// 保存全局 converter 登记，析构时还原。测试要临时登记、清空或替换 converter
// 时使用，避免污染同一进程里之后的 Init 审计。
class ScopedConverterRegistry {
 public:
  ScopedConverterRegistry()
      : inputs_(IoConverterRegistry::Instance().AllInputConverters()),
        outputs_(IoConverterRegistry::Instance().AllOutputConverters()) {}
  ScopedConverterRegistry(const ScopedConverterRegistry&) = delete;
  ScopedConverterRegistry& operator=(const ScopedConverterRegistry&) = delete;

  ~ScopedConverterRegistry() {
    auto& registry = IoConverterRegistry::Instance();
    registry.ClearForTesting();
    for (const auto& input : inputs_) registry.RegisterInputConverter(input);
    for (const auto& output : outputs_)
      registry.RegisterOutputConverter(output);
  }

 private:
  std::vector<InputConverterDefinition> inputs_;
  std::vector<OutputConverterDefinition> outputs_;
};

}  // namespace llm_edgeflow::test_support
