#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adapter/io_converter.h"

namespace llm_edgeflow {

// 仅传递失败所在阶段，供协议壳沿用原有错误映射；错误码中立化后置。
enum class RuntimeFailureStage { kNone, kPreparation, kExecution };

struct RuntimeCreateOptions {
  std::string model_root;
  std::string config_file;
  std::string compute_platform;
  int device_id = 0;
  uint32_t output_pool_depth = 0;  // 为零时使用框架默认值。
};

struct ControlRequest {
  int command = 0;
  std::string json;
};

// 视图仅在同步调用期间借用载体。binding 回调负责校验和复制；
// Converter 接收拥有数据所有权的中立值，不接触具体平台布局。
struct RuntimeInputBatch {
  size_t count = 0;
  std::vector<ExternalInputBatchView> views;
};

struct RuntimeOutputSlot {
  size_t output_index = 0;  // 已验证输出选择中的索引。
  std::shared_ptr<void> value;
};

// 调用前，每行声明所选输出，值必须为空。处理成功后统一发布所有租约，
// 失败则保持批次不变。缺省可选槽不会删除所在行。关闭运行时前须释放输出值；
// 租约不会延长运行时已分配内存块的生命周期。
struct RuntimeOutputBatch {
  std::vector<std::vector<RuntimeOutputSlot>> rows;
};

}  // namespace llm_edgeflow
