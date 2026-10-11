# 测试编写与归属

测试文件按验证职责组织，CTest 标签决定何时、在哪种构建中执行。
本指南维护新增、复用和合并测试的规则；开发流程与完整交付门禁见
[CONTRIBUTING](../CONTRIBUTING.md)。

## 先确定测试归属

| 要验证的行为 | 归属 | 当前参考 |
| --- | --- | --- |
| AuthorNode 执行、参数绑定、通用批次操作 | `unit/nodes/` 的机制文件 | [FunctionNode](unit/nodes/test_function_node.cpp)、[参数绑定](unit/nodes/test_parameter_binding.cpp) |
| 具体 Node 的算法、业务参数、拆分/聚合/排名语义 | `unit/nodes/test_<operation>_node.cpp` | [ASR](unit/nodes/test_asr_transcribe_node.cpp)、[OCR](unit/nodes/test_ocr_detect_node.cpp) |
| 多个具体 Node 接入同一模型输出契约 | `unit/nodes/test_node_model_contracts.cpp` | [真实 ASR/OCR/Embedding/LLM 注册的类型化用例](unit/nodes/test_node_model_contracts.cpp) |
| Converter 注册、端口、容量声明 | `unit/adapter/test_converter_contracts.cpp` | [独立预期数据表](unit/adapter/test_converter_contracts.cpp) |
| 转换辅助机制、具体字段转换 | `unit/adapter/` 中对应机制或转换器文件 | [行转换与 binding](unit/adapter/test_io_converters.cpp)、[业务转换](unit/adapter/test_business_converters.cpp) |
| 输出池、值类型等 Operator 内部机制 | `unit/operator/` | [输出池](unit/operator/test_operator_output_pool.cpp) |
| 完整业务请求经过 Operator 后形成响应 | `integration/operator/test_<biz>_operator.cpp` | [文档问答](integration/operator/test_doc_qa_operator.cpp)、[音频](integration/operator/test_audio_asr_intent_operator.cpp) |
| Operator API 与 I/O 参数的跨组件行为 | `integration/operator/` 中对应接口文件 | [Operator API](integration/operator/test_operator_api.cpp) |
| 并发、生命周期、热更新 | `integration/runtime/` | [句柄独立性](integration/runtime/test_operator_lifecycle.cpp) |
| DAG、Validator、跨组件连接 | `unit/core/` 或 `integration/pipeline/`，按实际边界选择 | [验证计划](unit/core/test_validated_pipeline_plan.cpp)、[Pipeline 契约](integration/pipeline/test_pipeline_catalog_validator.cpp) |
| Model 语义、Backend 协议与资源 | `unit/engine/` | [Model/Backend 解耦](unit/engine/test_model_backend_decoupling.cpp) |
| 公共 ABI、Catalog、编译诊断、架构规则 | `contract/` | [作者签名契约](contract/authoring/test_spec_signature_diagnostics.cmake) |
| CLI、HTTP、浏览器、文件事务、脚手架 | `tooling/` | [CLI](tooling/test_pipeline_cli.py)、[Studio](tooling/test_pipeline_studio.py) |
| 真实模型与目标环境的执行证据 | `e2e/`，按需启用 | [真实模型](e2e/real_models/test_real_models_e2e.cpp) |

同一业务的普通、批量、失败和连续调用场景归入同一个业务文件，保留能说明差异的用例名称。
`integration/runtime/` 专注运行机制；不要再新增“所有业务”“不同模态”这样的混合业务集合。
一份文件应有明确的验证职责，不要求一文件一用例，也不为减少文件数把不同职责合并。

## 什么时候新增，什么时候复用

| 变化 | 做法 |
| --- | --- |
| 执行步骤和断言相同，仅输入、预期不同 | 在原用例增加带名称的数据行，或使用参数化测试；失败必须指出案例 |
| 新的前置条件、分支、错误或生命周期顺序 | 在责任文件新增独立用例；不要放进无关的巨大循环 |
| 新组件、新业务或现有文件不拥有的独立职责 | 新增对应 `test_*.cpp` 文件，进入现有 runner |
| 新框架机制或公共规则变更 | 在机制责任文件维护完整规则矩阵，再验证受影响组件的代表性接入 |
| 新 Node 复用已有机制 | 用现有 Harness 测实际算法与组件声明，按适用性接入公共契约 |
| 新 Converter 或外部业务协议 | 测具体转换语义，并在 Operator 边界验证完整原始请求与响应 |
| 已有组件的新 Pipeline 组合 | 验证实际配置、连接与代表性结果，复用组件已有的算法矩阵 |
| 回归缺陷 | 在能复现缺陷的最窄责任层补用例；缺陷跨层时补对应集成路径 |
| 进程隔离、独立运行生命周期或不兼容依赖不可省略 | 才新增可执行目标；注册表冲突与可选 E2E 保持显式登记 |

增加测试前，先检查现有责任文件及 [RuntimeTests.cmake](RuntimeTests.cmake)。
普通数据变更不需要新 Fixture；新文件不自动意味着新 runner。
不按文件长度机械拆分，也不设删除用例数量指标。

## 三种复用及其覆盖边界

| 复用对象 | 提供什么 | 作者仍需证明什么 |
| --- | --- | --- |
| `support/` 中的 Harness、Fixture、辅助函数 | 上下文、输入准备、模型注入、资源清理 | 实际调用和独立预期断言；使用工具本身不是覆盖 |
| 框架公共机制测试 | 公共实现的完整规则矩阵 | 具体组件的声明、绑定和代表性行为确实接入该机制 |
| 针对真实组件的公共契约用例 | 对每个列入的组件实际执行相同契约 | 组件算法及公共契约不适用的独有语义 |

[NodeModelContractTest](unit/nodes/test_node_model_contracts.cpp) 对真实 ASR、OCR、Embedding 和 LLM
注册分别运行同一套契约：缺失/错误类型输入不调用模型、空批次跳过推理、模型输出数量及
`req_id` / `sub_id` 错误时不发布任何输出、失败诊断保留且同一实例可重试。
类型案例声明配置、输入与输出类型；局部模型探针只注入契约故障，不模拟业务结果。
新增案例前确认契约相同；已接入的组件文件不再另写这些基本场景。
不要将拆分、聚合、过滤、排名强塞进保序输出契约，或为不适用的组件增加大量分支开关。

完整规则矩阵留在其实现所属层：

- 参数类型、默认值、范围解析由参数机制测试完整覆盖；Node 验证自己的业务约束、参数传递和效果。
- 框架验证保序输出数量和来源；具体 Node 保留接入证据。派生数量、排名和来源关系由具体算法测试负责。
- Converter 验证字段与容量；Operator 验证完整载体、槽位连接、逐行结果和状态，不能用内部端口测试替代。
- CLI/HTTP 使用代表性输入检查状态、诊断路径和序列化，并测试各入口独有行为，不复制整个 Validator 矩阵。
- 组件使用或修改 Control、缓存、借用内存和并发能力时，补对应回滚、失效、生命周期和并发证据。

预期数据独立于被测实现；不要从生产 Definition 读取实际值后再把它当作全部预期。
辅助函数封装准备与清理，业务输入、命名槽位和关键输出断言保持可见。
相同 DTO、相同返回码或相同 Pipeline 名称不足以证明两个场景重复。

## 全框架的唯一测试责任

“一套”指每项契约只有一套主要验证实现，不是把所有测试装进一个文件或一个端到端测试。
下表中的机制矩阵、真实组件接入和跨层场景验证不同边界，不能互相替代。

| 契约 | 主要责任入口 | 其他层保留什么 |
| --- | --- | --- |
| Blackboard 类型、发布、来源、值生命周期 | [TypedBlackboardContractsTest](unit/core/test_typed_blackboard_contracts.cpp) | Node/Adapter 的实际端口与来源关系 |
| Session 资源与模型管理 | [FrameworkCoreTest](unit/core/test_framework_core.cpp)、[ModelBackendDecouplingTest](unit/engine/test_model_backend_decoupling.cpp) | 模型原子提交、缓存失效和组件共享资源的特有路径 |
| 参数声明、解析与 Definition schema | [ParameterBindingTest](unit/nodes/test_parameter_binding.cpp)、[DefinitionSchemaValidationTest](unit/core/test_definition_schema_validation.cpp) | 各组件自己的参数值、组合约束和参数传递；不复制通用类型/范围矩阵 |
| DAG 规划、构建状态与执行 | [ValidatedPipelinePlanTest](unit/core/test_validated_pipeline_plan.cpp)、[PipelineConfigTest](unit/core/test_pipeline_config.cpp)、[DagPipelineTest](unit/core/test_dag_pipeline.cpp) | 工具对诊断的传输、建议与修复，Operator 对错误的边界映射 |
| AuthorNode、批次操作、Control 快照 | [FunctionNodeTest](unit/nodes/test_function_node.cpp)、[TraceableBatchOperationsTest](unit/nodes/test_traceable_batch_operations.cpp) | 具体算法、参数、缓存、Control 效果及并发回归 |
| 保序模型 Node 的基本运行契约 | [NodeModelContractTest](unit/nodes/test_node_model_contracts.cpp) | 业务结果；Embedding 会话缓存、LLM 多端点、OCR 派生文本仍由各自文件验证 |
| Converter 声明与字段转换 | [ConverterContractsTest](unit/adapter/test_converter_contracts.cpp)、[BusinessConvertersTest](unit/adapter/test_business_converters.cpp)、[TextConvertersTest](unit/adapter/test_text_converters.cpp) | Operator 只补完整请求/响应组装和外部边界 |
| 行转换、平台 binding、选择和容量 | [IoConverterTest](unit/adapter/test_io_converters.cpp)、[IoConverterRegistryTest](unit/adapter/test_io_converter_registry.cpp)、`unit/operator/` | 业务独有的字段长度、合流规则、载体接线；不复制基础 helper 矩阵 |
| 完整业务请求/响应 | `integration/operator/test_<biz>_operator.cpp` | `test_operator_api.cpp` 保留公共 API、池、批次硬限额和服务映射，不再维护各业务成功链路 |
| 全局生命周期、并发与热更新 | `integration/runtime/` | 不用一次成功请求替代关闭顺序、输出租约、竞争或故障恢复测试 |
| 固定/逐项批执行、padding、失败回滚 | [BatchExecutorTest](unit/engine/test_batch_executor.cpp) | 每个 Model 如何使用 Session 策略、构造 Tensor 和解释输出 |
| Model 与 Backend 选择、加载、协议、语义 | `unit/engine/`、[ModelBackendPipelineTest](integration/pipeline/test_model_backend_pipeline.cpp) | BGE 池化与评分、Qwen 生成、图像预处理等不同语义分别测试；真实 Provider 的执行不能由 mock 替代 |
| 仓库配置有效性 | [PipelineValidatorTest](integration/pipeline/test_pipeline_catalog_validator.cpp) 与 [PipelineConfigTest](unit/core/test_pipeline_config.cpp) | 前者检查工具验证/诊断，后者检查可构建的部署；查看器只保留代表性启动与渲染契约 |
| CLI、HTTP、浏览器、Demo、脚手架 | `tooling/`、`integration/demo/` | 各入口的参数、序列化、HTTP 状态、文件事务、进程退出和生成代码执行；不重抄底层规则矩阵 |
| ABI、编译隔离、注册冲突、工程门禁 | `contract/` | 保留必要的独立进程和编译负例；运行时成功无法证明这些契约 |
| 真实模型、sanitizer、目标硬件 | `e2e/` 与对应构建/执行入口 | 这是不同环境的证据，不是可以删掉的重复 suite |

### 删除旧测试的条件

新增或替换测试与删除被替代测试在同一变更完成，不并存“新版本”和“历史版本”。
逐项比较以下证据，而不是只比较名称或返回码：

1. 实际被测实现和入口相同，或者保留测试明确覆盖旧入口的独有职责。
2. 输入形态、边界条件、预期输出和故障触发条件都有归属；只有数据不同则并入命名数据行。
3. 失败后的无残留、诊断、重试、资源归还、同一实例连续调用等顺序契约没有丢失。
4. 旧测试独有断言先迁入保留测试，移除旧 fixture/helper、CTest filter 和文档引用。
5. 实际执行保留测试并核对发现清单、过滤器和标签；空匹配不算通过。

历史用例数量不是验收下限，代码行数也不是删除配额。对没有独有覆盖的旧冒烟测试直接删除；
对框架已承接的规则不再保留业务副本。只检查自己编写的假实现分支、未触达被测框架的
“故障注入”不构成框架证据，应改由实际公共执行器或真实组件契约验证。
测试通过证明本次执行结果；全框架责任清单不等于穷举组合，也不证明未运行的真实 SDK/硬件。

## 常见开发路径

**新增 Node：** 在 `unit/nodes/test_<operation>_node.cpp` 使用
[NodeHarness](support/node_harness.h)，执行真实 Node，检查业务结果、特有参数和必要的多请求/来源关系。
`InitNodeForTest` 会先准备已验证计划，因此它对非法配置的拒绝属于准备阶段覆盖，不能当作 `Run` 已执行。
直接测试非法计划或初始化机制时，使用对应低层入口。

脚手架 `--write-test` 生成的用例进入 `CustomNodeBehaviorTest.<node>_*`，CTest 同名入口负责执行。
生成的样例验证起步实现；更改算法后填写自己的独立预期。注册成功不等于算法正确。
[类型化公共用例](unit/nodes/test_node_model_contracts.cpp) 是适用时的参考，不要求每个 Node 实现通用测试适配器。
作者 API 示例和文档原样代码仍归 [authoring examples](unit/nodes/test_node_authoring_examples.cpp)。

**新增 Converter：** 用 [AdapterHarness](support/adapter_harness.h) 或现有
`DecodeForTest` / `EncodeForTest` 验证真实 binding 下的字段与容量。外部协议变化还需对应业务
Operator 用例，覆盖完整请求/响应以及契约要求的无效输入、逐行关系和失败无残留。
批次硬限额等通用机制在其责任文件维护矩阵，业务入口保留相关接入证据。

**新增业务组合：** 在 `integration/operator/test_<biz>_operator.cpp` 继承
[OperatorTestFixture](support/operator_test_fixture.h)，并用 `ScopedTestOperator(ops_)` 管理句柄。
`Create(conf)` 默认 root `.`、CPU、depth 25、device 0；按场景显式覆盖并断言返回值及诊断。
在 outputs 之前声明句柄，保证致命断言提前返回时先释放输出租约。显式 `Close()` 前等待调用完成，
释放全部输出引用；断言关闭状态。只有外层 Fixture 管理全局 `Init/DeInit`。
容量、并发或销毁本身的契约测试保留对相关 API 的显式控制。

参考 [音频表驱动业务断言](integration/operator/test_audio_asr_intent_operator.cpp) 与
[Rerank 局部资源 Fixture](integration/operator/test_cross_rerank_operator.cpp)。
一个文件使用的 helper 留在本地；多个实际调用方共享相同语义时才放入 `support/`。

## 注册、运行与迁移验收

`RuntimeTests.cmake` 按 runner 所属目录非递归收集 `test_*.cpp`，增删文件在下次构建时重新配置。
新子目录需要显式调整收集范围；跨 runner 的 Pipeline 文件、生成源码、进程隔离及可选 E2E 保持显式登记。
目录收集兼容 CMake 3.19。PCH 可选，规范门禁默认关闭。

CTest 名称、过滤器和标签保持显式。新 suite 必须加入匹配的 filter；类型化/参数化名称也必须匹配。
业务 Operator 测试属于 integration/tier3，并保留 `dev-fast`、`sanitizer-compatible`、`sanitizer-runtime`。
`TestLabelsContractTest` 检查已编译用例至少被一个 CTest 入口执行；必需清单也会在配置时检查。
迁移时还要核对过滤器是否意外重叠，不能仅以“编译成功”或“零条用例也返回成功”为通过。

开发时构建并执行受影响套件，例如：

```bash
cmake --build build --target edgeflow_test_nodes_runner edgeflow_test_adapter_runner -j 4
(cd build && ctest --output-on-failure --no-tests=error -R '^(NodeModelContractTest|AsrTranscribeNodeTest|OcrDetectNodeTest|CustomNodeBehaviorTest|AudioAsrIntentOperatorTest)$')
```

用 `./build/edgeflow_test_nodes_runner --gtest_list_tests` 查看实际名称。
其他变体须始终使用对应目录的构建与执行文件。最终门禁只遵循
[CONTRIBUTING 的统一入口](../CONTRIBUTING.md#6-run-one-canonical-delivery-gate)。

重组或删除测试时，在变更说明记录旧场景到新场景的对应关系，检查输入条件、断言强度、调用顺序、
故障恢复、资源隔离与标签。确认独有行为仍有覆盖，再删除旧入口；不保留空 suite 或过时兼容别名。
普通、sanitizer 和真实模型验证提供不同证据，不能通过取消其中一种来宣称去重或提速。

## 测试资产与故障注入

`fixtures/` 保存稳定测试数据；`support/` 不注册生产能力。四种 Adapter 解析示例在
[support/adapter_examples](support/adapter_examples/flat_struct_adapter.h)，由 `AdapterContractSecurityTest` 编译。
Demo mock profiles 与测试共用的确定性 Model/Backend 注册在 `dev_support/inference/`，以 OBJECT 目标链接，
不进入生产 `alg_sdk`。

分配故障使用 [scoped_allocation_failure](support/scoped_allocation_failure.h)，仅链接 Core/Model、Node、Adapter
测试 runner，SDK、工具和 Demo 使用普通分配器。注入为线程局部、一次触发、作用域恢复。
只在同步操作周围启用注入，GoogleTest 断言放在注入作用域之外；扫描分配位置直到成功且未触发故障，
验证回滚、可观察状态和重试，不固定容器名称或分配次数。检查泄漏时在作用域内销毁操作资源，再验证
`!Overflowed()` 和 `Outstanding() == 0`。账本只观察当前线程的 C++ 分配，不覆盖 `malloc` 或其他线程；
sanitizer 提供补充证据。ELF 的 `posix_memalign` 包装仅用于现有 Model/Backend runner。

`QualityGateScriptsContractTest` 维护规范门禁与 sanitizer 脚本、缓存测试开关、空测试拒绝、失败传播及 CI 证据契约。
