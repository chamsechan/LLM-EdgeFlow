# LLM-EdgeFlow 架构设计

本文档说明 LLM-EdgeFlow 的职责边界、编译依赖和运行时数据流。算法方案由 Pipeline 组合能力节点，接入适配负责外部契约，模型执行负责模型语义与推理后端。

---

## 1. 架构总览

框架采用四层架构，统一按职责命名：**接入适配层 → 流程编排层 → 能力节点层 → 模型执行层**。

| 职责名称 | 英文名称 | 源码归属 | 构建目标 |
| :--- | :--- | :--- | :--- |
| 接入适配层 | Integration | `include/adapter/`、`include/operator/`、`src/adapter/` 及公共 C ABI | `edgeflow_integration_objects` |
| 流程编排层 | Orchestration | `include/core/`、`src/core/` | `edgeflow_orchestration_objects` |
| 能力节点层 | Capability Nodes | `include/nodes/`、`src/common_nodes/`、`src/custom_nodes/` | `edgeflow_capability_nodes_objects` |
| 模型执行层 | Model Execution | `include/engine/`、`src/engine/` | `edgeflow_model_execution_objects` |

文档、工具诊断和构建目标使用上述职责名称。旧资料中的 Layer 1–4 按表格顺序对应这四层；编号仅用于阅读历史记录。源码目录继续按 Adapter、Core、Nodes、Engine 等组件组织。

下图展示组件职责与调用关系；编译依赖由下文的构建边界约束。`include/core/` 中的中性运行时契约通过独立的 `edgeflow_runtime_contracts` 提供给实现层，不能将目录名称直接等同于完整编译依赖。

```mermaid
graph TD
    classDef integration fill:#E3F2FD,stroke:#1565C0,stroke-width:2px,color:#0D47A1;
    classDef orchestration fill:#E8F5E9,stroke:#2E7D32,stroke-width:2px,color:#1B5E20;
    classDef capability_nodes fill:#FFF3E0,stroke:#E65100,stroke-width:2px,color:#E65100;
    classDef model_execution fill:#F3E5F5,stroke:#7B1FA2,stroke-width:2px,color:#4A148C;
    classDef ext fill:#ECEFF1,stroke:#37474F,stroke-width:1px,color:#263238;

    subgraph External["外部调用方 (业务APP / 车机系统 / 边缘平台)"]
        Caller["下游集成程序 / 主控服务"]
    end

    %% Integration
    subgraph Integration["接入适配层（Integration）"]
        C_API["公司统一标准 C ABI 接口<br>• Alg_Init / Alg_DeInit<br>• Alg_Create / Alg_Destroy<br>• Alg_Process(const void** inputs, num_inputs, void** outputs, num_outputs)<br>• Alg_Control"]
        C_Adapter["company_c_adapter.cpp<br>• 同句柄 Process / Control 串行化<br>• 异常拦截屏障 (noexcept 安全防护)<br>• 外部输入解包 / 输出结构体强转打包"]
    end

    %% Orchestration
    subgraph Orchestration["流程编排层（Orchestration）"]
        PipeCore["Pipeline 核心调度器 (pipeline.cpp)<br>• 消费 ValidatedPipelinePlan<br>• 算子波前执行与错误熔断"]
        
        subgraph StateMgr["三级状态与注册管理器"]
            S_Ctx["SessionContext (句柄级持久状态)<br>• ModelManager 多模型池<br>• SessionResourceKey&lt;T&gt; 类型安全缓存"]
            R_Ctx["AlgContext (请求级瞬态黑板)<br>• Read / Publish 不可变快照<br>• 只读视图随请求生命周期稳定"]
            TraceTag["TraceableItem 溯源追踪<br>• req_id (请求索引)<br>• sub_id (1对N分片索引)"]
            Factory["NodeFactory / ModelRegistry / BackendRegistry<br>• *_WITH_DEFINITION 就地注册"]
        end
    end

    %% Capability Nodes
    subgraph CapabilityNodes["能力节点层（Capability Nodes）"]
        NodeApi["INode 运行时接口"]
        NodeBase["NodeBase<br>final noexcept 生命周期与 Typed I/O"]
        ModelNode["ModelBoundNode / TraceableUnaryInferenceNode"]
        CustomNodes["自定义节点扩展目录 (src/custom_nodes/)<br>复用现有接口，按操作组织文件"]
        
        subgraph CommonNodes["通用能力算子池 (src/common_nodes/)"]
            LlmNode["LlmGenerateNode (大语言模型生成)"]
            ChunkNode["TextChunkNode (文本切片)"]
            RuleNode["TextRuleMatchNode (规则与关键词匹配)"]
            EmbedNode["TextEmbeddingNode (向量提取)"]
            TopKNode["VectorTopKNode (Top-K 检索)"]
            RerankNode["TextRerankNode (精排评分)"]
            TemplateNode["TextTemplateNode (提示词模板渲染)"]
            JsonNode["StructuredJsonParseNode (JSON 结构化解析)"]
            AsrNode["AsrTranscribeNode (语音转写)"]
            OcrNode["OcrDetectNode (OCR 识别)"]
            CorpusNode["TextCorpusSourceNode (语料源)"]
        end
    end

    %% Model Execution
    subgraph ModelExecution["模型执行层（Model Execution）"]
        ModelBase["IModel 强类型能力抽象"]
        BackendBase["IInferenceBackend / IBackendSession<br>中性执行协议"]
        LlmIntf["ILlmModel + ITextGenerationSession<br>(formatted prompt / unified options / text)"]
        EmbedIntf["IEmbeddingModel / IRerankModel<br>TensorGraph / GeneratedTokenEmbedding"]
        
        BatchExec["FixedBatchExecutor (硬件固定 Batch 调度器)<br>• 样本自动 Chunking 分块<br>• 末尾 Dummy Pad 自动补齐<br>• 推理后剥离 Pad 并保留溯源标签"]
        
        subgraph ModelSemantics["模型语义实现 (src/engine/models/)"]
            BgeModels["BgeEmbeddingModel / BgeRerankerModel"]
            GeneratedEmbedModel["GeneratedTextEmbeddingModel<br>(generated token pooling / normalization)"]
            QwenModel["QwenCausalLmModel<br>(ChatML / provenance / protocol delegation)"]
        end

        subgraph HardwareBackends["硬件推理 Backend (src/engine/backends/)"]
            OnnxBackend["OnnxRuntimeBackend<br>(TensorGraph, CPU/CUDA)"]
            LlamaCpp["LlamaCppBackend<br>(TextGeneration, GGUF runtime)"]
            KiteLlm["KiteLlmBackend<br>(Text / ImageText / GeneratedTokenEmbedding, conditional SDK)"]
        end
    end

    %% 连接关系
    Caller <==|纯 C 指针数组 const void** inputs, outputs| C_API
    C_API --> C_Adapter
    C_Adapter -->|构造/销毁| PipeCore
    C_Adapter -->|解包/打包| R_Ctx
    PipeCore --> S_Ctx
    PipeCore --> NodeApi
    NodeApi --> NodeBase
    NodeBase --> ModelNode
    NodeBase -.-> CommonNodes
    NodeBase -.-> CustomNodes
    CommonNodes & CustomNodes -->|读写特征与溯源数据| R_Ctx
    CommonNodes & CustomNodes -->|从 ModelManager 获取模型| S_Ctx
    CommonNodes & CustomNodes -->|调用强类型模型能力| LlmIntf & EmbedIntf
    LlmIntf & EmbedIntf --> ModelSemantics
    ModelSemantics -->|仅依赖中性协议| BackendBase
    BackendBase --> HardwareBackends
    ModelSemantics --> BatchExec

    class Caller ext;
    class C_API,C_Adapter integration;
    class PipeCore,S_Ctx,R_Ctx,TraceTag,Factory orchestration;
    class NodeApi,NodeBase,ModelNode,CommonNodes,CustomNodes,LlmNode,ChunkNode,RuleNode,EmbedNode,TopKNode,RerankNode,TemplateNode,JsonNode,AsrNode,OcrNode,CorpusNode capability_nodes;
    class ModelBase,BackendBase,LlmIntf,EmbedIntf,BatchExec,BgeModels,GeneratedEmbedModel,QwenModel,OnnxBackend,LlamaCpp,KiteLlm model_execution;
```

---

## 2. 职责与扩展边界

### 接入适配层（Integration）
- **代码位置**：`include/company_alg_interface.h`，`include/operator/`，`src/adapter/`
- **核心职责**：
  1. 导出公司限定的标准 C 接口：`Alg_Init`, `Alg_Create`, `Alg_Process`, `Alg_Control`, `Alg_Destroy`, `Alg_DeInit`；
  2. 导出公共日志 C API：`AlgBase_setLogLevelByName`, `AlgBase_getLogLevelByName`, `AlgBase_logPrint`；
  3. 导出基于命名 I/O 槽位的 C++ Operator 门面：`Get_LLM_EDGEFLOW_OperatorTable()`, `GetOperatorLastError()`, `ValidateOperatorConfigBinding()`；
  4. 充当 `noexcept` 安全屏障，拦截所有 C++ 异常，防止跨动态库边界崩溃；
  5. 将外部传入的纯 C 指针数组或 NamedIoBatch 解包，转入内部强类型的 `AlgContext`。

#### 双外部门面与单一内部运行时架构

接入适配层并行维护两个外部门面，统一由 `SharedAlgorithmRuntime` 执行调度：

```text
纯 C ABI：const void** / void** + 现有 CompanyAlg DTO ─┐
                                                       ├─> SharedAlgorithmRuntime
C++ Operator API：NamedIoBatch + Operator 镜像 C 结构 ─┘
```

- 纯 C ABI 继续保持 C11、固定布局和现有六函数契约，当前 ABI 版本见下文。
- v10.0.0 / ABI 5 只承诺上述 12 个动态入口；Node、Registry、Model、Backend 及第三方
  运行时符号使用 hidden visibility，不构成稳定动态 ABI。
- 同一 C ABI handle 的 `Alg_Process` 与 `Alg_Control` 串行执行；不同 handle 可并行。
  `Alg_Destroy` 前调用方必须停止提交并等待该 handle 上所有调用返回，返回后句柄永久失效。
- C++ Operator API 根据 Key 的最后一个点号解析类型后缀：
  `OperatorValueTypeRegistry` 负责“后缀到外部 C 类型”的唯一绑定，
  `OperatorBizBridgeDescriptor` 负责按业务和方向收集一个或多个槽位，再转换为
  内部 DTO；两种协议不得通过 `reinterpret_cast` 混用布局。
- 组件调用关系：`外部调用方 → Operator / C ABI → Pipeline → Node → Model → Backend → Platform`。
  `Operator` 表达对外交付的算法实例，`Platform`（`ComputePlatform`）表达底层硬件执行平台（CPU、CUDA、AX650、Ascend 等）。
- 同一业务可以使用一个聚合结构槽位，也可以由多个原子槽位组成；支持多槽位解绑。
- `CompanyString` 只表达无嵌入 NUL 的文本；任意二进制数据使用 `CompanyBuffer`。
- 输入由外部持有，Process 只借用裸指针并复制所需值，不持有输入 shared_ptr。
- 输出由算法库在 Create 期按 `max_frame_depth` 预分配；Process 返回带自定义
  deleter 的 shared_ptr，最后一个引用析构后 reset 并回池；deleter 只捕获池状态的
  weak lifetime token，避免 Destroy 后解引用已释放句柄或池。
- 值类型表、业务桥接表和内存池只属于接入适配层，不得进入 Blackboard、Node、Model 或 Backend。
- 目标共享库输出名称为 `company_alg_sdk`，产品 VERSION 为 10.0.0，
  SOVERSION/C ABI major 为 5。
- v4 Create 和配置预检都以必填部署根 `model_path` 加相对 `cfg_file_name` 解析；
  `.conf` 的 `data.mem_que` 归一化输出后缀、metadata 容量和嵌套字段容量。

### 流程编排层（Orchestration）
- **代码位置**：`include/core/`，`src/core/`
- **核心职责**：
  1. **配置驱动与执行计划**：通过 `PipelineValidator::ValidateAndPlan` 一次性完成 JSON 解析、业务契约查找、拓扑排序生成 `ValidatedPipelinePlan`，杜绝重复解析与排序；
  2. **三级状态管理**：
     - `SessionContext`：句柄级常驻状态，管理单句柄加载的多个模型实例
       （`ModelManager`）与 `SessionResourceKey<T>` 类型安全资源；同名异型访问在 cast 前
       fail-closed，`GetOrCreateResource` 对同一 key 执行 single-flight 创建；
     - `AlgContext`：请求级强类型黑板（`BlackboardKey<T>`），`Read` 返回请求生命周期内
       稳定的只读视图，`Publish` 拒绝重复生产；Adapter 与 Node 使用同一 write-once 契约；
     - `TraceableItem<T>`：样本溯源标签（`req_id` + `sub_id`），保证 1对N 裂变后可严格 1:1 对齐回原请求；
  3. **自注册 SSOT 机制**：Node、Model 与 Backend 分别通过 `REGISTER_NODE_WITH_DEFINITION`、`REGISTER_MODEL_WITH_DEFINITION` 和 `REGISTER_BACKEND_WITH_DEFINITION` 就地声明；`PipelineCatalog` 查询返回值快照，Validator 每次规划只消费一次稳定的 Node/Biz Catalog 快照，后续注册不会使当前计划悬空。

### 能力节点层（Capability Nodes）
- **代码位置**：`src/common_nodes/`，`src/custom_nodes/`，`include/nodes/`
- **核心职责**：
  1. **算法工程师核心开发区**：算子继承 `NodeBase`，单模型算子继承 `ModelBoundNode`；
  2. **异常安全屏障**：`NodeBase::Init` 和 `NodeBase::Process` 设为 `final noexcept`，派生类覆写 `InitNode` 与 `ProcessNode`，提供 `Require`、`Publish`、`Fail` 辅助方法；
  3. **模块化与配置组合**：11 类核心通用算子（`LlmGenerateNode`, `TextChunkNode`, `TextRuleMatchNode`, `TextEmbeddingNode`, `VectorTopKNode`, `TextRerankNode`, `TextTemplateNode`, `StructuredJsonParseNode`, `AsrTranscribeNode`, `OcrDetectNode`, `TextCorpusSourceNode`）全部收敛在 `src/common_nodes/`，通过 JSON Pipeline 自由编排。
  4. **领域扩展与复用**：用户算法集中在 `src/custom_nodes/`，按操作命名文件，可跨方案复用。
     两类 Node 共用能力节点层构建目标、基类和注册机制；领域 Node 可完成前处理、声明绑定的
     模型调用与后处理，平台结构转换仍属于 Adapter。Core、Engine 和通用 Node 不依赖
     自定义实现。接入步骤见[自定义 Node 指南](../src/custom_nodes/README.md)。

### 模型执行层（Model Execution）
- **代码位置**：`include/engine/`，`src/engine/`
- **核心职责**：
  1. `IEmbeddingModel`、`IRerankModel`、`ILlmModel`、`IOcrModel` 和 `IAsrModel` 表达模型语义，Node 只依赖所需能力；
  2. `ITensorGraphSession`、`ITextGenerationSession`、`IImageTextGenerationSession` 和 `IGeneratedTokenEmbeddingSession` 表达中性执行协议；Qwen 只提交已格式化 prompt 与统一生成参数，llama.cpp 的低层 decoder 在 Backend 内复用公共自回归生成器，托管引擎可直接生成；ONNX Runtime 当前只提供 TensorGraph；
  3. `ModelRuntimeFactory` 依据 `model_type + backend` 组合模型与 Backend，校验协议和并发契约后再原子注册到 `ModelManager`；
  4. **固定 Max Batch 自动调度（`FixedBatchExecutor`）**：完成批次切分、Dummy Pad、Pad 剔除和 `(req_id, sub_id)` 溯源；
  5. 切换 NPU/GPU/CPU 或 LLM 生成引擎只改 JSON 中的 `backend`、`model_path` 与 `backend_config`，不改业务 Node 或模型语义实现。

### 编译期边界与 Composition Root

各层按总览表中的职责目标独立编译，只链接其下方的
dependency interface；根 `CMakeLists.txt` 是唯一 Composition Root，另以
`edgeflow_composition_objects` 持有日志和共享运行时装配翻译单元。最终 SDK、仓库工具和
测试只聚合这些对象，不重新声明层内源码。

聚合 include 搜索路径用于仓库内构建，不是编译器访问权限。分层约束由
`scripts/check_layer_dependencies.py` 按解析后的 include 路径检查，覆盖实现与公共头、
相对路径、共享辅助头及具体 Backend 的 vendor 头。Node 可引用的中性 Core 契约使用
显式清单；新增契约或改变依赖方向需同步设计与规则。LayerGuard 自测注入反向依赖，
验证这些规则能够拒绝违规源码。

业务 ingress/egress 的 Blackboard key 名称由接入适配层的
`adapter/biz_blackboard_keys.h` 持有；流程编排层只提供 Blackboard 机制和中性值类型，
能力节点层通过 `ValidatedNodePlan` 中已经解析的逻辑端口工作。这样业务槽位命名不会成为
Core、Node 或 Engine 的隐含依赖。

---

## 3. 数据流转与调用时序 (Runtime Sequence)

```mermaid
sequenceDiagram
    autonumber
    participant App as 外部调用方
    participant Adapter as C 适配层 (Integration)
    participant Pipe as Pipeline 调度器 (Orchestration)
    participant Ctx as AlgContext 黑板 (Orchestration)
    participant Node as 通用或自定义 NodeBase (Capability Nodes)
    participant Model as 强类型模型能力 (Model Execution)
    participant Backend as 中性协议 Backend 会话 (Model Execution)
    participant HW as 底层硬件 NPU/GPU

    App->>Adapter: Alg_Process(inputs: const void**, num_inputs, outputs: void**, &num_outputs)
    Adapter->>Ctx: 1. 解包外部结构体，注入输入数据
    Adapter->>Pipe: 2. Execute(ctx)
    
    loop 依次执行各拓扑层算子节点
        Pipe->>Node: Process(ctx)
        Node->>Ctx: Require(ctx, key, error_code) 读取上游特征
        opt 需要模型推理
            Node->>Model: Embed / Score / Generate / Recognize / Transcribe
            Model->>Model: 固定 Batch 切块 + Dummy Pad 补齐
            Model->>Backend: 通过 TensorGraph / TextGeneration 协议执行
            Backend->>HW: 调用硬件推理时
            HW-->>Backend: 返回原始执行结果
            Backend-->>Model: 返回中性 Tensor / Text 结果
            Model->>Model: 剥离 Pad，恢复 (req_id, sub_id) 溯源标签
            Model-->>Node: 返回强类型对齐输出
        end
        Node->>Node: 处理业务私有逻辑 / 规则字典匹配
        Node->>Ctx: Publish(ctx, key, value) 写回中间特征或最终结果
    end

    Pipe-->>Adapter: 管线执行完成
    Adapter->>Ctx: 3. 提取最终输出结果
    Adapter->>App: 4. 打包回 outputs: void**，返回状态码 0
```

---

## 4. 算法开发者新增节点示例

以下仅展示节点实现骨架。新增完整业务仍须按 RFC-first 流程同时提供 Business
Adapter/Definition、Pipeline JSON、GoogleTest，并通过完整门禁；不得把本节理解为
“三步即可交付一个业务”。

### 步骤 1：新建算子源文件并继承 `NodeBase`

```cpp
#include "core/node_registry.h"
#include "nodes/node_base.h"

namespace llm_edgeflow {

inline constexpr BlackboardKey<std::string> kInputText{"input_text", "string"};
inline constexpr BlackboardKey<std::string> kOutputText{"output_text", "string"};

class MyCustomNode final : public NodeBase {
 public:
  inline static constexpr char kNodeType[] = "MyCustomNode";

  MyCustomNode() : NodeBase(kNodeType) {}

 protected:
  // 1. 初始化：读取私有配置
  bool InitNode(const nlohmann::json& config,
                SessionContext& /*session_ctx*/) override {
    threshold_ = config.value("threshold", 0.8f);
    return true;
  }

  // 2. 执行业务计算：通过 Require / Publish 读写强类型黑板
  int ProcessNode(AlgContext& req_ctx) override {
    const auto* input = Require(req_ctx, kInputText, -9001);
    if (!input) return -9001;

    std::string result = *input + "_processed";
    Publish(req_ctx, kOutputText, std::move(result));
    return 0;
  }

 private:
  float threshold_ = 0.8f;
};

// 3. 声明元数据定义并自注册
NodeDefinition MakeMyCustomNodeDefinition() {
  NodeDefinition def;
  def.node_type = MyCustomNode::kNodeType;
  def.category = "business";
  def.description = "My custom business processing node";
  def.inputs = {RequiredInput(kInputText)};
  def.outputs = {Output(kOutputText)};
  def.config_fields = {ConfigFieldDefinition{
      "threshold", ConfigValueKind::kNumber, false, 0.8, 0.0, 1.0}};
  def.parallel_safe = true;
  return def;
}

REGISTER_NODE_WITH_DEFINITION(MyCustomNode, MakeMyCustomNodeDefinition());

} // namespace llm_edgeflow
```

### 步骤 2：编写业务配置文件（JSON）

在 `configs/` 下新建配置文件，自由编排模型和算子顺序：

```json
{
  "biz_name": "my_new_biz",
  "models": [
    {
      "model_id": "my_llm",
      "capability": "llm",
      "model_type": "qwen_causal_lm",
      "backend": "llama_cpp",
      "model_path": "my_llm.gguf",
      "model_config": {},
      "backend_config": {"context_size": 2048}
    }
  ],
  "pipeline": [
    { "id": "node_0", "node_type": "MyCustomNode", "config": { "threshold": 0.9 } }
  ]
}
```

### 步骤 3：交付配置文件与算法库即可！

图像文档识别沿用 `OcrDetectNode → IOcrModel`：`VisionDocumentModel` 在模型执行层
通过中性 `IImageTextGenerationSession` 调用 Kite，Model 负责图像解码与识别指令，
Backend 负责原生 RGB/聊天输入映射和运行资源。识别结果仅填充 `combined_text`，不伪造
`boxes` 或置信度；原有 C ABI/Operator、DAG 端口和请求溯源保持原样。

生成向量接入遵循相同分层：`generated_text_embedding` 实现 `IEmbeddingModel`，
经 `IGeneratedTokenEmbeddingSession` 获得生成 token 隐藏向量；Model 独占 prompt、
池化及归一化语义，Backend 独占原生任务和输出内存。该向量空间与 BGE encoder
不同，既有 ONNX 模型和配置保持可用，见 [RFC-0035](rfcs/0035-generated-token-embedding.md)。
