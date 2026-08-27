# 🛠️ LLM-EdgeFlow 开发者扩展指南 (Developer Guide)

本文档为开发人员扩展 `LLM-EdgeFlow` 提供保姆级、可直接复制的代码模板与规范手册。

---

## 4 层扩展速查表 (Quick Reference)

| 架构层级 | 新增什么？ | 核心修改文件 | 关键宏 / 核心类 |
| :--- | :--- | :--- | :--- |
| **Layer 1: C ABI 适配层** | 新增业务枚举、输入/输出纯 C 结构体与专属适配器 | `include/company_alg_interface.h`<br>`src/adapter/adapters/<biz>_adapter.cpp` | `CompanyAlgBizType`<br>`IBusinessAdapter`<br>`REGISTER_BUSINESS_ADAPTER` |
| **Layer 2: 核心编排层** | 扩展动态黑板、会话模型管理与全局资源 | `include/core/alg_context.h`<br>`include/core/session_context.h` | `AlgContext::Set<T>()`<br>`SessionContext::SetResource()` |
| **Layer 3: 业务算子池** | 新增前处理/后处理/推理/规则算子 | `src/business/<biz_name>/*.cpp`<br>`src/common_nodes/*.cpp` | `NodeBase`<br>`REGISTER_NODE_WITH_DEFINITION(NodeName, def)` |
| **Layer 4: 异构引擎层** | 接入新芯片或推理后端 (如 Ascend/RKNN/TensorRT) | `include/engine/engine_interface.h`<br>`src/engine/<backend>/*_engine.cpp` | `IModelEngine`<br>`REGISTER_ENGINE_WITH_DEFINITION(Cls, def)`<br>`FixedBatchExecutor` |

---

## 1. Layer 1: 如何新增一个业务的 C ABI 接口与专属 Adapter

> ⚠️ **平台治理红线**：普通业务接入严禁修改中心分发文件 `src/adapter/company_c_adapter.cpp`，必须编写业务专属 Adapter 类并注册。

### 平台镜像结构与输出池方向（Proposed）

> [RFC-0009](rfcs/0009-company-string-and-slot-map-struct-binding.md) 正在定义公司
> C++ Platform Operator 的长期数据边界，本节仅用于提前锁定扩展方向，尚不是当前
> 实现指南。

后续新增平台数据类型时必须区分两类协议：

| 协议 | 使用位置 | 扩展方式 |
| --- | --- | --- |
| 纯 C ABI DTO | `Alg_Process` 与 BusinessAdapter | 继续按本章现有步骤声明并注册 |
| 平台镜像 C 结构 | C++ `NamedIoBatch` Process 边界 | 注册值类型、业务槽位桥接、双向转换和输出池操作 |

平台扩展必须分两步：先在 `PlatformValueTypeRegistry` 中建立“规范后缀 -> 外部 C
类型 + validate/allocate/reset/destroy”的唯一绑定，再通过
`PlatformBusinessBridgeDescriptor` 声明业务、方向和一个或多个逻辑槽位，并完成与
现有内部 DTO 的逐字段转换。不要把 `.frame` 或 `.string` 直接绑定成整套业务 DTO，
也不要恢复“一帧恰好一个输入/输出组”的限制。

`CompanyString` 只用于无嵌入 NUL 的文本，二进制内容使用 `CompanyBuffer`。平台镜像
结构不得替换或渗透内部 DTO。输入转换只读取 `.get()` 指针并复制数据值；输出由
Create 期固定池分配，Process 只向空输出槽位提交池化 shared_ptr。输出 deleter 只
持有池状态的 weak lifetime token，Destroy 后不得访问输出数据。任何需要修改
Blackboard、Node 或 Engine 才能识别平台结构的方案均违反分层要求。

RFC-0009 还会改变 C++ `CreateParam` 的二进制布局，因此 Platform 包必须提升到
SOVERSION 3 并与公司调用方原子升级；这不改变纯 C ABI V2。若发布系统不能用 SONAME
隔离旧调用方，必须提供版本化 Getter 和参数类型，不能复用旧二进制入口。

Platform v3 的 Create 和配置预检都使用部署根 `model_path` 加相对
`cfg_file_name`。每份 v3 `.conf` 必须提供 `data.mem_que`，由 Resolver 归一化规范
输出后缀、`meta_num`、metadata type 和字段容量；业务桥接与输出池不得直接读取原始
JSON 键。

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

## 2. Layer 2: 核心编排层与静态校验计划 (Pipeline & ValidatedPipelinePlan)

Layer 2 负责请求黑板生命周期与 DAG 管线单趟构建：
- **`ValidatedPipelinePlan`**：`PipelineValidator::ValidateAndPlan()` 单趟静态校验与 DAG 拓扑排序输出的不可变执行计划，`Pipeline::BuildInternal()` 直接消费该计划，杜绝运行时二次解析或隐式 DAG 计算。
- **`BlackboardKey<T>`**：强类型黑板键，各算子间通过 `Require` 与 `Publish` 交换数据，杜绝无类型内存乱序。

---

## 3. Layer 3: 如何新增一个业务算子 (NodeBase)

```cpp
// src/business/my_biz/my_custom_node.cpp
#include "core/node_registry.h"
#include "engine/engine_interface.h"
#include "nodes/model_bound_node.h"
#include "nodes/node_support.h"

namespace alg_framework {

inline constexpr BlackboardKey<std::string> kQueryText{"query_text", "string"};
inline constexpr BlackboardKey<std::string> kCustomResultJson{"custom_result_json", "string"};

class MyCustomNode final : public ModelBoundNode<ILlmEngine> {
 public:
  inline static constexpr char kNodeType[] = "MyCustomNode";

  MyCustomNode() : ModelBoundNode<ILlmEngine>(kNodeType, "my_model_v1") {}

 protected:
  bool InitNode(const nlohmann::json& config, SessionContext& session_ctx) override {
    threshold_ = config.value("threshold", 0.85f);
    return true;
  }

  int ProcessNode(AlgContext& req_ctx) override {
    const auto* query = Require(req_ctx, kQueryText, -9001);
    if (!query) return -9001;

    // 按需调用已绑定的 Layer 4 模型引擎
    if (engine()) {
      std::string llm_out;
      ILlmEngine::GenerateOption opt;
      engine()->Generate(*query, opt, &llm_out);
    }

    Publish(req_ctx, kCustomResultJson, std::string("{\"verdict\":\"PASS\"}"));
    return 0;
  }

  int ControlNode(int cmd, const std::string& json_param) override {
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

 private:
  float threshold_ = 0.85f;
};

NodeDefinition MakeMyCustomNodeDefinition() {
  NodeDefinition def;
  def.node_type = MyCustomNode::kNodeType;
  def.category = "business";
  def.description = "Custom business inference node";
  def.inputs = {RequiredInput(kQueryText)};
  def.outputs = {Output(kCustomResultJson)};
  def.config_fields = {
      ConfigFieldDefinition{"bind_model", ConfigValueKind::kString, false, "my_model_v1"},
      ConfigFieldDefinition{"threshold", ConfigValueKind::kNumber, false, 0.85, 0.0, 1.0}};
  def.model_capability = "llm";
  def.model_config_field = "bind_model";
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(MyCustomNode, MakeMyCustomNodeDefinition());

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

EngineDefinition MakeMyBackendLlmEngineDefinition() {
  EngineDefinition def;
  def.engine_type = "my_backend_llm";
  def.capability = "llm";
  def.hardware_backend = "custom_npu";
  def.description = "Custom backend LLM engine";
  def.config_fields = {
      ConfigFieldDefinition{"max_batch_size", ConfigValueKind::kInteger, false, 4, 1.0, 64.0}};
  return def;
}

REGISTER_ENGINE_WITH_DEFINITION(MyBackendLlmEngine, MakeMyBackendLlmEngineDefinition());

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
