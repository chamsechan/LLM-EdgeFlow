#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "contracts/json_pointer.h"

namespace llm_edgeflow {

/**
 * @brief `io` 中的一项：宿主结构体 type、业务 name 和可选的参数覆盖
 */
struct IoItemSpec {
  std::string type;
  std::string name;
  nlohmann::json params = nlohmann::json::object();
};

/**
 * @brief 根层 `io`：输入项和输出项，顺序与文档相同
 */
struct IoSpec {
  std::vector<IoItemSpec> input;
  std::vector<IoItemSpec> output;
};

/**
 * @brief Pipeline 文档拆分结果
 */
struct PipelineDocumentSplit {
  IoSpec io;
  nlohmann::json core_json;  // 去掉 io 后交给 Core 的文档
};

/**
 * @brief 拆分并严格校验 Pipeline JSON 中的 io 与其余 Core 结构
 *
 * - 根必须是对象，且只允许 Core 的根字段加上 io；
 * - io 必须为对象，只含必填数组 input 和 output，各至少一项；
 * - 每一项是对象，只允许 type、name、params；type 和 name 为非空字符串，
 *   params 为对象，可省略；
 * - core_json 为移除 io 后的副本，保留所有其他根字段供 Core 校验。
 * 结构错误的 out_error_path 指向出错位置，如 /io/input/0/name。
 */
bool SplitPipelineDocument(const nlohmann::json& root,
                           PipelineDocumentSplit* out_split,
                           std::string* out_error,
                           std::string* out_error_path = nullptr);

}  // namespace llm_edgeflow
