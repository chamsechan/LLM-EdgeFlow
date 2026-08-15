# Alg-SDK Runtime 架构设计与分层规范文档

本文档详细描述了企业级算法交付与管线框架（Alg-SDK Runtime Pipeline Framework）的分层架构、设计模式、数据流转与多人协作规范。

---

## 1. 框架整体 4 层抽象架构

框架严格遵循职责单一与接口隔离原则，划分为 **4 个独立的抽象层次**：

```mermaid
graph TD
    classDef l1 fill:#E3F2FD,stroke:#1565C0,stroke-width:2px,color:#0D47A1;
    classDef l2 fill:#E8F5E9,stroke:#2E7D32,stroke-width:2px,color:#1B5E20;
    classDef l3 fill:#FFF3E0,stroke:#E65100,stroke-width:2px,color:#E65100;
    classDef l4 fill:#F3E5F5,stroke:#7B1FA2,stroke-width:2px,color:#4A148C;
    classDef ext fill:#ECEFF1,stroke:#37474F,stroke-width:1px,color:#263238;

    subgraph External["外部调用方 (业务APP / 车机系统 / 边缘平台)"]
        Caller["下游集成程序 / 主控服务"]
    end

    %% Level 1
    subgraph L1["Layer 1: 平台接入与 C-ABI 适配层 (Platform C-ABI Adapter Layer)"]
        C_API["公司统一标准 C ABI 接口<br>• Alg_Init / Alg_DeInit<br>• Alg_Create / Alg_Destroy<br>• Alg_Process(vector&lt;void*&gt; inputs, outputs)<br>• Alg_Control"]
        C_Adapter["company_c_adapter.cpp<br>• 句柄生命周期托管 (Handle)<br>• 异常拦截屏障 (noexcept 安全防护)<br>• 外部输入解包 / 输出结构体强转打包"]
    end

    %% Level 2
    subgraph L2["Layer 2: 管线调度与状态黑板层 (Pipeline & Multi-Level Context Layer)"]
        PipeCore["Pipeline 核心调度器 (pipeline.cpp)<br>• JSON 配置解析与自动组装<br>• 算子拓扑执行与错误熔断"]
        
        subgraph StateMgr["三级状态与注册管理器"]
            S_Ctx["SessionContext (句柄级持久状态)<br>• ModelManager 多模型池<br>• 句柄共享缓存资源"]
            R_Ctx["AlgContext (请求级瞬态黑板)<br>• std::any 类型安全擦除<br>• 自动生命周期析构 (无内存泄漏)"]
            TraceTag["TraceableItem 溯源追踪<br>• req_id (请求索引)<br>• sub_id (1对N分片索引)"]
            Factory["NodeFactory & EngineFactory<br>• REGISTER_NODE / REGISTER_ENGINE 宏"]
        end
    end

    %% Level 3
    subgraph L3["Layer 3: 业务算子与可复用节点层 (Stateful Node & Business Logic Layer)"]
        NodeBase["INode 基类抽象 (Init / Process)"]
        
        subgraph CommonNodes["全组通用算子池 (src/common_nodes/)"]
            PromptNode["PromptBuilderNode"]
            VecSearchNode["VectorSearchNode"]
            FilterNode["RuleFilterNode / SensitiveNode"]
        end
        
        subgraph BizNodes["开发者私有业务算子池 (src/business/)"]
            PreNode["DocChunkPreNode (1对N切片)"]
            RuleNode["IntentRuleNode (含节点私有词典/规则数据)"]
            PostNode["DocQaPostNode (多样本聚合对齐)"]
        end
    end

    %% Level 4
    subgraph L4["Layer 4: 多后端模型引擎与批处理调度层 (Multi-Backend Engine & Batch Layer)"]
        EngineBase["IModelEngine 基础引擎抽象"]
        LlmIntf["ILlmEngine<br>(Generate / Stream)"]
        EmbedIntf["IEmbeddingEngine<br>(BatchEncode)"]
        
        BatchExec["FixedBatchExecutor (硬件固定 Batch 调度器)<br>• 样本自动 Chunking 分块<br>• 末尾 Dummy Pad 自动补齐<br>• 推理后剥离 Pad 并保留溯源标签"]
        
        subgraph HardwareBackends["底层硬件与引擎适配器 (src/engines/)"]
            NpuEmbed["MockNpuEmbeddingEngine<br>(NPU CANN/RKNN, Batch=4)"]
            NpuLlm["MockNpuLlmEngine<br>(NPU LLM, Batch=2)"]
            GpuTrt["TensorRtLlmEngine (GPU)"]
            CpuMnn["MnnEmbeddingEngine (CPU)"]
        end
    end

    %% 连接关系
    Caller <==>|结构体指针 vector&lt;void*&gt;| C_API
    C_API --> C_Adapter
    C_Adapter -->|构造/销毁| PipeCore
    C_Adapter -->|解包/打包| R_Ctx
    PipeCore --> S_Ctx
    PipeCore --> NodeBase
    NodeBase -.-> CommonNodes
    NodeBase -.-> BizNodes
    CommonNodes & BizNodes -->|读写特征与溯源数据| R_Ctx
    CommonNodes & BizNodes -->|从 ModelManager 获取模型| S_Ctx
    CommonNodes & BizNodes -->|调用模型推理| LlmIntf & EmbedIntf
    LlmIntf & EmbedIntf --> BatchExec
    BatchExec --> HardwareBackends

    class Caller ext;
    class C_API,C_Adapter l1;
    class PipeCore,S_Ctx,R_Ctx,TraceTag,Factory l2;
    class NodeBase,CommonNodes,BizNodes,PromptNode,VecSearchNode,FilterNode,PreNode,RuleNode,PostNode l3;
    class EngineBase,LlmIntf,EmbedIntf,BatchExec,NpuEmbed,NpuLlm,GpuTrt,CpuMnn l4;
```

---

## 2. 4 层抽象职责定义

### Layer 1: 平台接入层 (Platform C-ABI Adapter)
- **代码位置**：`include/company_alg_interface.h`，`src/adapter/company_c_adapter.cpp`
- **核心职责**：
  1. 导出公司限定的标准 C 接口：`Alg_Init`, `Alg_Create`, `Alg_Process`, `Alg_Control`, `Alg_Destroy`, `Alg_DeInit`；
  2. 充当 `noexcept` 安全屏障，拦截所有 C++ 异常，防止跨动态库边界崩溃；
  3. 将外部传入的 `vector<void*>` 业务结构体解包，转入内部强类型的 `AlgContext`。

### Layer 2: 管线调度与状态黑板层 (Pipeline & State Engine)
- **代码位置**：`include/core/`，`src/core/`
- **核心职责**：
  1. **配置驱动**：解析外部 JSON 配置文件，动态初始化模型与组装算子节点序列；
  2. **三级状态管理**：
     - `SessionContext`：句柄级常驻状态，管理单句柄加载的多个模型实例（`ModelManager`）；
     - `AlgContext`：请求级瞬态黑板，使用 `std::any` 存储中间特征，用完即释放，避免内存泄漏；
     - `TraceableItem<T>`：样本溯源标签（`req_id` + `sub_id`），保证 1对N 裂变后可严格 1:1 对齐回原请求；
  3. **自注册机制**：`REGISTER_NODE` 与 `REGISTER_ENGINE`，消除硬编码。

### Layer 3: 算子节点与业务编排层 (Stateful Node & Business)
- **代码位置**：`src/common_nodes/`，`src/business/`
- **核心职责**：
  1. **算法工程师核心开发区**：继承 `INode`，实现 `Init(config, session_ctx)` 和 `Process(req_ctx)`；
  2. **有状态节点支持**：开发者可自由在类中定义私有成员变量（私有规则表、词典映射、预编译正则），随句柄常驻；
  3. **模块化复用**：通用算子（Prompt 构造、向量检索、敏感词过滤）全组共享。

### Layer 4: 多后端模型引擎与批处理调度层 (Multi-Backend Engine & Batch)
- **代码位置**：`include/engine/`，`src/engines/`
- **核心职责**：
  1. 纯虚接口屏蔽硬件差异（`ILlmEngine`, `IEmbeddingEngine`, `ICvEngine`）；
  2. **固定 Max Batch 自动调度（`FixedBatchExecutor`）**：解决端侧 NPU 静态编译 `max_batch_size` 限制，自动完成批次切分、末尾补齐 Dummy Pad、推理后剔除 Pad 与结果回溯对齐；
  3. 切换底层芯片（NPU/GPU/CPU）只需改动 JSON 配置中的 `engine_type`，业务代码 0 修改。

---

## 3. 数据流转与调用时序 (Runtime Sequence)

```mermaid
sequenceDiagram
    autonumber
    participant App as 外部系统 (L1)
    participant Adapter as C 适配层 (L1)
    participant Pipe as Pipeline 调度器 (L2)
    participant Ctx as AlgContext 黑板 (L2)
    participant Node as 业务算子 INode (L3)
    participant Engine as 模型引擎 & Batch调度器 (L4)
    participant HW as 底层硬件 NPU/GPU (L4)

    App->>Adapter: Alg_Process(inputs: vector<void*>)
    Adapter->>Ctx: 1. 解包外部结构体，注入输入数据
    Adapter->>Pipe: 2. Execute(ctx)
    
    loop 依次执行各算子节点
        Pipe->>Node: Process(ctx)
        Node->>Ctx: 读取上游特征 / 溯源数据
        opt 需要模型推理
            Node->>Engine: InferTraceableBatch(items)
            Engine->>Engine: 固定 Batch 切块 + Dummy Pad 补齐
            Engine->>HW: 执行硬件推理 (固定 batch_size)
            HW-->>Engine: 返回硬件 Raw 输出
            Engine->>Engine: 剥离 Pad，恢复 (req_id, sub_id) 溯源标签
            Engine-->>Node: 返回强类型对齐输出
        end
        Node->>Node: 处理业务私有逻辑 / 规则字典匹配
        Node->>Ctx: 写回中间特征或最终结果
    end

    Pipe-->>Adapter: 管线执行完成
    Adapter->>Ctx: 3. 提取最终输出结果
    Adapter->>App: 4. 打包回 outputs: vector<void*>，返回状态码 0
```

---

## 4. 算法开发者开发新业务指南（3 步上手）

算法同学开发新业务仅需 3 步，**无需修改任何 Core 框架代码**：

### 步骤 1：新建算子源文件并继承 `INode`

```cpp
#include "core/node_base.h"
#include "core/node_registry.h"

namespace alg_framework {

class MyCustomNode : public INode {
public:
    // 1. 初始化：读取私有配置，定义类成员存储私有业务数据
    bool Init(const nlohmann::json& config, SessionContext* session_ctx) override {
        my_threshold_ = config.value("threshold", 0.8f);
        return true;
    }

    // 2. 执行业务计算：从 req_ctx 读取数据，计算后写回
    int Process(AlgContext* req_ctx) override {
        auto* input = req_ctx->Get<std::string>("some_key");
        // 业务处理 ...
        req_ctx->Set("output_key", "result");
        return 0;
    }

    const std::string& Name() const override {
        static std::string name = "MyCustomNode";
        return name;
    }

private:
    float my_threshold_ = 0.8f; // 节点私有状态
};

// 3. 注册节点 (一行宏即可完成自动注册)
REGISTER_NODE(MyCustomNode);

} // namespace alg_framework
```

### 步骤 2：编写业务配置文件（JSON）

在 `configs/` 下新建配置文件，自由编排模型和算子顺序：

```json
{
  "business_name": "my_new_business",
  "models": [
    {
      "model_id": "my_llm",
      "engine_type": "mock_npu_llm",
      "model_path": "./models/my_llm.bin"
    }
  ],
  "pipeline": [
    { "node_type": "MyCustomNode", "config": { "threshold": 0.9 } }
  ]
}
```

### 步骤 3：交付配置文件与算法库即可！
