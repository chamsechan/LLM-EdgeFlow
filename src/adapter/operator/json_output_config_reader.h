#pragma once

#include "adapter/operator_output_config.h"
#include "nlohmann/json.hpp"

namespace llm_edgeflow {

// 仅在解析器 Create 期间借用单个输出对象。返回的字符串自有存储，
// 该文档不会进入输出池。
class JsonOutputConfigReader final : public OutputConfigReader {
 public:
  explicit JsonOutputConfigReader(const nlohmann::json& config)
      : config_(config) {}
  JsonOutputConfigReader(nlohmann::json&&) = delete;

  bool Read(OutputConfigField field, std::string* text,
            std::string* error) const noexcept override;

 private:
  const nlohmann::json& config_;
};

}  // namespace llm_edgeflow
