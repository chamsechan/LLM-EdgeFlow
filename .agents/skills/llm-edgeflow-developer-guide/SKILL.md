---
name: llm-edgeflow-developer-guide
description: >-
  Comprehensive developer manual and step-by-step cookbook for extending LLM-EdgeFlow across all 4 architectural layers.
  Use when adding new C ABI business modalities (Layer 1), pipeline blackboard features (Layer 2),
  business/common nodes (Layer 3), or new hardware inference engines (Layer 4).
---

# LLM-EdgeFlow 4-Tier Developer Manual & Extension Guide

This guide provides a standardized, copy-pasteable cookbook for developers extending the `LLM-EdgeFlow` framework.

---

## 4-Tier Extension Cheat Sheet

| Layer | What to Add? | Target Files | Key Macro / Mechanism |
| :--- | :--- | :--- | :--- |
| **Layer 1: C ABI** | 新业务类型、输入/输出 C 结构体与专属适配器 | `include/company_alg_interface.h`<br>`src/adapter/adapters/<biz>_adapter.cpp` | `ALG_BIZ_TYPE_*`<br>`IBusinessAdapter`<br>`REGISTER_BUSINESS_ADAPTER` |
| **Layer 2: Core** | 核心调度、黑板扩展、会话共享资源 | `include/core/alg_context.h`<br>`include/core/session_context.h` | `AlgContext::Set<T>()`<br>`SessionContext::SetResource()` |
| **Layer 3: Nodes** | 业务专属算子 / 通用公共算子 | `src/business/<biz_name>/*.cpp`<br>`src/common_nodes/*.cpp` | `INode`<br>`REGISTER_NODE(NodeName)` |
| **Layer 4: Engine** | 新硬件芯片推理后端 (如 Ascend/RKNN/TensorRT) | `include/engine/engine_interface.h`<br>`src/engine/<backend>/*_engine.cpp` | `IModelEngine`<br>`REGISTER_ENGINE(Name, Cls)`<br>`FixedBatchExecutor` |

---

## 🛠️ Layer 1: 新增业务 C ABI 接口与数据结构

### 步骤 1.1: 声明 C ABI 业务枚举与数据结构 (`include/company_alg_interface.h`)
```c
// 1. 在枚举中追加新业务类型
typedef enum {
    // ... 已有业务
    ALG_BIZ_TYPE_CUSTOM_TASK = 8,  // 你的新业务
} CompanyAlgBizType;

// 2. 声明纯 C 输入结构体
typedef struct {
    uint64_t request_id;
    const char* query_text;
    const float* feature_vector;
    int feature_dim;
} CompanyCustomInputStruct;

// 3. 声明纯 C 输出结构体 (零堆悬挂指针，固定数组或只读指针)
typedef struct {
    uint64_t request_id;
    int status_code;
    char result_json[4096];
} CompanyCustomOutputStruct;
```

### 步骤 1.2: 编写业务专属适配器 (`src/adapter/adapters/<biz>_adapter.cpp`)
实现 `IBusinessAdapter` 并通过宏自动注册，无需修改中心分发文件：
```cpp
#include "adapter/business_adapter_registry.h"
#include "company_alg_interface.h"
#include "business/custom_task/custom_task_dto.h"

namespace alg_framework {

class CustomTaskAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override { return ALG_BIZ_TYPE_CUSTOM_TASK; }
  const char* BizName() const override { return "CustomTask"; }

  const AdapterDescriptor& GetDescriptor() const override {
    static AdapterDescriptor desc{
        ALG_BIZ_TYPE_CUSTOM_TASK,
        "CustomTask",
        "2.0.0",
        "CompanyCustomInputStruct",
        "CompanyCustomOutputStruct",
        64,
        OwnershipPolicy::kCopyIn,
        ThreadModel::kStatelessThreadSafe,
        OutputCardinality::kOneToOne,
        {"custom_pipeline_v1"}};
    return desc;
  }

  int Unpack(const void** inputs, int num_inputs, AlgContext* ctx,
             AdapterStatus* out_status = nullptr) const override {
    int valid_ret = AdapterValidationHelper::ValidateBatchInputs(
        inputs, num_inputs, GetDescriptor().max_batch_size, BizName());
    if (valid_ret != 0 || !ctx) {
      if (out_status) {
        *out_status = AdapterStatus::InvalidInput(
            "Batch envelope validation failed", "inputs", -1, BizName());
      }
      return COMPANY_ALG_ERR_INVALID_INPUT;
    }

    std::vector<uint64_t> req_ids;
    std::vector<std::string> queries;
    req_ids.reserve(num_inputs);
    queries.reserve(num_inputs);

    for (int i = 0; i < num_inputs; ++i) {
      auto* in = static_cast<const CompanyCustomInputStruct*>(inputs[i]);
      if (!AdapterValidationHelper::RequireNotNull("inputs[i]", in, i, BizName(),
                                                   out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }
      if (!AdapterValidationHelper::RequireBoundedString(
              "inputs[i].query_text", in->query_text, 64 * 1024, i, BizName(),
              out_status)) {
        return COMPANY_ALG_ERR_INVALID_INPUT;
      }

      req_ids.push_back(in->request_id);
      queries.push_back(in->query_text);
    }
    ctx->Set("raw_request_ids", std::move(req_ids));
    ctx->Set("query_texts", std::move(queries));
    return COMPANY_ALG_SUCCESS;
  }

  int Pack(AlgContext* ctx, void** outputs, int* num_outputs,
           AdapterStatus* out_status = nullptr) const override {
    if (!ctx) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
    auto* res = ctx->Get<std::vector<CustomTaskResult>>("custom_task_outputs");
    if (!res) return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;

    int count = static_cast<int>(res->size());
    int valid_ret = AdapterValidationHelper::ValidateBatchOutputs(
        outputs, num_outputs, count, BizName());
    if (valid_ret != 0) return valid_ret;

    for (int i = 0; i < count; ++i) {
      auto* out = static_cast<CompanyCustomOutputStruct*>(outputs[i]);
      out->request_id = (*res)[i].request_id;
      out->status_code = (*res)[i].status_code;

      if (!AdapterValidationHelper::CheckedStringCopy(
              out->result_json, sizeof(out->result_json),
              (*res)[i].result_json.c_str(), "outputs[i].result_json", i,
              BizName(), out_status)) {
        return COMPANY_ALG_ERR_BUFFER_TOO_SMALL;
      }
    }
    *num_outputs = count;
    return COMPANY_ALG_SUCCESS;
  }
};

REGISTER_BUSINESS_ADAPTER(CustomTaskAdapter);

}  // namespace alg_framework
```

---

## 🛠️ Layer 3: 新增业务算子或公共算子 (INode)

### 步骤 3.1: 编写算子类 (`src/business/<biz_name>/my_node.cpp` 或 `src/common_nodes/`)
```cpp
#include "core/node_base.h"
#include "core/node_registry.h"
#include "engine/engine_interface.h"

namespace alg_framework {

class MyCustomNode : public INode {
 public:
  bool Init(const nlohmann::json& config, SessionContext* session_ctx) override {
    session_ctx_ = session_ctx;
    model_id_ = config.value("model_id", "my_model_v1");
    threshold_ = config.value("threshold", 0.85f);
    return true;
  }

  int Process(AlgContext* req_ctx) override {
    // 1. 从黑板读取输入
    auto* query = req_ctx->Get<std::string>("query_text");
    if (!query) return -1;

    // 2. 按需调用 Layer 4 引擎 (如需模型推理)
    auto engine = session_ctx_->GetModelManager().GetModel<ILlmEngine>(model_id_);
    if (engine) {
        std::string llm_out;
        ILlmEngine::GenerateOption opt;
        engine->Generate(*query, opt, &llm_out);
        req_ctx->Set("llm_raw_out", llm_out);
    }

    // 3. 写入后续算子需要的结果
    req_ctx->Set("custom_result_json", std::string("{\"status\":\"OK\"}"));
    return 0;
  }

  int Control(int cmd, const nlohmann::json& param) override {
    // 处理动态热更新 (如在线下发阈值或词表)
    if (cmd == 1 && param.contains("threshold")) {
        threshold_ = param["threshold"].get<float>();
    }
    return 0;
  }

  const std::string& Name() const override {
    static const std::string name = "MyCustomNode";
    return name;
  }

 private:
  SessionContext* session_ctx_ = nullptr;
  std::string model_id_;
  float threshold_ = 0.85f;
};

// 关键：注册算子到反射工厂
REGISTER_NODE(MyCustomNode);

} // namespace alg_framework
```

### 步骤 3.2: 在 `CMakeLists.txt` 中添加源码编译
```cmake
add_library(alg_sdk SHARED
    # ...
    src/business/my_biz/my_custom_node.cpp
)
```

---

## 🛠️ Layer 4: 新增芯片后端与推理引擎 (Engine)

### 步骤 4.1: 实现引擎类 (`src/engine/<backend>/my_engine.h` & `.cpp`)
必须继承能力纯虚接口（如 `ILlmEngine`, `IEmbeddingEngine`, `IRerankEngine`, `IOcrEngine`, `IAudioAsrEngine`）：

```cpp
#pragma once
#include "engine/engine_interface.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

class TensorRtLlmEngine : public ILlmEngine {
 public:
  bool Load(const std::string& model_path, const nlohmann::json& config) override {
    max_batch_size_ = config.value("max_batch_size", 4);
    // 初始化芯片 SDK / 加载 TensorRT Engine
    return true;
  }

  int Generate(const std::string& prompt, const GenerateOption& opt, std::string* output) override {
    // 单条推理实现
    return 0;
  }

  int InferTraceableBatch(const std::vector<TraceableItem<std::string>>& prompts,
                          const GenerateOption& opt,
                          std::vector<TraceableItem<std::string>>* outputs) override {
    // 关键：使用 FixedBatchExecutor 自动处理硬件定长分批、Dummy Pad 与溯源
    std::string dummy_pad = "<PAD>";
    return FixedBatchExecutor::Execute<std::string, std::string>(
        prompts, max_batch_size_, dummy_pad,
        [this, &opt](const std::vector<std::string>& batch_in, std::vector<std::string>* batch_out) {
          // 底层硬件前向前处理与推理
          batch_out->resize(batch_in.size());
          for (size_t i = 0; i < batch_in.size(); ++i) {
            (*batch_out)[i] = "Engine Output for: " + batch_in[i];
          }
          return 0;
        },
        outputs);
  }

  size_t GetMaxBatchSize() const override { return max_batch_size_; }
  const std::string& EngineType() const override {
    static const std::string type = "tensorrt_llm";
    return type;
  }

 private:
  size_t max_batch_size_ = 4;
};

} // namespace alg_framework
```

### 步骤 4.2: 注册引擎到工厂 (`src/engine/<backend>/my_engine.cpp`)
```cpp
#include "engine/engine_registry.h"
#include "src/engine/tensorrt/tensorrt_llm_engine.h"

namespace alg_framework {
REGISTER_ENGINE("tensorrt_llm", TensorRtLlmEngine);
}
```

---

## 📋 业务编排、测试与上线全流程 Check List

1. **新建配置文件 (`configs/pipeline_custom.json`)**：
   ```json
   {
     "business_name": "custom_business",
     "models": [
       { "model_id": "my_model", "model_path": "./models/model.bin", "engine_type": "tensorrt_llm" }
     ],
     "pipeline": [
       { "node_type": "MyCustomPreNode" },
       { "node_type": "MyCustomNode", "model_id": "my_model" },
       { "node_type": "MyCustomPostNode" }
     ]
   }
   ```
2. **编写 Google Test 单元测试 (`tests/test_custom.cpp`) 并注册到 CTest**：
   - 编写测试用例 `TEST_F(MySuite, BasicFlow)`，验证空指针安全、DAG 正确性与结果对齐；
   - **强制在 `CMakeLists.txt` 中注册 CTest 目标**：
     ```cmake
     add_executable(test_custom tests/test_custom.cpp)
     target_link_libraries(test_custom PRIVATE alg_sdk GTest::gtest GTest::gtest_main)
     add_test(NAME CustomTest COMMAND test_custom)
     ```
3. **格式化与全量回归**：
   ```bash
   ./scripts/format.sh
   cd build && ctest --output-on-failure
   ./scripts/run_all_tests.sh
   ```
4. **Git 分支合并上传**：
   ```bash
   ./scripts/git_branch_upload.sh "feat(my_biz): add custom business and nodes" "feat"
   ```
