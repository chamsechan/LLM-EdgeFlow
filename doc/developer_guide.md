# 🛠️ LLM-EdgeFlow 开发者扩展指南 (Developer Guide)

本文档为开发人员扩展 `LLM-EdgeFlow` 提供保姆级、可直接复制的代码模板与规范手册。

---

## 4 层扩展速查表 (Quick Reference)

| 架构层级 | 新增什么？ | 核心修改文件 | 关键宏 / 核心类 |
| :--- | :--- | :--- | :--- |
| **Layer 1: C ABI 适配层** | 新增业务枚举、输入/输出纯 C 结构体与专属适配器 | `include/company_alg_interface.h`<br>`src/adapter/adapters/<biz>_adapter.cpp` | `CompanyAlgBizType`<br>`IBusinessAdapter`<br>`REGISTER_BUSINESS_ADAPTER` |
| **Layer 2: 核心编排层** | 扩展动态黑板、会话模型管理与全局资源 | `include/core/alg_context.h`<br>`include/core/session_context.h` | `AlgContext::Set<T>()`<br>`SessionContext::SetResource()` |
| **Layer 3: 业务算子池** | 新增前处理/后处理/推理/规则算子 | `src/business/<biz_name>/*.cpp`<br>`src/common_nodes/*.cpp` | `INode`<br>`REGISTER_NODE(NodeName)` |
| **Layer 4: 异构引擎层** | 接入新芯片或推理后端 (如 Ascend/RKNN/TensorRT) | `include/engine/engine_interface.h`<br>`src/engine/<backend>/*_engine.cpp` | `IModelEngine`<br>`REGISTER_ENGINE(Name, Cls)`<br>`FixedBatchExecutor` |

---

## 1. Layer 1: 如何新增一个业务的 C ABI 接口与专属 Adapter

> ⚠️ **平台治理红线**：普通业务接入严禁修改中心分发文件 `src/adapter/company_c_adapter.cpp`，必须编写业务专属 Adapter 类并注册。

### 步骤 1.1: 声明 C 枚举与数据结构 (`include/company_alg_interface.h`)
```c
// 1. 追加业务类型枚举
typedef enum {
    // ...
    ALG_BIZ_TYPE_CUSTOM_TASK = 8,
} CompanyAlgBizType;

// 2. 声明输入结构体
typedef struct {
    uint64_t request_id;
    const char* query_text;
    const float* feature_vector;
    int feature_dim;
} CompanyCustomInputStruct;

// 3. 声明输出结构体 (杜绝动态内存悬挂，使用固定缓冲区)
typedef struct {
    uint64_t request_id;
    int status_code;
    char result_json[4096];
} CompanyCustomOutputStruct;
```

### 步骤 1.2: 编写业务专属适配器 (`src/adapter/adapters/custom_task_adapter.cpp`)
```cpp
#include "adapter/adapter_validation_helper.h"
#include "adapter/business_adapter_registry.h"
#include "business/custom_task/custom_task_dto.h"
#include "company_alg_interface.h"

namespace alg_framework {

class CustomTaskAdapter : public IBusinessAdapter {
 public:
  CompanyAlgBizType BizType() const override {
    return ALG_BIZ_TYPE_CUSTOM_TASK;
  }

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
      auto* out_ptr = static_cast<CompanyCustomOutputStruct*>(outputs[i]);
      out_ptr->request_id = (*res)[i].request_id;
      out_ptr->status_code = (*res)[i].status_code;

      // 截断时严格拦截返回 -4
      if (!AdapterValidationHelper::CheckedStringCopy(
              out_ptr->result_json, sizeof(out_ptr->result_json),
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

## 2. Layer 3: 如何新增一个业务算子 (INode)

```cpp
// src/business/my_biz/my_custom_node.cpp
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
    auto* query = req_ctx->Get<std::string>("query_text");
    if (!query) return -1;

    // 按需调用 Layer 4 模型
    auto engine = session_ctx_->GetModelManager().GetModel<ILlmEngine>(model_id_);
    if (engine) {
        std::string llm_out;
        ILlmEngine::GenerateOption opt;
        engine->Generate(*query, opt, &llm_out);
        req_ctx->Set("llm_raw_out", llm_out);
    }

    req_ctx->Set("custom_result_json", std::string("{\"verdict\":\"PASS\"}"));
    return 0;
  }

  int Control(int cmd, const std::string& json_param) override {
    if (cmd == 1) {
      try {
        nlohmann::json param = nlohmann::json::parse(json_param);
        if (param.contains("threshold")) {
          threshold_ = param["threshold"].get<float>();
        }
      } catch (const std::exception& e) {
        return -1;
      }
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

// 宏自动注册到反射工厂
REGISTER_NODE(MyCustomNode);

} // namespace alg_framework
```

---

## 3. Layer 4: 如何新增一个硬件推理引擎 (Engine)

```cpp
// src/engine/my_backend/my_backend_llm_engine.cpp
#include "engine/engine_interface.h"
#include "engine/engine_registry.h"
#include "engine/fixed_batch_executor.h"

namespace alg_framework {

class MyBackendLlmEngine : public ILlmEngine {
 public:
  bool Load(const std::string& model_path, const nlohmann::json& config) override {
    max_batch_size_ = config.value("max_batch_size", 4);
    // 初始化驱动硬件
    return true;
  }

  int Generate(const std::string& prompt, const GenerateOption& opt, std::string* output) override {
    *output = "Inference result for: " + prompt;
    return 0;
  }

  int InferTraceableBatch(const std::vector<TraceableItem<std::string>>& prompts,
                          const GenerateOption& opt,
                          std::vector<TraceableItem<std::string>>* outputs) override {
    // 调用定长硬件分批模板 (FixedBatchExecutor)
    return FixedBatchExecutor::Execute<std::string, std::string>(
        prompts, max_batch_size_, "<PAD>",
        [this](const std::vector<std::string>& in, std::vector<std::string>* out) {
          out->resize(in.size());
          for (size_t i = 0; i < in.size(); ++i) (*out)[i] = "Output: " + in[i];
          return 0;
        },
        outputs);
  }

  size_t GetMaxBatchSize() const override { return max_batch_size_; }
  const std::string& EngineType() const override {
    static const std::string type = "my_backend_llm";
    return type;
  }

 private:
  size_t max_batch_size_ = 4;
};

REGISTER_ENGINE("my_backend_llm", MyBackendLlmEngine);

} // namespace alg_framework
```

---

## 4. 上线、单测与 Git 分支合并规范

1. **新建配置文件**：`configs/pipeline_custom.json`
2. **编写 GTest 单元测试并注册进 CTest**：
   - 编写 `tests/test_custom.cpp` 包含 `TEST_F(MySuite, BasicFlow)`；
   - 在 `CMakeLists.txt` 中注册测试目标：
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
4. **一键分支合并上传**：
   ```bash
   ./scripts/git_branch_upload.sh "feat(custom): add new business and engine" "feat"
   ```
