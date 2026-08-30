# LLM-EdgeFlow 开发者扩展指南

本文档说明当前扩展边界，并把实现者指向可编译的生产代码。不要从文档复制大段骨架；
接口签名、Definition 和注册宏以对应头文件及现有实现为准。开发生命周期见
[`CONTRIBUTING.md`](../CONTRIBUTING.md)，Agent 路由见 [`AGENTS.md`](../AGENTS.md)。

---

## 4 层扩展速查表 (Quick Reference)

| 架构层级 | 新增什么？ | 核心修改文件 | 关键宏 / 核心类 |
| :--- | :--- | :--- | :--- |
| **Layer 1: C ABI 适配层** | 新增业务枚举、输入/输出纯 C 结构体与专属适配器 | `include/company_alg_interface.h`<br>`src/adapter/adapters/<biz>_adapter.cpp` | `CompanyAlgBizType`<br>`IBizAdapter`<br>`REGISTER_BIZ_ADAPTER` |
| **Layer 2: 核心编排层** | 扩展动态黑板、会话模型管理与全局资源 | `include/core/alg_context.h`<br>`include/core/session_context.h` | `AlgContext::Read/Publish`<br>`SessionContext::SetResource()` |
| **Layer 3: 通用能力算子池** | 新增通用能力算子 (分片/向量检索/重排/模板/规则/解析) | `src/common_nodes/*.cpp`<br>`include/nodes/*.h` | `NodeBase`<br>`REGISTER_NODE_WITH_DEFINITION(NodeName, def)` |
| **Layer 4: Model / Backend 层** | 新增模型语义或接入新推理后端 | `include/engine/model_interface.h`<br>`include/engine/backend_interface.h`<br>`src/engine/models/`<br>`src/engine/backends/` | `REGISTER_MODEL_WITH_DEFINITION`<br>`REGISTER_BACKEND_WITH_DEFINITION`<br>`ModelRuntimeFactory`<br>`FixedBatchExecutor` |

---

## 1. Layer 1: 如何新增一个业务的 C ABI 接口与专属 Adapter

> ⚠️ **平台治理红线**：普通业务接入严禁修改中心分发文件 `src/adapter/company_c_adapter.cpp`，必须编写业务专属 Adapter 类并注册。

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
整套业务 DTO，也不要恢复“一帧恰好一个输入/输出组”的限制。

`CompanyString` 只用于无嵌入 NUL 的文本，二进制内容使用 `CompanyBuffer`。Operator 镜像
结构不得替换或渗透内部 DTO。输入转换只读取 `.get()` 指针并复制数据值；输出由
Create 期固定池分配，Process 只向空输出槽位提交池化 shared_ptr。输出 deleter 只
持有池状态的 weak lifetime token，Destroy 后不得访问输出数据。任何需要修改
Blackboard、Node、Model 或 Backend 才能识别 Operator 结构的方案均违反分层要求。

目标交付共享库为 `company_alg_sdk`，SOVERSION 为 4。
Operator v4 的 Create 和配置预检都使用部署根 `model_path` 加相对
`cfg_file_name`。每份 `.conf` 必须提供 `data.mem_que`，由 Resolver 归一化规范
输出后缀、`meta_num`、metadata type 和字段容量；业务桥接与输出池不得直接读取原始
JSON 键，也不得为缺失容量提供本地 fallback。当前配置只有一个 `data.mem_que`，因此
每个业务只允许一个输出池；支持多输出池前必须先扩展并版本化配置 Schema。

### Adapter 实施检查表

1. 在 `company_alg_interface.h` 中只声明 C11 枚举、定长结构、指针和明确所有权；公开
   结构体变更必须先有 RFC。
2. 在 `src/adapter/adapters/` 实现无请求状态的 `IBizAdapter`，用
   `AdapterValidationHelper` 完成批次、指针、长度和输出容量校验。
3. `AdapterDescriptor::pipelines` 使用完整 `BizDefinition` 声明合法 `biz_name` 及
   ingress/egress typed ports；通过 `REGISTER_BIZ_ADAPTER` 注册，不修改中心派发。
4. `Unpack`/`Pack` 使用 `core/common_contracts.h` 中的共享值类型和
   `BlackboardKey<T>`；不要引入业务专属内部 DTO 或裸字符串 Key 的第二套契约。
5. 以 [`entity_extract_adapter.cpp`](../src/adapter/adapters/entity_extract_adapter.cpp) 和
   [`cross_rerank_adapter.cpp`](../src/adapter/adapters/cross_rerank_adapter.cpp) 为当前模板，
   并扩展 Adapter/C ABI/Operator 对应契约测试。

---

## 2. Layer 2: 核心编排层与静态校验计划 (Pipeline & ValidatedPipelinePlan)

Layer 2 负责请求黑板生命周期与 DAG 管线单趟构建：
- **`ValidatedPipelinePlan`**：`PipelineValidator::ValidateAndPlan()` 单趟静态校验与 DAG 拓扑排序输出的不可变执行计划，`Pipeline::BuildInternal()` 直接消费该计划，杜绝运行时二次解析或隐式 DAG 计算。
- **`BlackboardKey<T>`**：强类型黑板键，各算子间通过 `Require` 与 `Publish` 交换数据，杜绝无类型内存乱序。
- **`AlgContext` 并发契约**：输入使用 `Read` 获取只读快照，输出通过 typed port 单次
  `Publish`；不存在覆盖、删除或清空请求值的迁移入口。聚合行为由专用 Node 读取上游端口并
  发布新的输出 key，不原地修改已经发布的值。

Node 作者仍使用 `BoundInput<T>::Require` 与 `BoundOutput<T>::Set`；端口包装负责执行
`Read/Publish`，无需在业务 Node 中管理锁或快照。

---

## 3. Layer 3: 如何新增通用能力 Node

先运行 `alg_pipeline_tool catalog --biz <name>` 和 `describe-node`。只有现有操作无法闭合
typed port 契约时才新增 Node。Node 必须：

- 表达一个可命名、可测试且业务无关的操作，放在 `src/common_nodes/`；
- 通过 `NodeBase`、`ModelBoundNode` 或 `TraceableUnaryInferenceNode` 使用已经解析的逻辑
  端口，不固定实际 Blackboard Key；
- 把请求状态留在 `AlgContext`，成员只保存不可变配置或并发安全句柄；
- 提供完整 `NodeDefinition` 并通过 `REGISTER_NODE_WITH_DEFINITION` 一次注册；
- 在 Catalog 可见，并覆盖非法配置、端口缺失/类型错误、输出、provenance 和并发声明。

以 [`llm_generate_node.cpp`](../src/common_nodes/llm_generate_node.cpp)、
[`text_rerank_node.cpp`](../src/common_nodes/text_rerank_node.cpp) 及其同名测试为当前模板。

---

## 4. Layer 4: 如何新增模型语义或推理 Backend

Layer 4 必须保持两个独立扩展面：

- **Model** 实现 Embedding/Rerank/LLM/OCR/ASR 语义，只依赖
  `ITensorGraphSession` 或 `ICausalLmSession` 等中性协议。
- **Backend** 封装 ONNX Runtime、llama.cpp、TensorRT 或 NPU SDK，加载后返回
  `IBackendSession`，不实现业务模型语义。

已有 Model 能力只是切换硬件时，只新增 Backend；已有 Backend
协议能支持新模型时，只新增 Model。不得再创建同时包含模型语义和
第三方运行时的 `*Engine`。

Model 自注册需实现 `IModel` 的某一强类型能力并声明所需协议；Backend 实现
`IInferenceBackend` 并只返回中性 `IBackendSession`。二者分别提供完整
`ModelDefinition` / `BackendDefinition` 并使用对应 `REGISTER_*_WITH_DEFINITION` 宏。

其中，Model 的 `Concurrency()` 只声明语义对象是否可重入，Backend Session 的
`Concurrency()` 声明具体运行时资源能力，Pipeline 以二者更严格的值调度。
`ModelRuntimeFactory` 会将 Model 要求写入 `BackendLoadSpec::requested_protocol`，Backend
必须在创建厂商资源前拒绝不支持的显式协议。因果协议由
`ICausalLmSession` 创建 `ICausalLmSequence`，序列自己提供 `Evaluate`
并保持其状态、模型资源和必要的执行锁生命周期。

Pipeline 配置只使用 Model/Backend 语法：

```json
{
  "model_id": "embedding_v1",
  "capability": "embedding",
  "model_type": "my_embedding_model",
  "backend": "my_tensor_backend",
  "model_path": "models/embedding/model.bin",
  "model_config": {"embedding_dim": 768},
  "backend_config": {"max_batch_size": 4}
}
```

`ModelRuntimeFactory` 会验证 Model 能力、执行协议、并发模型与配置字段，
再把构建好的 `IModel` 原子注册到 `ModelManager`。参考实现：
`src/engine/models/bge_embedding/` 与 `src/engine/backends/onnxruntime/`。

---

## 5. 验证与交付

开发中运行最小相关测试，交付前运行一次 `./scripts/run_all_tests.sh`。是否需要 RFC、
Changelog、PR 或合并，以及对应授权边界，统一遵循
[`CONTRIBUTING.md`](../CONTRIBUTING.md)，本指南不维护第二套流程。
