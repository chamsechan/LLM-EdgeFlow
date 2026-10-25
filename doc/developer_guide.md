# LLM-EdgeFlow 开发者扩展指南

按下表定位扩展责任。入门教程见[文档目录](README.md)，架构依赖见[架构指南](architecture.md)，
开发与交付流程见 [CONTRIBUTING](../CONTRIBUTING.md)。接口签名以头文件和当前实现为准。

## 按职责选择扩展入口

| 架构层 | 新增什么？ | 核心修改文件 | 关键宏 / 核心类 |
| :--- | :--- | :--- | :--- |
| **接入适配层（Integration）** | 新增输入/输出结构、转换器与 I/O 参数 | `include/platform_mock/operator_data_types.h`<br>`src/adapter/input/<biz>_input.cpp`<br>`src/adapter/output/<biz>_output.cpp` | `InputConverterDefinition`<br>`OutputConverterDefinition`<br>`REGISTER_INPUT_CONVERTER`<br>`REGISTER_OUTPUT_CONVERTER` |
| **流程编排层（Orchestration）** | 扩展配置校验、调度、类型黑板与会话资源 | `include/core/pipeline_validator.h`<br>`include/core/pipeline.h`<br>`include/core/alg_context.h`<br>`include/core/session_context.h` | `ValidatedPipelinePlan`<br>`AlgContext::Read/Publish`<br>`SessionResourceKey<T>` |
| **能力节点层（Capability Nodes）** | 新增通用操作或可跨方案复用的领域算法 | `src/common_nodes/*.cpp`<br>`src/custom_nodes/*.cpp`<br>`include/nodes/*.h` | `MakeNodeSpec`<br>`REGISTER_FUNCTION_NODE(NodeName, spec)` |
| **模型执行层（Model Execution）** | 新增模型语义或接入新推理后端 | `include/engine/model_interface.h`<br>`include/engine/backend_interface.h`<br>`src/engine/models/`<br>`src/engine/backends/` | `REGISTER_MODEL_WITH_DEFINITION`<br>`REGISTER_BACKEND_WITH_DEFINITION`<br>`ModelRuntimeFactory`<br>`FixedBatchExecutor` |

---

## 1. 接入适配层：Operator 与业务转换

完整请求和响应由 Converter 处理；平台载体与内存布局由 binding 处理，Demo 负责调用和展示。
按实际改动查阅对应指南：

| 修改内容 | 归属文档 |
| --- | --- |
| 新业务字段、InputConverter、OutputConverter 与 Demo 接入 | [业务接入](dev_guide/business_onboarding.md) |
| 宿主类型、输出布局、容量与调用生命周期 | [输出池与宿主接口](dev_guide/operator_output_allocation.md) |
| Runtime、协议壳与公开 ABI 边界 | [接入架构](architecture.md#统一-operator-门面与运行时架构) |
| 公共头、内部头与构建组织 | [源码布局](dev_guide/source_layout.md) |
| 部署及模型文件路径 | [配置路径](../configs/README.md#配置路径) |
| 错误映射与日志 | [错误处理](dev_guide/error_handling.md) · [日志接口](logging.md) |

## 2. 流程编排层：Pipeline 与静态校验计划

流程编排层管理请求数据和 DAG 执行计划：

- **`ValidatedPipelinePlan`**：`PipelineValidator::ValidateAndPlan()` 从 `inputs` 引用推导数据依赖。
  `depends_on` 补充执行顺序。Validator 校验并排序后生成不可变计划。
  `Pipeline::BuildFromPlan()` 直接使用该计划。Node 支持代码只依赖 `ValidatedNodePlan`。
- **`BlackboardKey<T>`**：强类型黑板键，各节点通过 `AlgContext::Read` 与 `Publish` 读取不可变输入并发布新值。
- **`AlgContext` 并发契约**：输入使用 `Read` 获取只读快照，输出通过类型端口单次
  `Publish`。聚合行为由专用 Node 读取上游端口并
  发布新的输出 key，不原地修改已经发布的值。
- **`SessionResourceKey<T>`**：会话级共享资源必须使用带静态类型的 key。动态资源名也要先
  构造类型化 key。相同名称只能绑定同一种 `T`，类型不匹配会抛出 `std::logic_error`，
  Core 的 `SessionContext::GetOrCreateResource` 对同名同型资源提供 single-flight 创建。同一次创建中的等待者
  共享结果或异常。失败不进入缓存，后续调用可重试。
- **`PipelineCatalogSnapshot`**：需要跨多次查找保持一致视图时先调用 `Snapshot()`。普通
  `Nodes/Models/Backends/FindNode/FindModel/FindBackend` 返回独立值，不保存指向 Catalog 内部容器的引用或指针。

Core 的 `ValidationReport` 只携带中立诊断、校验事实和执行顺序。CLI 在自己的
`ToolValidationReport` 中附加原因说明和已验证的 JSON Patch。修复数据类型与序列化
均属于 `src/cli/`，不编译进 SDK。默认值审计与输入参数校验共用值校验逻辑。

Validator 为未知节点类型、模型类别和 Backend 提供可定位诊断。模型按类别和 Backend 协议
选择唯一实现，无法解析时优先报告根因。未知生产者节点类型不引出额外的缺失端口误报。
已知来源的端口不存在、类型或生命周期不符，以及模型能力不符仍会报告。
输出边界缺失生产者使用 `MISSING_OUTPUT_PRODUCER`。

Node 作者声明 `InputsOf` / `OutputsOf`，算法接收只读输入并返回结果。`AuthorNode` 负责
绑定和 `Read/Publish`，无需在业务函数中管理黑板、锁或快照。
节点只使用 `type`、`name`、`params`、`inputs` 与 `depends_on`。必需输入显式连接，
可选输入省略即未连接。所有输出由 `节点名.端口名` 唯一确定，只有被下游或输出 Converter 引用的输出才绑定并发布。
Pipeline 仅通过 `max_parallel_workers` 控制并发上限，范围为 1–64，默认 1。
线程池创建失败时先停止并回收已创建的线程，再将异常交给 Pipeline 的失败诊断路径。

---

## 3. 能力节点层：通用与自定义 Node

所有 Node 使用普通函数与同一 Spec 结构。领域算法放在 `src/custom_nodes/`，中立操作放在
`src/common_nodes/`，均可跨方案复用。先查询目标构建的 Catalog，确认是否已有可用能力。

- 首次实现：[自定义 Node 练习](dev_guide/first_custom_node.md)。
- 端口、参数、模型、批次来源及并发：[Node 契约参考](dev_guide/custom_node_concepts.md)。
- 运行中参数更新：[Control 练习](dev_guide/first_control.md)。
- 源码收录、脚手架、快速测试和完整样例：[源码目录说明](../src/custom_nodes/README.md)。

## 4. 模型执行层：如何新增模型语义或推理 Backend

模型执行层必须保持两个独立扩展面：

- **Model** 实现 Embedding/Rerank/LLM/OCR/ASR 语义，只依赖
  `ITensorGraphSession`、`ITextGenerationSession`、`IImageTextGenerationSession` 或
  `IGeneratedTokenEmbeddingSession` 等中立协议。
  `generated_text_embedding` 负责生成 token 向量的池化与归一化。Backend 只返回原始向量。
  `vision_document` 将 RGB8 像素帧预处理、补边和识别语义封装在 Model，Kite 类型仍只在 Backend 内出现。
- **Backend** 封装 Inferlet、ONNX Runtime、llama.cpp、Kite、whisper.cpp 等运行时，加载后返回
  `IBackendSession`，不实现业务模型语义。

更换兼容权重、参数或已支持的设备，先调整配置。只有现有实现缺少所需模型语义时才新增 Model；
只有需要接入新的厂商运行时或执行协议时才扩展 Backend。模型语义和厂商资源分别归属两者。

Model 自注册需实现 `IModel` 的某一强类型能力并声明所需协议。Backend 实现
`IInferenceBackend` 并只返回中性 `IBackendSession`。二者分别提供完整
`ModelDefinition` / `BackendDefinition` 并使用对应 `REGISTER_*_WITH_DEFINITION` 宏。
Model 继承 [`ModelIdentity<Model, 能力接口>`](../include/engine/model_identity.h)，只声明
`kImplName` 与 `kConcurrency`，能力由接口推导。Definition 用 `MakeModelDefinition<Model>()`
取得同一身份后再补协议和配置字段。Backend Provider 同样继承
[`BackendIdentity<Backend>`](../include/engine/backend_identity.h)，Definition 从
`MakeBackendDefinition<Backend>()` 开始。创建时工厂仍逐项核对实例、Session 与 Definition。

Model 和 Backend 按以下步骤声明参数：

1. 定义 `Params` 结构。
2. 编写返回 `Parameters<Params>` 的 `ParamSpec()`。
3. 设置 `def.params = ParamSpec()`。
4. 用 `ctx.Params<Params>()` 或 `spec.Params<Params>()` 读取参数。

`Field` 声明类型、默认值、范围与说明，默认值只写一次。
额外纯参数规则放在 `ParamSpec().Validate`，不读文件或创建会话。
Validator 规划时检查字段与语义。ModelRuntimeFactory 在创建 Provider 前分别解析两组
参数一次。Model `Create` 和 Backend `Load` 读取只读值，只检查真实会话、资源和形状。
直接调用创建入口的测试先用注册 Definition 的 `params.Parse` 产生对应参数。

可选标量成员使用 `std::optional<T>`：不写时为空，不能声明 `Required()` 或 `Default()`，
Catalog 中非必填且无默认值。BGE 的维度和编码长度通过
[`ResolveFromModel`](../src/engine/models/common/from_model.h) 从固定形状读取。配置与模型值
冲突时报错，动态编码长度回退到 512，动态向量维度必须填写。创建日志记录最终值。

逐项推理使用 `FixedBatchExecutor::ExecuteItems`：回调只接收一个
`TraceableItem<Input>` 和 `Output*`，返回状态码。框架循环调用、保留 `(req_id, sub_id)`，
任一项失败或抛异常就清空整批输出。回调可捕获本次调用的 options 和 diagnostic，不能保存请求引用。
标准异常和未知异常分别返回框架 `INTERNAL_EXCEPTION`、`UNKNOWN_EXCEPTION`。
主动失败保留框架原因码，详见[错误码与日志](dev_guide/error_handling.md)。
模型仍负责资源准备、协议检查、输入输出语义与必要的整批预检，例如 Whisper 在首次推理前检查全部音频。
固定 Tensor 路径继续使用 `FixedBatchExecutor::Execute` 显式准备补齐批次。`ExecuteItems` 拒绝固定批策略。
参考 Qwen 的 `GenerateOne` 和 VisionDocument 的逐项回调，无需为普通循环新增模型基类。

固定批次的补齐内容由 Model 回调按 `BatchSlice::execution_count` 构造。执行器校验返回数量、
剥离补齐项并恢复原始 `(req_id, sub_id)`。任一切片失败或抛异常，整批输出清空，不能暴露
之前切片的部分结果。具体接口见 [`FixedBatchExecutor`](../include/engine/fixed_batch_executor.h)。
Tensor 协议的共享 buffer 必须覆盖执行和结果解码的生命周期。Model 使用
[`CreateHostTensor` 与类型化访问检查](../include/engine/tensor.h)，拒绝运行时未解析的负维度、
字节数溢出、类型不匹配或存储字节数与形状不符。厂商内存及释放方式封装在 Backend 中。

Embedding 的 `normalize` 是模型参数（默认 true）。同一模型的全部消费节点共享该向量空间。
`Embed` 不接收选项。生成的 `system_prompt`、`random_seed` 和转写的 `language` 是节点调用参数，
通过 `GenerateOptions` / `TranscribeOptions` 传入模型。节点可用 `ValidateModels` 在绑定后核对要求，
只在 Create 执行一次。不支持转写语言时节点初始化失败。后端不支持固定 `random_seed` 时，
文本 Model 在每次非空 `Generate` 调用记录一条 warning，不向后端传递种子并继续生成。

文件字段使用 `.File()`，只接受字符串或可选字符串。BGE 的 `tokenizer_file` 必填，
Kite 和 Inferlet 的 `run_config_file` 不写时为空。接入层在 Core 校验前相对 Pipeline JSON 目录解析：
拒绝空名、绝对路径、盘符、UNC、任何 `..` 分量和符号链接越界，不检查文件存在。
目录未知的内存文档只校验写法。Model/Backend 拿到已解析路径，实际打开时检查资源。
Kite 和 Inferlet 的 `vision.mmproj` 相对 run config 所在目录使用相同规则。批策略只取自 Backend Session。

`inferlet` 使用独立的 Inferlet C API 绑定，提供文本生成、图像文本生成、生成 token
向量三个中性协议。会话按请求串行执行，批策略为单输入、非固定批；模型和输出资源由
会话及调用内 RAII 持有，输出在释放厂商对象前复制。文本走 token 输入以保留 Model
已格式化的提示词；框架 `top_k=0` 映射为不截断候选，不使用 Inferlet 的默认 top-k。
当前不支持每请求固定随机种子（配置后告警并忽略）。CPU 包仅接受自动选择或设备 0；
运行配置和设备约束在 Backend 加载时核对。构建和运行方法见 [Inferlet 验证](VERIFIABLE_SELECTION.md#inferlet-cpu-验证)。

后端跨字段约束同样用 `ParamSpec().Validate`，例如 llama.cpp 的 `decode_batch_size` 不得大于
`context_size`。依赖其他字段的默认值在先于 `Validate` 执行的 `ParamSpec().Prepare` 中补齐，
例如 `decode_batch_size` 未配置时取 `min(512, context_size)`；`resolve-conf` 展示补齐后的生效值。
环境、设备和资产可用性仍由实际加载路径检查。

其中，Model 的 `Concurrency()` 只声明语义对象是否可重入，Backend Session 的
`Concurrency()` 声明具体运行时资源能力，Pipeline 以二者更严格的值调度。
`ModelRuntimeFactory` 会将 Model 要求写入 `BackendLoadSpec::requested_protocol`，Backend
必须在创建厂商资源前拒绝不支持的显式协议。文本生成协议接收 Model 已格式化的 prompt、
`add_bos`、统一采样参数和可选 seed。暴露 logits 的 Backend 通过 Backend 私有
`IAutoregressiveDecoder` 复用 `CommonAutoregressiveGenerator`，托管生成 Backend 可直接
实现会话。decoder 不进入 Catalog，vendor 类型不得离开 concrete Backend。

Pipeline 的模型条目如下：

```json
{
  "type": "embedding",
  "name": "embed_model",
  "file": "embedding/model.bin",
  "params": {"tokenizer_file": "embedding/vocab.txt"},
  "backend": {"type": "onnxruntime", "params": {"max_batch_size": 4}}
}
```

`type` 是模型类别。`ModelDefinition.impl_name` 是 C++ 实现名。注册表按类别与后端支持的
协议选出唯一实现，`GlobalInit` 审计每个组合最多一个实现。Core 也独立拒绝歧义。
节点 `bind_model` 显式引用 `models[].name`，类别由所实现的接口核对。
未被节点使用的模型报 `UNUSED_MODEL`。存在未知节点类型时抑制这条连带诊断。
测试模型要求 `kFixture`，只匹配 `fixture_backends` 中的后端。测试后端仅声明该协议。

`ModelRuntimeFactory` 会验证 Model 能力、执行协议、并发模型与配置字段，
返回构建好的单个 `IModel`。Pipeline 暂存全部模型，全部构建成功后通过
`ModelManager::RegisterBatch` 完成会话内的批量原子注册。参考实现：
`src/engine/models/bge_embedding/` 与 `src/engine/backends/onnxruntime/`。

---

## 5. 验证与交付

开发中运行相关测试。本地交付前，执行 `./scripts/run_all_tests.sh`。
如果用户已授权 PR 交付，使用交付脚本执行同一门禁。
设计、文档更新和交付规则见 [CONTRIBUTING.md](../CONTRIBUTING.md)。
