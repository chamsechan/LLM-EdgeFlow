#pragma once

#include <string>

namespace llm_edgeflow {

// 选择某个输出配置中的字段。这些是框架字段，
// 区别于 kParameters 内各结构自有的枚举。
enum class OutputConfigField {
  kAllocator,
  kParameters,
  kCapacities,
  kMetadataCount,
  kMetadataTypeId
};

// 创建期的接入适配层组件，独立于 Pipeline 执行。JSON 实现返回序列化后的
// JSON 值 (字符串含引号)；其他配置载体可实现同样的文本边界。
// 结构分配器只接收最终的参数文本。
class OutputConfigReader {
 public:
  virtual ~OutputConfigReader() = default;
  virtual bool Read(OutputConfigField field, std::string* text,
                    std::string* error) const noexcept = 0;
};

}  // namespace llm_edgeflow
