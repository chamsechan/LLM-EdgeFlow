#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapter/deployment_preparation.h"
#include "adapter/operator/operator_value_type_registry.h"
#include "adapter/runtime_types.h"
#include "edgeflow/operator/interface.h"

namespace llm_edgeflow {

struct FrameOutputBinding {
  std::string key;
  size_t output_index = 0;
};

/**
 * @brief 按已选 Converter 的 type 路由宿主输入，校验载体及 service_type
 * 后组成批视图。
 * 保留原始行号与输入引用；拒绝重复、未知或缺失必需槽位，失败结果不可用于执行。
 */
int ValidateAndExtractOperatorInputs(
    const llm_edgeflow::operator_api::NamedIoBatch& inputs,
    const std::vector<SelectedInput>& selected, const InputLimits& limits,
    std::vector<ExternalInputBatchView>* out_views, std::string* error);

/**
 * @brief 将宿主预置的空输出槽位映射到已选 Converter，保留宿主 key
 * 和请求行位置。
 * 拒绝非空、重复及未知槽位，并检查必需输出；此处不分配或发布输出对象。
 */
int ResolveOperatorOutputs(
    const llm_edgeflow::operator_api::NamedIoBatch& outputs,
    const std::vector<SelectedOutput>& selected,
    std::vector<std::vector<FrameOutputBinding>>* frame_bindings,
    std::string* error);

void PublishOperatorOutputs(
    RuntimeOutputBatch* staged,
    const std::vector<std::vector<FrameOutputBinding>>& bindings,
    const std::vector<SelectedOutput>& selected,
    llm_edgeflow::operator_api::NamedIoBatch* outputs);

}  // namespace llm_edgeflow
