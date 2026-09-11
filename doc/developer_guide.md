# LLM-EdgeFlow 开发者扩展指南

本文档说明当前扩展边界，并把实现者指向可编译的生产代码。不要从文档复制大段骨架；
接口签名、Definition 和注册宏以对应头文件及现有实现为准。开发生命周期见
[`CONTRIBUTING.md`](../CONTRIBUTING.md)，Agent 路由见 [`AGENTS.md`](../AGENTS.md)。

方案开发从[Studio 编排练习](../tools/pipeline_studio/README.md#第一次编排)开始；已有能力
和外部契约下，只需方案配置与必要的 `.conf`，Profile 可选。缺失业务算法时完成
[自定义 Node 动手练习](dev_guide/first_custom_node.md)，按需查阅
[五个概念说明](dev_guide/custom_node_concepts.md)。初始参数与运行中调参见
[Control 练习](dev_guide/first_control.md)，换模型后使用
[原生部署解析](VERIFIABLE_SELECTION.md#替换模型后确认实际生效配置)检查实际路径与参数。平台结构转换见
[业务接入指南](dev_guide/business_onboarding.md)，结果检查见
[运行当前方案](../tools/pipeline_studio/README.md#运行当前方案)。

本页保留四层扩展边界与进阶接口查询。普通方案开发通常无需修改 Core、Model 或
Backend；出现调度、模型语义或硬件能力缺口时，再查阅相应章节。

---

## 按职责选择扩展入口

| 架构层 | 新增什么？ | 核心修改文件 | 关键宏 / 核心类 |
| :--- | :--- | :--- | :--- |
| **接入适配层（Integration）** | 新增业务枚举、输入/输出纯 C 结构体与专属适配器 | `include/edgeflow/c_api.h`<br>`src/adapter/biz/<biz>_adapter.cpp` | `CompanyAlgBizType`<br>`IBizAdapter`<br>`REGISTER_BIZ_ADAPTER` |
| **流程编排层（Orchestration）** | 扩展动态黑板、会话模型管理与全局资源 | `include/core/alg_context.h`<br>`include/core/session_context.h` | `AlgContext::Read/Publish`<br>`SessionResourceKey<T>` |
| **能力节点层（Capability Nodes）** | 新增通用操作或可跨方案复用的领域算法 | `src/common_nodes/*.cpp`<br>`src/custom_nodes/*.cpp`<br>`include/nodes/*.h` | `NodeBase`<br>`REGISTER_NODE_WITH_DEFINITION(NodeName, def)` |
| **模型执行层（Model Execution）** | 新增模型语义或接入新推理后端 | `include/engine/model_interface.h`<br>`include/engine/backend_interface.h`<br>`src/engine/models/`<br>`src/engine/backends/` | `REGISTER_MODEL_WITH_DEFINITION`<br>`REGISTER_BACKEND_WITH_DEFINITION`<br>`ModelRuntimeFactory`<br>`FixedBatchExecutor` |

---

## 1. 接入适配层：如何新增一个业务的 C ABI 接口与专属 Adapter

> ⚠️ **平台治理红线**：普通业务接入严禁修改中心分发文件 `src/adapter/c_api_adapter.cpp`，必须编写业务专属 Adapter 类并注册。

业务需求的输入输出以完整 C ABI 请求/响应为准，由 Adapter 解包、转换和组装。
即使复用同一个 C 结构，字符串内部协议变化仍可能需要 Adapter 实现；不能用 Demo
预处理或后处理补足 SDK 契约。职责划分与复用判断见
[输入输出边界](dev_guide/business_onboarding.md#输入输出以-c-abi-为边界)。

### Operator 镜像结构与输出池扩展指南

新增 Operator 数据类型时必须区分两类协议：

| 协议 | 使用位置 | 扩展方式 |
| --- | --- | --- |
| 纯 C ABI DTO | `Alg_Process` 与 BizAdapter | 声明纯 C 类型，使用 `IBizAdapter` 转换并注册 |
| Operator 镜像 C 结构 | C++ `NamedIoBatch` Process 边界 | 注册值类型、业务槽位桥接、双向转换和输出池操作 |

Operator 扩展分为两步：先在 `OperatorValueTypeRegistry` 中建立“规范后缀 -> 显式
I/O 方向 + 外部 C 类型 + 校验/分配生命周期”的唯一绑定。输出 Binding 还必须统一声明
每个 `CompanyString` 字段的默认/最大容量、metadata 上限及池载荷预算，Resolver 和输出池
只消费这份 Schema，不维护第二套按后缀分支。再通过 `OperatorBizBridgeDescriptor` 声明
业务及逻辑槽位，并完成与内部 DTO 的逐字段转换。Bridge 完整性按实际注册的 Adapter
快照审计，新增业务无需修改中央业务 ID 列表。不要把 `.frame` 或 `.string` 直接绑定成
整套业务 DTO，也不要恢复“一帧恰好一个输入/输出组”的限制。命名 I/O Key 的
输入后缀必须与 Registry 中的规范后缀精确一致；输出后缀匹配 bridge 的 `key_suffix`，
未设置时匹配类型后缀，不做自动归一化。

`CompanyString` 只用于无嵌入 NUL 的文本，二进制内容使用 `CompanyBuffer`。Operator 镜像
结构不得替换或渗透内部 DTO。输入转换只读取 `.get()` 指针并复制数据值；输出由
Create 期固定池分配，Process 只向空输出槽位提交池化 shared_ptr。输出 deleter 只
持有池状态的 weak lifetime token，Destroy 后不得访问输出数据。任何需要修改
Blackboard、Node、Model 或 Backend 才能识别 Operator 结构的方案均违反分层要求。

目标交付共享库为 `company_alg_sdk`，产品 VERSION 为 10.0.0，
SOVERSION/C ABI major 为 5。
其正式动态符号面固定为 6 个 `Alg_*`、3 个 `AlgBase_*` 和 3 个 Operator 入口；
仓库内 Node、Registry、Model、Backend 和第三方运行时是隐藏实现，不得被外部扩展直接链接。
Operator v4 的 Create 和配置预检都使用部署根 `model_path` 加相对
`cfg_file_name`。每份 `.conf` 的根对象只能包含 `data`，`data` 只接受
`pipe_path`、`model_paths`、`mem_que` 和 `outputs`；单模型覆盖也必须使用以 `model_id`
为键的 `model_paths` 映射。单输出 `data.mem_que` 与按逻辑槽位配置的 `data.outputs`
互斥。Resolver 选择注册的输出类型与 `allocator`；独立配置读取组件通过固定枚举
选取配置项并返回字符串。方案用 `MakeOutputParameterParser<T>` 将参数文本解析为
普通 C++ 结构，框架归一化 `meta_num`、metadata type 和字段容量；业务桥接使用
该规范化结果，不重复解析原始部署 JSON 或
补默认值。每个输出槽位可注册自己的转换并拥有独立池，具体分配实现不接触队列深度。
完整例子见 [输出分配方案](dev_guide/operator_output_allocation.md)。

### Adapter 实施检查表

`Unpack` 负责业务字段校验及深拷贝；批次预检不能替代字段校验。RFC-0044 删除了未被
运行时调用的内部 `IBizAdapter::ValidateInput`：已有扩展应把校验迁入 `Unpack`（或其局部
辅助函数），移除 override，并重新编译。两种入口共用
[`biz_input_constraints.h`](../include/adapter/biz_input_constraints.h) 的渠道和音频限制，
分别处理 C 字符串与 Operator 显式长度；显式部署限制可更严格。

Biz egress 描述 Adapter 消费的内部端口。普通一对一出口仍要求 `1:1 / preserve`；
CrossRerank 的排名数组和 Compliance 的首项选择使用 `N:1 / aggregate`。
预检检查声明兼容性，打包阶段仍检查实际请求来源、排名及输出容量。

1. 当前环境的模拟平台枚举和 C 数据结构放在 `platform_mock/alg_types.h`，只使用
   C11 类型并明确所有权；函数入口保留在 `edgeflow/c_api.h`。公开结构体变更必须先有 RFC。
2. 在 `src/adapter/biz/` 实现无请求状态的 `IBizAdapter`，用
   `AdapterValidationHelper` 完成批次、指针、长度和输出容量校验。
3. `AdapterDescriptor::biz_definitions` 使用完整 `BizDefinition` 声明合法 `biz_name` 及
   ingress/egress typed ports；通过 `REGISTER_BIZ_ADAPTER` 注册，不修改中心派发。
4. `Unpack`/`Pack` 使用 `core/common_contracts.h` 中的中性值类型，并在
   `adapter/biz_blackboard_keys.h` 集中声明业务 ingress/egress `BlackboardKey<T>`；
   Core、Node 和 Engine 不得包含该业务 key 头。
5. 以 [`entity_extract_adapter.cpp`](../src/adapter/biz/entity_extract_adapter.cpp) 和
   [`cross_rerank_adapter.cpp`](../src/adapter/biz/cross_rerank_adapter.cpp) 为当前模板，
   并扩展 Adapter/C ABI/Operator 对应契约测试。

---

## 2. 流程编排层：Pipeline 与静态校验计划

流程编排层负责请求黑板生命周期与 DAG 管线单趟构建：
- **`ValidatedPipelinePlan`**：`PipelineValidator::ValidateAndPlan()` 单趟静态校验与 DAG 拓扑排序输出的不可变执行计划，`Pipeline::BuildInternal()` 直接消费该计划，杜绝运行时二次解析或隐式 DAG 计算；Node 支持代码只依赖其中抽出的 `ValidatedNodePlan` 轻量契约，不反向包含完整 Validator。
- **`BlackboardKey<T>`**：强类型黑板键，各算子间通过 `Require` 与 `Publish` 交换数据，杜绝无类型内存乱序。
- **`AlgContext` 并发契约**：输入使用 `Read` 获取只读快照，输出通过 typed port 单次
  `Publish`；不存在覆盖、删除或清空请求值的迁移入口。聚合行为由专用 Node 读取上游端口并
  发布新的输出 key，不原地修改已经发布的值。
- **`SessionResourceKey<T>`**：会话级共享资源必须使用带静态类型的 key；动态资源名也要先
  构造 typed key。相同名称只能绑定同一种 `T`，类型不匹配会抛出 `std::logic_error`，
  `GetOrCreateResource` 对同名同型资源提供 single-flight 创建。
- **`PipelineCatalogSnapshot`**：需要跨多次查找保持一致视图时先调用 `Snapshot()`；普通
  `Nodes/Bizs/FindNode/FindBiz` 返回独立值，不保存指向 Catalog 内部容器的引用或指针。

Node 作者仍使用 `BoundInput<T>::Require` 与 `BoundOutput<T>::Set`；端口包装负责执行
`Read/Publish`，无需在业务 Node 中管理锁或快照。

---

## 3. 能力节点层：如何新增通用或自定义 Node

先运行 `alg_pipeline_tool catalog --biz <name>` 和 `describe-node`。只有现有操作无法闭合
typed port 契约时才新增 Node。Node 必须：

- 通用操作放在 `src/common_nodes/`；领域算法与特定前后处理放在 `src/custom_nodes/`，
  默认按操作命名文件，不按业务建目录。自定义 Node 同样可以被多个方案复用；
- 通过 `NodeBase`、`ModelBoundNode` 或 `TraceableUnaryInferenceNode` 使用已经解析的逻辑
  端口，不固定实际 Blackboard Key；
- 请求间通过各自的 `AlgContext` 隔离数据，临时值留在处理函数局部，不把请求数据保存为成员；
  成员可持有配置和安全共享句柄。配置可初始化后固定，也可按
  [Control 约定](dev_guide/first_control.md)安全更新，每次处理读取一致快照；
- 提供完整 `NodeDefinition` 并通过 `REGISTER_NODE_WITH_DEFINITION` 一次注册；
- 在 Catalog 可见，并覆盖非法配置、端口缺失/类型错误、输出、provenance 和并发声明。

入门默认使用[轻量 LLM 模板](../dev_support/node_authoring/starter_llm_node.cpp)：
脚手架生成后，先编写 `BuildPrompt` 和 `FormatAnswer` 两个普通文本函数；端口与来源
处理保留在固定结构中。完整步骤见[第一个自定义 Node](dev_guide/first_custom_node.md)。

熟悉基本流程后，以 [`llm_generate_node.cpp`](../src/common_nodes/llm_generate_node.cpp)、
[`text_rerank_node.cpp`](../src/common_nodes/text_rerank_node.cpp) 及其同名测试为当前模板。

自定义 Node 可以在一次处理内完成前处理、调用声明绑定的模型和后处理，沿用现有
`ModelBoundNode`，无需新增专属基类。Definition 使用 `category = "custom"`；仅在存在
真实业务契约限制时设置 `biz_names`。平台结构转换留在 Adapter，Core、Engine 和通用
Node 不依赖自定义实现。编写、构建和复用步骤见
[自定义 Node 接入指南](../src/custom_nodes/README.md)。

---

## 4. 模型执行层：如何新增模型语义或推理 Backend

模型执行层必须保持两个独立扩展面：

- **Model** 实现 Embedding/Rerank/LLM/OCR/ASR 语义，只依赖
  `ITensorGraphSession`、`ITextGenerationSession`、`IImageTextGenerationSession` 或
  `IGeneratedTokenEmbeddingSession` 等中性协议。
  `generated_text_embedding` 负责生成 token 向量的池化与归一化；Backend 只返回原始向量。
  `vision_document` 将图像解码、补边和识别语义封装在 Model，Kite 类型仍只在 Backend 内出现。
- **Backend** 封装 ONNX Runtime、llama.cpp、TensorRT 或 NPU SDK，加载后返回
  `IBackendSession`，不实现业务模型语义。

已有 Model 能力只是切换硬件时，只新增 Backend；已有 Backend
协议能支持新模型时，只新增 Model。不得再创建同时包含模型语义和
第三方运行时的 `*Engine`。

Model 自注册需实现 `IModel` 的某一强类型能力并声明所需协议；Backend 实现
`IInferenceBackend` 并只返回中性 `IBackendSession`。二者分别提供完整
`ModelDefinition` / `BackendDefinition` 并使用对应 `REGISTER_*_WITH_DEFINITION` 宏。

Embedding 的归一化选择由 `EmbeddingOptions.normalize` 决定，模型负责实际计算；
TextEmbeddingNode 将 `config.normalize` 传给调用选项。BGE 不再接受重复的
`model_config.normalize`，已有配置应将该选择移到消费节点。

后端有跨字段约束时，通过 `BackendDefinition.validate_config` 注册纯配置校验函数。
PipelineValidator 在字段检查和默认值展开后调用；Backend 初始化复用同一解析规则。
回调不得加载模型或访问外部资源，例如 llama.cpp 的 `decode_batch_size` 不得大于
`context_size`。环境、设备和资产可用性仍由实际加载路径检查。

其中，Model 的 `Concurrency()` 只声明语义对象是否可重入，Backend Session 的
`Concurrency()` 声明具体运行时资源能力，Pipeline 以二者更严格的值调度。
`ModelRuntimeFactory` 会将 Model 要求写入 `BackendLoadSpec::requested_protocol`，Backend
必须在创建厂商资源前拒绝不支持的显式协议。文本生成协议接收 Model 已格式化的 prompt、
`add_bos`、统一采样参数和可选 seed；暴露 logits 的 Backend 通过 Backend 私有
`IAutoregressiveDecoder` 复用 `CommonAutoregressiveGenerator`，托管生成 Backend 可直接
实现会话。decoder 不进入 Catalog，vendor 类型不得离开 concrete Backend。

Pipeline 配置只使用 Model/Backend 语法：

```json
{
  "model_id": "embedding_v1",
  "capability": "embedding",
  "model_type": "my_embedding_model",
  "backend": "my_tensor_backend",
  "model_path": "embedding/model.bin",
  "model_config": {"embedding_dim": 768},
  "backend_config": {"max_batch_size": 4}
}
```

`ModelRuntimeFactory` 会验证 Model 能力、执行协议、并发模型与配置字段，
再把构建好的 `IModel` 原子注册到 `ModelManager`。参考实现：
`src/engine/models/bge_embedding/` 与 `src/engine/backends/onnxruntime/`。

---

## 5. 验证与交付

开发中运行最小相关测试。本地交付运行 `./scripts/run_all_tests.sh`；已授权的 PR 交付
由交付脚本执行同一门禁，无需预先单独运行。是否需要 RFC、
Changelog、PR 或合并，以及对应授权边界，统一遵循
[`CONTRIBUTING.md`](../CONTRIBUTING.md)，本指南不维护第二套流程。
