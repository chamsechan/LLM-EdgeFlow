# 🛠️ LLM-EdgeFlow 开发者扩展指南 (Developer Guide)

本文档为开发人员扩展 `LLM-EdgeFlow` 提供保姆级、可直接复制的代码模板与规范手册。

---

## 4 层扩展速查表 (Quick Reference)

| 架构层级 | 新增什么？ | 核心修改文件 | 关键宏 / 核心类 |
| :--- | :--- | :--- | :--- |
| **Layer 1: C ABI 适配层** | 新增业务枚举、输入/输出纯 C 结构体 | `include/company_alg_interface.h`<br>`src/adapter/company_c_adapter.cpp` | `CompanyAlgBizType`<br>`noexcept` 异常安全屏障 |
| **Layer 2: 核心编排层** | 扩展动态黑板、会话模型管理与全局资源 | `include/core/alg_context.h`<br>`include/core/session_context.h` | `AlgContext::Set<T>()`<br>`SessionContext::SetResource()` |
| **Layer 3: 业务算子池** | 新增前处理/后处理/推理/规则算子 | `src/business/<biz_name>/*.cpp`<br>`src/common_nodes/*.cpp` | `INode`<br>`REGISTER_NODE(NodeName)` |
| **Layer 4: 异构引擎层** | 接入新芯片或推理后端 (如 Ascend/RKNN/TensorRT) | `include/engine/engine_interface.h`<br>`src/engine/<backend>/*_engine.cpp` | `IModelEngine`<br>`REGISTER_ENGINE(Name, Cls)`<br>`FixedBatchExecutor` |

---

## 1. Layer 1: 如何新增一个业务的 C ABI 接口

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

### 步骤 1.2: 在适配器中实现解包与打包 (`src/adapter/company_c_adapter.cpp`)
```cpp
case ALG_BIZ_TYPE_CUSTOM_TASK: {
    auto* in_struct = static_cast<const CompanyCustomInputStruct*>(inputs[i]);
    req_ctx.SetRequestId(in_struct->request_id);
    req_ctx.Set("query_text", std::string(in_struct->query_text));
    
    int ret = inst->pipeline->Execute(&req_ctx);
    if (ret != 0) return ret;
    
    auto* out_struct = static_cast<CompanyCustomOutputStruct*>(outputs[i]);
    out_struct->request_id = in_struct->request_id;
    auto* res_json = req_ctx.Get<std::string>("custom_result_json");
    if (res_json) {
        snprintf(out_struct->result_json, sizeof(out_struct->result_json), "%s", res_json->c_str());
    }
    break;
}
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

  int Control(int cmd, const nlohmann::json& param) override {
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
