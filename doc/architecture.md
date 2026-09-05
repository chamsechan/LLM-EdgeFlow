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
    subgraph L1["Layer 1: Operator 接入与 C-ABI 适配层 (Operator & C-ABI Adapter Layer)"]
        C_API["公司统一标准 C ABI 接口<br>• Alg_Init / Alg_DeInit<br>• Alg_Create / Alg_Destroy<br>• Alg_Process(const void** inputs, num_inputs, void** outputs, num_outputs)<br>• Alg_Control"]
        C_Adapter["company_c_adapter.cpp<br>• 同句柄 Process / Control 串行化<br>• 异常拦截屏障 (noexcept 安全防护)<br>• 外部输入解包 / 输出结构体强转打包"]
    end

    %% Level 2
    subgraph L2["Layer 2: 管线调度与状态黑板层 (Pipeline & Multi-Level Context Layer)"]
        PipeCore["Pipeline 核心调度器 (pipeline.cpp)<br>• 消费 ValidatedPipelinePlan<br>• 算子波前执行与错误熔断"]
        
        subgraph StateMgr["三级状态与注册管理器"]
            S_Ctx["SessionContext (句柄级持久状态)<br>• ModelManager 多模型池<br>• SessionResourceKey&lt;T&gt; 类型安全缓存"]
            R_Ctx["AlgContext (请求级瞬态黑板)<br>• Read / Publish 不可变快照<br>• 只读视图随请求生命周期稳定"]
            TraceTag["TraceableItem 溯源追踪<br>• req_id (请求索引)<br>• sub_id (1对N分片索引)"]
            Factory["NodeFactory / ModelRegistry / BackendRegistry<br>• *_WITH_DEFINITION 就地注册"]
        end
    end

    %% Level 3
    subgraph L3["Layer 3: 无请求状态的通用能力算子与可复用节点层"]
        NodeApi["INode 运行时接口"]
        NodeBase["NodeBase<br>final noexcept 生命周期与 Typed I/O"]
        ModelNode["ModelBoundNode / TraceableUnaryInferenceNode"]
        
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

    %% Level 4
    subgraph L4["Layer 4: 模型能力与推理 Backend 层 (Model & Backend Layer)"]
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
    NodeBase -.-> BizNodes
    CommonNodes & BizNodes -->|读写特征与溯源数据| R_Ctx
    CommonNodes & BizNodes -->|从 ModelManager 获取模型| S_Ctx
    CommonNodes & BizNodes -->|调用强类型模型能力| LlmIntf & EmbedIntf
    LlmIntf & EmbedIntf --> ModelSemantics
    ModelSemantics -->|仅依赖中性协议| BackendBase
    BackendBase --> HardwareBackends
    ModelSemantics --> BatchExec

    class Caller ext;
    class C_API,C_Adapter l1;
    class PipeCore,S_Ctx,R_Ctx,TraceTag,Factory l2;
    class NodeApi,NodeBase,ModelNode,CommonNodes,BizNodes,LlmNode,PromptNode,VecSearchNode,RerankNode,PreNode,RuleNode,PostNode l3;
    class ModelBase,BackendBase,LlmIntf,EmbedIntf,BatchExec,BgeModels,GeneratedEmbedModel,QwenModel,OnnxBackend,LlamaCpp,KiteLlm l4;
```

---

## 2. 4 层抽象职责定义

### Layer 1: Operator 与 C-ABI 接入层 (Operator & C-ABI Adapter)
- **代码位置**：`include/company_alg_interface.h`，`include/operator/`，`src/adapter/`
- **核心职责**：
  1. 导出公司限定的标准 C 接口：`Alg_Init`, `Alg_Create`, `Alg_Process`, `Alg_Control`, `Alg_Destroy`, `Alg_DeInit`；
  2. 导出公共日志 C API：`AlgBase_setLogLevelByName`, `AlgBase_getLogLevelByName`, `AlgBase_logPrint`；
  3. 导出基于命名 I/O 槽位的 C++ Operator 门面：`Get_LLM_EDGEFLOW_OperatorTable()`, `GetOperatorLastError()`, `ValidateOperatorConfigBinding()`；
  4. 充当 `noexcept` 安全屏障，拦截所有 C++ 异常，防止跨动态库边界崩溃；
  5. 将外部传入的纯 C 指针数组或 NamedIoBatch 解包，转入内部强类型的 `AlgContext`。

#### 双外部门面与单一内部运行时架构

Layer 1 并行维护两个外部门面，统一由 `SharedAlgorithmRuntime` 执行调度：

```text
纯 C ABI：const void** / void** + 现有 CompanyAlg DTO ─┐
                                                       ├─> SharedAlgorithmRuntime
C++ Operator API：NamedIoBatch + Operator 镜像 C 结构 ─┘
```

- 纯 C ABI 继续保持 C11、固定布局和现有六函数契约，其 ABI V2 保持源码与符号兼容。
- v10.0.0 / ABI 5 只承诺上述 12 个动态入口；Node、Registry、Model、Backend 及第三方
  运行时符号使用 hidden visibility，不构成稳定动态 ABI。
- 同一 C ABI handle 的 `Alg_Process` 与 `Alg_Control` 串行执行；不同 handle 可并行。
  `Alg_Destroy` 前调用方必须停止提交并等待该 handle 上所有调用返回，返回后句柄永久失效。
- C++ Operator API 根据 Key 的最后一个点号解析类型后缀：
  `OperatorValueTypeRegistry` 负责“后缀到外部 C 类型”的唯一绑定，
  `OperatorBizBridgeDescriptor` 负责按业务和方向收集一个或多个槽位，再转换为
  内部 DTO；两种协议不得通过 `reinterpret_cast` 混用布局。
- 命名解耦设计：`Integration -> Operator -> Pipeline -> Node -> Model -> Backend -> Platform`。
  `Operator` 表达对外交付的算法实例，`Platform`（`ComputePlatform`）表达底层硬件执行平台（CPU、CUDA、AX650、Ascend 等）。
- 同一业务可以使用一个聚合结构槽位，也可以由多个原子槽位组成；支持多槽位解绑。
- `CompanyString` 只表达无嵌入 NUL 的文本；任意二进制数据使用 `CompanyBuffer`。
- 输入由外部持有，Process 只借用裸指针并复制所需值，不持有输入 shared_ptr。
- 输出由算法库在 Create 期按 `max_frame_depth` 预分配；Process 返回带自定义
  deleter 的 shared_ptr，最后一个引用析构后 reset 并回池；deleter 只捕获池状态的
  weak lifetime token，避免 Destroy 后解引用已释放句柄或池。
- 值类型表、业务桥接表和内存池只属于 Layer 1，不得进入 Blackboard、Node、Model 或 Backend。
- 目标共享库输出名称为 `company_alg_sdk`，产品 VERSION 为 10.0.0，
  SOVERSION/C ABI major 为 5。
- v4 Create 和配置预检都以必填部署根 `model_path` 加相对 `cfg_file_name` 解析；
  `.conf` 的 `data.mem_que` 归一化输出后缀、metadata 容量和嵌套字段容量。

### Layer 2: 管线调度与状态黑板层 (Pipeline & State Engine)
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

### Layer 3: 通用能力算子层 (Stateless Capability Nodes)
- **代码位置**：`src/common_nodes/`，`include/nodes/`
- **核心职责**：
  1. **算法工程师核心开发区**：算子继承 `NodeBase`，单模型算子继承 `ModelBoundNode`；
  2. **异常安全屏障**：`NodeBase::Init` 和 `NodeBase::Process` 设为 `final noexcept`，派生类覆写 `InitNode` 与 `ProcessNode`，提供 `Require`、`Publish`、`Fail` 辅助方法；
  3. **模块化与配置组合**：11 类核心通用算子（`LlmGenerateNode`, `TextChunkNode`, `TextRuleMatchNode`, `TextEmbeddingNode`, `VectorTopKNode`, `TextRerankNode`, `TextTemplateNode`, `StructuredJsonParseNode`, `AsrTranscribeNode`, `OcrDetectNode`, `TextCorpusSourceNode`）全部收敛在 `src/common_nodes/`，通过 JSON Pipeline 自由编排。

### Layer 4: 模型能力与推理 Backend 层 (Model & Backend)
- **代码位置**：`include/engine/`，`src/engine/`
- **核心职责**：
  1. `IEmbeddingModel`、`IRerankModel`、`ILlmModel`、`IOcrModel` 和 `IAsrModel` 表达模型语义，Node 只依赖所需能力；
  2. `ITensorGraphSession`、`ITextGenerationSession`、`IImageTextGenerationSession` 和 `IGeneratedTokenEmbeddingSession` 表达中性执行协议；Qwen 只提交已格式化 prompt 与统一生成参数，llama.cpp 的低层 decoder 在 Backend 内复用公共自回归生成器，托管引擎可直接生成；ONNX Runtime 当前只提供 TensorGraph；
  3. `ModelRuntimeFactory` 依据 `model_type + backend` 组合模型与 Backend，校验协议和并发契约后再原子注册到 `ModelManager`；
  4. **固定 Max Batch 自动调度（`FixedBatchExecutor`）**：完成批次切分、Dummy Pad、Pad 剔除和 `(req_id, sub_id)` 溯源；
  5. 切换 NPU/GPU/CPU 或 LLM 生成引擎只改 JSON 中的 `backend`、`model_path` 与 `backend_config`，不改业务 Node 或模型语义实现。

### 编译期边界与 Composition Root

四层不只依靠目录约定，还分别编译为
`edgeflow_layer1_adapter_objects`、`edgeflow_layer2_core_objects`、
`edgeflow_layer3_node_objects` 和 `edgeflow_layer4_engine_objects`。各层只链接其下方的
dependency interface；根 `CMakeLists.txt` 是唯一 Composition Root，另以
`edgeflow_composition_objects` 持有日志和共享运行时装配翻译单元。最终 SDK、仓库工具和
测试只聚合这些对象，不重新声明层内源码。

业务 ingress/egress 的 Blackboard key 名称由 Layer 1 的
`adapter/biz_blackboard_keys.h` 持有；Layer 2 只提供 Blackboard 机制和中性值类型，
Layer 3 通过 `ValidatedNodePlan` 中已经解析的逻辑端口工作。这样业务槽位命名不会成为
Core、Node 或 Engine 的隐含依赖。

---

## 3. 数据流转与调用时序 (Runtime Sequence)

```mermaid
sequenceDiagram
    autonumber
    participant App as 外部系统 (L1)
    participant Adapter as C 适配层 (L1)
    participant Pipe as Pipeline 调度器 (L2)
    participant Ctx as AlgContext 黑板 (L2)
    participant Node as 通用能力 NodeBase (L3)
    participant Model as 强类型模型能力 (L4)
    participant Backend as 中性协议 Backend 会话 (L4)
    participant HW as 底层硬件 NPU/GPU (L4)

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
#include "nodes/node_support.h"

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

图像文档识别沿用 `OcrDetectNode → IOcrModel`：`VisionDocumentModel` 在 Layer 4
通过中性 `IImageTextGenerationSession` 调用 Kite，Model 负责图像解码与识别指令，
Backend 负责原生 RGB/聊天输入映射和运行资源。识别结果仅填充 `combined_text`，不伪造
`boxes` 或置信度；原有 C ABI/Operator、DAG 端口和请求溯源保持原样。

生成向量接入遵循相同分层：`generated_text_embedding` 实现 `IEmbeddingModel`，
经 `IGeneratedTokenEmbeddingSession` 获得生成 token 隐藏向量；Model 独占 prompt、
池化及归一化语义，Backend 独占原生任务和输出内存。该向量空间与 BGE encoder
不同，既有 ONNX 模型和配置保持可用，见 [RFC-0035](rfcs/0035-generated-token-embedding.md)。
