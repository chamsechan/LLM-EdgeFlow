# RFC 0012: I/O 契约驱动的通用 Node 架构

- **RFC 编号**：0012-node-authoring-experience
- **创建日期**：2026-08-27
- **文档状态**：Proposed
- **关联分支**：`feat/node-authoring-experience`
- **目标版本**：v4.1.0
- **负责人 / 作者**：LLM-EdgeFlow Architecture & Quality Team

---

## 1. 最终结论

LLM-EdgeFlow 不应按业务名、Pipeline 阶段名或单纯的物理输出格式拆分 Node。最终采用：

> **以 I/O 语义契约组织 Node，以单一操作语义定义 Node，以执行契约约束 Node。**

可简写为：**I/O-first, operation-defined, contract-guarded**。

三个层次必须分开：

```text
Node Family   = I/O 语义契约                 # 用于分类、检索和组合
Node Type     = 操作 + I/O 契约 + 执行契约   # C++ 实现与注册的边界
Node Instance = Type + Port Binding + Config + Model Binding
```

因此：

- “输出是文本/向量”适合作为分类依据，但不是 Node Type 的充分定义；
- LLM 和 ASR 都可输出文本，但输入、能力、失败语义不同，必须是不同 Node Type；
- Prompt、模型前后缀、阈值、TopK、规则表等差异优先进入配置；
- 只有操作、I/O 或执行语义发生变化时，才新增 Node Type；
- CPU、CUDA、AX650 属于 Engine/Profile，不进入 Node 分类；
- 当前 26 个 Biz Node 应迁移为 11 个首批通用 Node 的组合；当前 7 个示例业务目标上不再保留 Biz Node；
- “当前 Biz Node 为 0”是本次迁移结果，不是永久禁止领域 Node。未来确有不可配置的领域计算时仍可新增 Biz Node。

现有浅继承树方向正确，不增加 `BizNodeBase`、`PreNodeBase`、`PostNodeBase` 等阶段型抽象：

```text
INode
└── NodeBase
    └── ModelBoundNode<Capability>
        └── TraceableUnaryInferenceNode<Capability, Input, Output>
```

本 RFC 的核心不是“再造基类”，而是统一契约、引入实例级类型化端口绑定，并把业务差异从 C++ 类下沉到 Pipeline 配置。

---

## 2. 背景与目标

当前 Catalog 注册 27 个 Node，其中 26 个位于 `src/biz/`，仅 `LlmGenerateNode` 位于
`src/common_nodes/`。大量类的差异只是 Prompt、Key、阈值、规则、模型绑定或最终 DTO
封装，导致：

- 业务越多，Node Type 数量近似线性增长；
- 相同能力在不同业务目录重复实现；
- 开发者和 Agent 难以判断应该复用、配置还是新增 C++；
- 固定 Blackboard Key 使同一种通用 Node 难以在一个 Pipeline 中多次实例化；
- `NodeDefinition` 与构造函数重复声明契约，容易漂移；
- 一些 Pre/Post Node 实际承担的是 C ABI 转换或结果拷贝，层次归属不正确。

目标是让普通新业务优先成为：

```text
Operator Adapter + Pipeline JSON + 已有 Common Nodes + 测试
```

仅当现有操作语义无法表达需求时，才新增 Common Node 或真正的 Biz Domain Node。

### 2.1 范围内（In-Scope）

- 公共数据契约、实例级类型化端口绑定、Control 结果契约和 NodeDefinition 单一事实源；
- 第一阶段 Common Node Catalog 及其配置边界；
- 7 个现有业务、26 个 Biz Node 和 11 个 Pipeline 配置的原子迁移；
- Adapter ingress/egress 重整、Catalog/Validator/测试和开发工具同步更新。

### 2.2 范围外（Out-of-Scope）

- 不改变 6 个外部 C ABI 函数签名及现有 Operator 可观察行为；
- 不新增模型能力、硬件 backend 或外部业务模态；
- 不在本期引入通用脚本 DSL、任意动态类型黑板或跨 Pipeline 子图运行时；
- Pipeline fragment/template 仅在迁移后有重复证据时另行设计。

---

## 3. Node 的拆分判定

### 3.1 新增 Node Type 的必要条件

满足下列任一项才考虑新增 Node Type：

1. 操作发生变化，例如模板渲染与向量检索；
2. 输入或输出的**语义契约**发生变化，例如文本批次与带分数候选集；
3. 基数或 provenance 规则变化，例如一进一出与一进多出；
4. Engine capability、批处理或失败语义发生变化；
5. 生命周期或资源所有权发生变化。

### 3.2 仅增加配置或策略的条件

以下差异不得直接派生新业务类：

- Prompt 模板、模型特殊前后缀；
- 规则、关键词、正则、阈值、TopK、分隔符；
- 同一操作下可互换且有明确枚举的算法，如 `cosine` / `dot_product`；
- 模型 ID、Engine backend、硬件平台；
- Blackboard 实际 Key 名称。

可替换算法使用封闭、注册且可校验的 `strategy`；禁止使用
`mode=doc_qa|audit|xxx` 形成隐藏业务分支。

### 3.3 Biz Domain Node 的准入门槛

只有同时满足以下条件才允许保留或新增 Biz Node：

- 逻辑是真实领域计算，而非格式转换、调用模型、排序或 DTO 打包；
- 用现有 Common Node 组合会破坏语义、性能或原子性；
- 若配置化，必然演变为任意 DSL 或大量业务条件分支；
- 有独立契约、单元测试和至少一个明确业务所有者。

---

## 4. 目标通用契约与 Node Catalog

### 4.1 公共数据契约

公共契约应表达语义、基数和 provenance，而不只是 `string` / `vector<float>`：

| 契约 | 含义 |
| :--- | :--- |
| `TextBatch` | 带 `(req_id, sub_id)` 的文本批次 |
| `TextAttributesBatch` | 与文本项对齐的命名标量字段，不承载任意对象 |
| `EmbeddingBatch` | 与输入 provenance 对齐，并保留源文本引用的向量批次 |
| `QueryCandidatesBatch` | 查询与候选集合关系 |
| `RankedTextBatch` | 带分数、排名和来源的候选结果 |
| `RuleMatchBatch` | 规则 ID、分数及捕获字段 |
| `StructuredDocumentBatch` | 结构化文档及解析诊断 |
| `AudioPcmBatch` | 框架无关的音频样本 |
| `ImageRefBatch` | 图像引用或受控图像数据 |
| `OcrDocumentBatch` | OCR 文本、区域与置信度 |

这些类型放入独立的轻量 `include/contracts/`，只包含值类型和 provenance，不依赖
Adapter、Node 或具体 Engine。

### 4.2 第一阶段 Common Node

| Node Type | 输入 → 输出 | 配置职责 |
| :--- | :--- | :--- |
| `TextTemplateNode` | 文本/候选/属性 → `TextBatch` | 模板、静态变量、连接符、长度上限 |
| `TextChunkNode` | `TextBatch` → `TextBatch` | chunk size、overlap |
| `TextRuleMatchNode` | `TextBatch` → `RuleMatchBatch` | 规则表、匹配策略、阈值 |
| `StructuredJsonParseNode` | `TextBatch` → `StructuredDocumentBatch` | 受限 schema、字段映射、失败策略 |
| `TextEmbeddingNode` | `TextBatch` → `EmbeddingBatch` | 模型绑定、归一化选项 |
| `VectorTopKNode` | 查询/候选向量 → `RankedTextBatch` | metric、TopK、阈值 |
| `TextRerankNode` | `QueryCandidatesBatch` → `RankedTextBatch` | 模型绑定、TopK |
| `LlmGenerateNode` | `TextBatch` → `TextBatch` | 模型绑定、生成参数 |
| `AsrTranscribeNode` | `AudioPcmBatch` → `TextBatch` | 模型绑定 |
| `OcrDetectNode` | `ImageRefBatch` → `OcrDocumentBatch` | 模型绑定 |
| `TextCorpusSourceNode` | Session/config 资源 → `TextBatch` | 小型静态语料或资源引用 |

首批目标规模为 **11 个 Common Node Type**。数量不是 KPI；若原型证明
某个类型混合了两个操作，应拆分，若两个类型契约完全相同，则合并。

### 4.3 `TextTemplateNode` 边界

模板化是本次复用的关键，但必须保持受限：

```json
{
  "id": "build_prompt",
  "node_type": "TextTemplateNode",
  "depends_on": [],
  "ports": {
    "inputs": {"primary": "request.user_text"},
    "outputs": {"text": "generation.prompt"}
  },
  "config": {
    "template": "<|im_start|>system\n{{system}}<|im_end|>\n<|im_start|>user\n{{primary}}<|im_end|>\n<|im_start|>assistant\n",
    "values": {"system": "You are a helpful assistant."}
  }
}
```

约束如下：

- 仅支持标量占位符和确定性连接，不支持条件、循环、函数或任意 Blackboard 访问；
- 模板在 `Init` 阶段编译并校验，缺失变量、非法模板和超长结果 fail-closed；
- 保持输入输出 provenance 一一对应；
- Definition 固定声明 `primary: TextBatch`、`context: RankedTextBatch`、
  `matches: RuleMatchBatch`、`document: OcrDocumentBatch`、
  `attributes: TextAttributesBatch` 等可选逻辑输入及 `text: TextBatch` 输出；
- 至少绑定一个动态输入；配置只能绑定 Definition 已声明的端口；
- 业务内容模板可留在 Pipeline；模型 tokenizer/chat template 能力成熟后，模型专属序列化应下沉 Engine/Profile。

---

## 5. 实例级类型化端口绑定

通用 Node 能否成立，取决于同一 Node Type 能否在一个 Pipeline 中安全地重复实例化。
因此 Layer 2 必须先消除“Node Type 写死 Blackboard Key”的限制。

### 5.1 配置模型

```text
NodeDefinition
  logical input/output name + type_id + required + cardinality/provenance

Pipeline Node Instance
  logical port name -> actual Blackboard key

PipelineValidator
  definition + binding + DAG -> resolved typed port plan

Node runtime
  only BoundInput<T>/BoundOutput<T>; no string lookup or config reparse
```

`ParsedNodeConfig` 增加 `ports.inputs` / `ports.outputs`；`ValidatedPipelinePlan` 保存解析后的
端口绑定。`Pipeline` 只消费该 Plan，不重新解析、不重新排序。动态 Key 必须拥有稳定字符串
生命周期，不能把临时 `c_str()` 填入现有 `BlackboardKey<T>`。

最小接口形态固定为：

```cpp
struct ResolvedPortBinding {
  std::string logical_name;
  std::string blackboard_key;
  std::string type_id;
  PortDirection direction;
};

struct ValidatedNodePlan {
  ParsedNodeConfig node;
  nlohmann::json normalized_config;
  std::vector<ResolvedPortBinding> ports;
};

struct NodeInitContext {
  const ValidatedNodePlan* plan;
  SessionContext* session_ctx;
};
```

`ValidatedPipelinePlan` 持有 `ValidatedNodePlan`；`Pipeline` 用 `NodeInitContext` 初始化 Node。
`NodeBase` 根据已解析绑定提供拥有 Key 字符串生命周期的 `BoundInput<T>` /
`BoundOutput<T>`。具体 Node 只能按逻辑端口名获取类型化句柄，不能再次解析 `ports` JSON。

### 5.2 Validator 必须拒绝

- 未声明的逻辑端口；
- 必需端口未绑定；
- 绑定类型与 Definition 不一致；
- 缺少上游 producer；
- 非法重复 producer 或并行写冲突；
- provenance / cardinality 不兼容；
- Engine capability 与模型绑定不匹配。

`NodeDefinition` 是名称、端口、配置、能力和并发属性的唯一事实源；构造函数不得再次维护
一套默认 Key 或配置 schema。

### 5.3 Control 契约

当前 `INode::Control()` 默认返回 `0`，Pipeline 无法区分“已处理”和“不支持”。迁移时统一为：

```cpp
enum class NodeControlStatus { kUnsupported, kHandled, kFailed };

struct NodeControlResult {
  NodeControlStatus status;
  int code;
  std::string message;
};
```

`NodeDefinition` 声明支持的 command；Pipeline 只向声明支持的 Node 分发。任一处理失败则整体
失败，至少一个成功则成功，没有处理者则返回 unsupported。`TextRuleMatchNode` 负责规则和阈值
更新，`TextTemplateNode` 负责 Prompt 更新；更新采用线程安全的不可变配置快照，不修改请求级
状态。该约束替代当前“所有 Node 广播且默认成功”的模糊语义。

---

## 6. 四层职责

```text
External Operator
      ↓
Layer 1 Adapter: C ABI 校验、Unpack 为公共契约、Pack 外部输出
      ↓
Layer 2 Pipeline: 解析、类型化端口绑定、DAG 校验与调度
      ↓
Layer 3 Common Nodes: 单一操作、无业务名、无请求状态
      ↓
Layer 4 Engines: 模型/硬件能力、FixedBatchExecutor、平台差异
```

### Layer 1：Adapter

- `Unpack` 将外部 C DTO 转为 `AudioPcmBatch`、`TextBatch` 等公共 ingress；
- `Pack` 可读取多个公共 egress，完成对齐、内存复制和 C DTO 填充；
- 可提供类型化 Pack Helper 降低重复代码；
- 不执行检索、规则、Prompt、默认业务结果等领域计算。

因此不新增接触外部指针的 `ResultCopyNode`。现有纯输入转换和纯输出 DTO Node 应归回
Adapter；结构化解析仍由 Common Node 完成。

### Layer 2：Pipeline / Blackboard

- 引入实例级类型化端口绑定；
- Validator 仍是配置、DAG、类型、能力和并发校验的唯一入口；
- 请求数据只进入 `AlgContext`，共享模型和资源进入 `SessionContext`。

### Layer 3：Nodes

- 保留现有三层浅继承结构；
- Node 无业务名、无平台名、无外部 DTO、无请求级成员状态；
- Common Node 晋升要求：业务无关契约、至少两个业务场景测试；
- Control 必须区分 handled、unsupported 和 failed，不允许静默成功。

### Layer 4：Engines

- CUDA、CPU、AX650 和模型实现由 Engine/Profile 选择；
- 固定 Batch 推理继续统一经过 `FixedBatchExecutor::Execute`；
- Node 仅声明 semantic capability，不感知 vendor 类型或硬件分支。

---

## 7. 当前 26 个 Biz Node 的演变

### 7.1 按现有类迁移

| 当前 Biz | 当前 Node | 目标 |
| :--- | :--- | :--- |
| AudioAsrIntent | `AudioFeaturePreNode` | Adapter `Unpack` → `AudioPcmBatch` |
|  | `AsrInferNode` | `AsrTranscribeNode` |
|  | `SlotExtractNode` | `TextRuleMatchNode` 的捕获规则配置 |
|  | `AudioPostNode` | Adapter `Pack` |
| CrossRerank | `RerankPairBuilderNode` | Adapter `Unpack` → `QueryCandidatesBatch` |
|  | `CrossRerankBatchNode` | `TextRerankNode` |
|  | `RerankSortPostNode` | `TextRerankNode` 排序/TopK + Adapter `Pack` |
| DialogueAudit | `SafetyRulePreNode` | `TextRuleMatchNode` |
|  | `DenseRetrievalNode` | `TextCorpusSourceNode` + `TextEmbeddingNode` + `VectorTopKNode` |
|  | `CrossRerankNode` | `TextRerankNode` |
|  | `RiskPromptNode` | `TextTemplateNode` |
|  | `LlmAuditNode` | 合并到 `LlmGenerateNode` |
|  | `AuditPostNode` | `StructuredJsonParseNode` + Adapter `Pack` |
| DocQA | `DocChunkPreNode` | `TextChunkNode` |
|  | `DocEmbeddingNode` | 两个 `TextEmbeddingNode` 实例，分别绑定文档与查询端口 |
|  | `VectorSearchNode` | `VectorTopKNode` |
|  | `RerankRefineNode` | `TextRerankNode` |
|  | `PromptBuilderNode` | `TextTemplateNode` |
|  | `IntentRuleNode` | `TextRuleMatchNode` |
|  | `DocQaPostNode` | `StructuredJsonParseNode`（需要时）+ Adapter `Pack` |
| EntityExtract | `EntityExtractPreNode` | `TextTemplateNode` |
|  | `EntityExtractPostNode` | `StructuredJsonParseNode` + Adapter `Pack` |
| KeywordMatch | `KeywordMatcherNode` | `TextRuleMatchNode` + Adapter `Pack`；规则更新遵循统一 Control 契约 |
| OcrDocQA | `ImagePreNode` | Adapter `Unpack` → `ImageRefBatch` |
|  | `OcrInferNode` | 拆为 `OcrDetectNode` + `TextTemplateNode` |
|  | `OcrDocPostNode` | `StructuredJsonParseNode` + Adapter `Pack`，删除硬编码示例结果 |

### 7.2 迁移后的业务 Pipeline

| 业务 | 目标组合 |
| :--- | :--- |
| KeywordMatch | `TextRuleMatch` |
| EntityExtract | `TextTemplate → LlmGenerate → StructuredJsonParse` |
| CrossRerank | `TextRerank` |
| AudioAsrIntent | `AsrTranscribe → TextRuleMatch` |
| OcrDocQA | `OcrDetect → TextTemplate → LlmGenerate → StructuredJsonParse` |
| DocQA | `TextChunk → TextTemplate/Embedding → VectorTopK → [TextRerank] → TextTemplate → LlmGenerate → [StructuredJsonParse]` |
| DialogueAudit | `TextRuleMatch + Corpus/Embedding/VectorTopK → TextRerank → TextTemplate → LlmGenerate → StructuredJsonParse` |

Pipeline 实例数可能增加，但 Node Type 数量和业务 C++ 数量显著下降。这是合理交换：DAG
显式表达业务过程，比把多个操作重新藏进大型 Biz Node 更利于维护、测试和 Agent 生成。

---

## 8. 设计不变量与非目标

必须保持：

1. Node Type 只做一个可命名、可测试的操作；
2. 所有端口有公共类型、基数和 provenance 契约；
3. Node 不按业务名或平台名分支；
4. Node 不直接读写外部 C 指针或 DTO；
5. NodeDefinition 是契约单一事实源；
6. Pipeline 只消费 `ValidatedPipelinePlan`；
7. 推理批处理保持 `(req_id, sub_id)`，固定 Batch 由 Layer 4 处理；
8. 模板、规则和结构化解析必须是受限配置，不演变为通用脚本语言。

明确不做：

- 不建立 `AnyNode`、`variant` 黑板或运行时任意字符串取值；
- 不按 `Pre/Post/Biz` 阶段增加抽象基类；
- 不以减少类数量为唯一目标；
- 不用一个带大量 `mode` 的超级 Node 替代多个清晰操作；
- 不强制未来所有业务永远为 0 个 Biz Node；
- 不保留长期双配置 schema 或旧 Node alias。本项目尚未交付，迁移完成后直接删除旧实现。

---

## 9. 实施顺序

1. **契约与端口基础**
   - 建立公共值类型；
   - 扩展 NodeDefinition、Pipeline 配置和 Validator 的逻辑端口绑定；
   - 让 Node 只接收已校验的绑定和归一化配置。
2. **双场景原型**
   - EntityExtract 验证模板、LLM、结构化输出链；
   - CrossRerank 验证关系型输入、模型批处理、排序和 Adapter 直接 Pack。
3. **Common Node Catalog**
   - 实现并测试第一阶段通用 Node；
   - 用 Definition 自动驱动 Catalog、校验和 Agent 可发现信息。
4. **全量迁移**
   - 迁移其余五个业务及全部 11 个 Pipeline 配置；
   - 删除 26 个旧 Biz Node、重复 contract/key 和硬编码示例输出。
5. **开发体验收口**
   - `alg_pipeline_tool` 展示逻辑端口、类型、策略和示例；
   - 后续若重复子图明显，再增加 Pipeline fragment/template；展开后仍由同一 Validator 校验。

由于尚未实际交付，不引入长期兼容层。每个阶段可独立提交，但最终在同一特性分支完成原子
迁移后再进入主分支。

---

## 10. 测试与验收

- 每个 Common Node 覆盖正常、空输入、非法配置、缺失端口、类型错误和 provenance 测试；
- 每个 Common Node 至少由两个业务场景或一个业务场景加独立契约测试证明通用性；
- Validator 覆盖未知端口、未绑定端口、类型不匹配、缺 producer、重复 producer、并行写冲突和 capability 不匹配；
- Control 覆盖 handled、unsupported、failed、并发更新和不可变快照一致性；
- Adapter 覆盖多 egress Pack、容量不足、顺序/provenance 对齐和 C ABI 异常安全；
- 7 个现有业务保持外部 Operator 行为一致，11 个 Pipeline 配置全部通过 validate、plan 和 smoke；
- Engine 推理路径继续验证 FixedBatch padding、dummy stripping 和 provenance；
- 完成前执行：

```bash
./scripts/format.sh
cmake -B build -G Ninja -DLLM_EDGEFLOW_USE_CCACHE=ON
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
./scripts/run_all_tests.sh
```

验收指标：

- 当前 7 个业务由首批 11 个 Common Node Type 组合完成；
- 当前 26 个 Biz Node 注册归零，Adapter 保留 7 个业务边界实现；
- 同一 Node Type 可在单 Pipeline 多次实例化且无固定 Key 冲突；
- 不存在运行时 Key 猜测、业务 `mode`、外部 DTO Node 或硬编码业务结果；
- 所有测试 100% 通过后，RFC 才更新为 `Completed`。

---

## 11. 变更记录

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-08-27 | v1.0.0 | 将历次讨论压缩为最终架构结论与现有业务迁移方案 | LLM-EdgeFlow Architecture & Quality Team |
