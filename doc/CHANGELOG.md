# Changelog

## Unreleased

`src/engine/models/`、`src/engine/backends/` 下的 `.cpp` 自动编入，新增 Model/Backend 不再修改 CMakeLists。

新增宿主结构时结构名只写一次，并补上槽位结构核对：`MakePooledOutputBinding<T>` 与 `MakeTypedInputBinding<T>`
删除结构名参数，改从 `DECLARE_EXTERNAL_TYPE_TRAITS` 取名，未声明 trait 时编译失败（trait 模板与宏移到
`adapter/operator_io_contracts.h`，平台类型的声明仍在 `io_converter.h`）。Init 时 IoBinding 审计新增检查：槽位声明的
宿主结构须与其后缀登记的 ValueType 结构一致，否则报错并指明槽位；此前只检查后缀已登记，写错后缀时会按另一结构的
布局读写内存。仓库内全部生产登记原本一致；嵌套输出测试夹具的槽位类型改为实际登记的 `NestedOutputEnvelope`。

"LLM 节点加自有参数"不再需要手写解析器：新增重载 `GenerateParameters(默认 max_tokens, &Params::generation,
{Field(...)})`，生成参数字段与默认值同原重载，自有字段照常用 `Field` 声明。`PromptGuidedLlmNode` 改用该重载，
模板解析移到 `Prepare`，配置字段名、类型、默认值与报错不变，Catalog 中 `strip_markdown` 的显示位置随自有字段前移；
自定义 Node 概念文档的示例改为同一写法并有编译测试。输出分配指南与平台模拟说明补充暂缓项的触发条件：接入第一个
生产命名方案时增加槽位可接受方案的校验和 Catalog 列表；内网接入时核对 `meta_num`、`metadata_type_id`、`key_suffix`。

删除 ABI 版本：去掉 CMake 的 `LLM_EDGEFLOW_ABI_VERSION` / `_MAJOR`、生成头 `edgeflow/version.h` 的
`COMPANY_ALG_ABI_VERSION` / `COMPANY_ALG_ABI_VERSION_MAJOR`、共享库的 `VERSION` / `SOVERSION` 与导出符号的
版本节点 `LLM_EDGEFLOW_9`。构建产物改为单个 `libcompany_alg_sdk.so`（SONAME 同名），导出符号名称和数量不变，
链接旧库的宿主需重新链接。产品版本 11.0.0 仍是唯一的版本标识；导出白名单与公开结构布局检查保持不变。

输出分配的扩展点按"只新增"整理：删除只有一个 JSON 实现、仅用于读取 `params` 的 `OutputConfigReader`
（及 `OutputConfigField`、公开头 `adapter/operator_output_config.h`），`OperatorConfigResolver` 直接把
`params` 原样序列化后交给所选实现，未配置时仍为 `{}`。命名分配方案、`params`、容量与 metadata 字段保持不变；
Pipeline JSON、`.conf`、SDK 接口与 `resolve-conf` 输出不变。输出分配指南新增扩展方式表，统一说明平台宿主类型
集中登记（结构、traits、ValueType 一一对应）、已有类型的新布局以命名方案在自己的文件中新增，以及转换器须核对
所选方案。

一个 Control 命令 ID 只属于一个节点类型：删除 `ControlCommandDefinition::shared_id` 与
`FieldControlCommand::SharedId()`，注册表对任何跨类型重号一律拒绝，Catalog 的控制命令不再导出
`shared_id`。原有的两处设置（`TextRuleMatchNode`、`TextTemplateNode`）并没有与其他类型共用 ID。

删除 Converter 的 `schema_id`：每个业务只有一个绑定后，它唯一的比较逻辑已随多绑定比较一起删除，
此后只做非空检查和展示；解析方式由绑定选中的 `converter_id` 决定，外部契约由业务名确定。Catalog 的
Converter 与 Pipeline Studio 不再显示该字段；Operator ABI、Pipeline JSON 与 `.conf` 不变。

`StructuredJsonParseNode` 的参数结构移回源文件的匿名命名空间，避免与其他编译单元中同名的
`Params` 冲突；删除 `Field` 中与 `Description()` 重复的 `Semantic()`；自定义 Node 概念文档直接给出
"LLM 节点加自有参数"的写法示例。

所有 Node 只保留一种写法：`MakeBatchSpec` 改名为 `MakeNodeSpec`，删除 `MakeMapSpec` 与
`MakeLlmTextSpec`；每个 Node 统一由 `Inputs`、可选的 `Params` 与 `Models`、`Run`、`Spec` 组成，
12 个生产 Node 按此命名。`Run` 只接收 Spec 实际声明的部分（不再写 `const NoParameters&` 或
`const NoModels&`），删除成员函数形式的 `Run`；模型槽统一用 `Model(...)` 声明，删除 `Llm(...)` 与
`Embedding(...)` 别名。逐项处理改用 `MapPayloads`，失败诊断会标出失败条目的 `req_id` / `sub_id`。
LLM 入门模板与脚手架生成的 LLM 节点改为从节点配置读取 `max_tokens`、`temperature` 等生成参数
（新增 `GenerateParameters`，默认值与原来一致）。删除 Node 的 `biz_names` 限制、Catalog 中的该字段
和诊断 `NODE_BIZ_MISMATCH`。三个 Node skill 合并为 `edgeflow-node-developer`。

门禁与测试不再甄别历史名称：文档漂移检查只保留核心概念、架构图和版本一致性检查，删除旧业务名、
旧注册宏、已移除接口与路径的黑名单；LayerGuard 和治理检查删除针对已不存在头文件、接口和旧术语的
规则；固定已删除字段、命令或参数名的测试改为通用的未知字段检查或删除。元测试删除对 `ci.yml`
文本的逐字断言和 CTest 标签检查器自身的自测。

精简未使用的扩展点：批次上限只在 IoBinding 上声明，删除 Converter 的 `max_batch_size`（及
`EffectiveMaxBatchSize`），Catalog 的 Converter 不再导出该字段；字段 Control 只保留整体替换的
`ReplaceFields`，删除 `PatchFields` 及其策略枚举；端口存活期只接受 `request` 与 `session`，删除
未使用的 `global`；Catalog 与 `validate-io` 删除恒为 `operator` 的 `transport` 字段。

示例 Pipeline 配置、Demo 夹具与资产清单中的模型实例名去掉版本后缀（如 `embed_model_v1` /
`embed_model_v2` 改为 `embed_model`，`llm_model_v1` 改为 `llm_model`）；Pipeline Studio 的 HTTP 接口
由 `/api/v1/...` 改为 `/api/...`。

开发工具 JSON 不再携带 `schema_version`：`alg_pipeline_tool` 各命令输出、Catalog、校验报告与
remediation、`edit` 请求与响应、Pipeline Studio 接口、Demo 的 Profile 文件与结果文件、
`dev_recipe` / `verify_selection` 报告、效果规格、资产清单和验收证据都删除该字段及对应的版本检查；
`edit` 请求携带 `schema_version` 时按未知字段拒绝。kiteLLM 的 run config 属于第三方格式，保持不变。

IoBinding 改以业务名标识：删除 `IoBindingDefinition::binding_id`，Pipeline 的 `deployment.io.io_binding`
直接填写业务名（如 `keyword_match`），`catalog` / `init` 的 `--io-binding` 同样接受业务名。转换器 ID
去掉 `.operator.v1` 后缀（如 `text.plain`）。Catalog 的 `io_bindings` 与 `validate-io` 不再输出
`binding_id`。全部 Pipeline 配置与 Demo 夹具已同步更新。

每个业务只注册一个 IoBinding：同一 `biz_name` 的第二个 binding 在注册时被拒绝并记为注册冲突
（SDK 初始化返回 -6），删除同业务多 binding 的外部契约比对与部署诊断 `BIZ_IO_CONTRACT_MISMATCH`。
发布后外部契约不兼容的变化原地修改并随新 SDK 版本发布，新旧契约须并存时新增业务，不再新增带版本的 binding。

转换器回调去掉 `InputPortBindings` / `OutputPortBindings` 参数：`DecodeInputFn` / `EncodeOutputFn`、
`DecodeRequestRows`、`EncodeResultRows` 与 `ReadOutputValue` 直接使用端口常量读写 `AlgContext`。
写入未声明端口不再被静默发布到空键名。

删除 IoBinding 的端口改名映射（`input_ports` / `output_ports`、`BindIoPort`、`EffectivePortMapping`）：
转换器逻辑端口名即业务 Blackboard Key，注册审计直接按端口名核对业务出入口与类型；Catalog 与
`validate-io` 不再输出 `*_port_mapping`。业务接入指南补充端口命名约定。Pipeline JSON 不变。

对话合规审核的输出 Converter 直接使用业务出口键 `matched_policy`，删除只为改名存在的
`kMatchedPolicies`（`matched_policies`）及其 Binding 端口映射；Pipeline 配置与外部契约不变。

Converter 定义精简（不涉及 Operator ABI、Pipeline JSON 与 `.conf`）：删除没有运行时作用的
`schema_version`、`external_type`、输出 `cardinality` 与 `capacity_policy`，以及槽位的 `value_type` 与
`capacity_fields`。输出槽容量字段只由 ValueType 决定，`ExternalInputSlot` / `ExternalOutputSlot`
只接受槽名。Catalog 的 Converter 不再导出上述元数据，`external_type` 改由槽位类型推导。

开发工具修复：Markdown 链接检查支持单引号与圆括号标题、带空格的尖括号目标及平衡或转义的
目标圆括号；生产版 `alg_pipeline_tool edit` 根据 `validation.diagnostics` 提示构建变体与测试工具。
交付脚本要求远端 PR head 与已验证提交一致，以该 SHA 复核历史，并通过 `--match-head-commit`
阻止检查后的并发更新进入合并。Control 快照基准改用当前 `slots` DTO，仅测量当前源码，删除历史
源码对比和 `--baseline` 参数。

架构审查回归修复：Map 的回调通过移动交给运行时，支持捕获 `unique_ptr` 等不可复制状态；
流契约错误由 Validator 提供生产者、消费者、有效端口契约与推导出的数量形状，CLI 据此解释
节点输入、业务出口及 IO 边界错误，保留拆分来源并正确区分逐项配对与出口数量要求。
`ConfigValueOrDefault` 始终核对字段声明，拒绝数组和标量配置，`null` 仍按省略配置读取声明默认值。

源码扩展接口整理（随架构审查落地，不涉及 Operator ABI）：
- Model 继承 `ModelIdentity<Model, 能力接口>`、Backend Provider 继承 `BackendIdentity<Backend>`，
  身份只声明一次，Definition 从 `MakeModelDefinition` / `MakeBackendDefinition` 开始；
  配置读取使用 `ConfigValueOrDefault`，默认值只写在 `config_fields`。
- `ResolvedInputLimits` 更名为 `InputLimits` 且不再出现在部署配置中。
- 删除 `include/adapter/biz_results.h`、`ModelManager::RegisterModel`（改用 `RegisterBatch`）和
  `RuntimeOptions` 中只写不读的 `biz_type`、`depth_num`、`biz_name`；`NodeBase` 的类写法端口辅助
  函数移到 `dev_support` 的 `LegacyNodeBase`。
- Map Node 改在 Batch 运行时上执行，作者写法与 Definition 不变；Batch 端口绑定诊断补充期望与实际
  类型。注册表冲突状态在消息无法保存时仍保持失败封闭。

修复建议移出 Core：remediation 的原因、事实、中文摘要与经 Validator 复核的 JSON Patch 修复改由
`alg_pipeline_tool` 生成，不再编入 SDK，也不在 Create 校验失败时执行。`PipelineValidator::Explain`
删除；Validator 只返回中性诊断，未注册类型的相近名称仍在 `suggestions` 中给出。CLI 与 Studio 的
`validate` / `--explain` 继续提供修复建议；流契约诊断与建议的 `facts` 包含有效声明，
数量错误还附上 Validator 已推导的数量形状。

公开错误码：Operator 门面按失败阶段映射返回码，内部的 Pipeline、Node、Model 错误码不再直接返回给宿主。
Create 阶段的配置、部署与模型/Backend 加载失败返回 `-2`（此前部分为 `-3`）；Process 中节点或模型
执行失败返回 `-100`；Control 请求不合法返回 `-2`（此前为 `-1`），节点拒绝或未能应用更新返回 `-100`。
`GetOperatorLastError()` 以 `<阶段> failed with internal code <内部码>: ...` 保留原始码与节点诊断，
见[宿主调用与生命周期](dev_guide/operator_output_allocation.md#宿主调用与生命周期)。

规则匹配结果：`TextRuleMatchNode` 只输出中性结果，`RuleMatchItem` 以 `matches` 列出全部命中、
以 `slots` 保存带 JSON 类型的捕获与常量，删除 `match_result_json`、`details`、`captures` 和
`constants`。关键词响应的 `match_result_json` 与音频响应的 `intent_slot_json` 改由输出转换器序列化，
字段、类型、默认命中与字节内容保持不变。

端口数量关系：输入端口的 cardinality 只描述节点如何消费，接受任意数量的上游数据；
`TextChunkNode` 等拆分输出可以直接接逐项节点（如 `TextRuleMatchNode`）。Validator 改为沿 DAG
推导每个数据名在请求内的条目数，仅在 `1:1` 业务/IO 边界收到多项，或同一节点的逐项输入无法配对时
报告 `PORT_CARDINALITY_MISMATCH`。`TextEmbeddingNode` 的端口声明由 `N:M` 改为实际的 `1:1`。
Pipeline JSON 与 Definition 字段不变，见[数量关系声明](dev_guide/custom_node_concepts.md#数量关系声明与-validator-检查)。

文档链接检查：门禁新增 `DocLinksTest`，检查仓库内 Markdown 的相对链接、HTML `href`/`src`
和标题锚点；没有扫描到文件或跨文件锚点时直接失败，不会空跑通过。

交付脚本要求 PR 分支从最新 `origin/main` 延伸线性提交，拒绝把 main 或其他工作分支合回
PR 分支；正式合并前再次检查 main 是否推进。依赖阶段逐个合并并验证后，再创建下一阶段分支。

Node 作者接口：Batch `Run`、`BuildPrompt`、`FormatAnswer` 签名不符时，编译期直接给出可接受的签名。
两个 LLM 钩子统一返回值规则：接受能隐式转换为 `std::string` 的类型、`std::string_view` 及其 `NodeResult`。
约束收紧：`FormatAnswer` 不再接受 `char`、`int` 等算术类型返回值（此前会被当作单个字符写入结果）。
约束放宽：`BuildPrompt` 可以返回 `std::string_view`；两个钩子的视图先复制再写入，保留来源与内容。

校验诊断：未注册的节点类型、模型类型和 Backend 给出原因与相近的已注册名称；
未注册模型不再连带报告引用错误，业务出口与 IO 边界的同一缺失键只报告一次。
未知节点显式映射的键没有任何已知生产者或 ingress 时，不再连带报告缺少生产者；
已知来源的类型不符、生产者不唯一、ingress 冲突及其他模型的能力不符仍照常报告。
未知配置字段的建议按相似度排序。生产版 `alg_pipeline_tool` 在 stderr 提示检查构建变体
或改用 `alg_pipeline_tool_test`，stdout JSON 保持原有结构。

业务源码自动收录：`src/custom_nodes/`、`src/common_nodes/`、`src/adapter/{input,output,biz}/`
与 `demo/biz/` 下的 `.cpp`（含子目录）在下次构建时自动编入，不再需要修改 CMake。
脚手架删除 `--add-to-cmake`，recipe 不再修改 CMakeLists。

LLM-EdgeFlow 尚未正式发布。当前产品版本标识为 **v11.0.0**；
它描述当前构建与接口基线，不代表已经交付的正式 Release。

Node 的 `OptionalValue` 允许请求缺值，但拒绝已连接输入的运行时类型错误，失败时不调用业务
函数或发布输出。效果验收在独立 Pipeline 副本中继承输出池配置，避免把自身配置合成误报为
执行期间资产变化；验收记录保留实际执行配置，部署变更仍使旧证据失效。

当前基线采用四层架构，通过 C++ Operator SDK 接入宿主；Pipeline JSON 描述明确的数据连接、
模型配置与接入绑定，函数式 Node 使用统一 Spec 声明端口、参数、模型能力和 Control。
现行规则见[架构设计](architecture.md)、[开发者指南](developer_guide.md)和
[配置说明](../configs/README.md)，可用能力以目标构建的 Catalog 为准。

模型效果、目标设备、内部 SDK 接入和开发者体验的验证范围见
[模型、构建与效果验收](VERIFIABLE_SELECTION.md#验收范围与发布准备)。

研发过程与变更历史通过 Git 追溯。正式发布后，本文件按发布版本记录用户可感知的能力、
契约变化及必要的迁移说明。

当前修正包括：线程池部分创建失败时完整回收线程；接入 Binding 批次上限参与运行时
限额；Model 纯配置语义在 Backend 加载前校验；vendor 编译依赖隔离至 Backend。
LLM 节点复用生成参数声明和解析，保留原有字段及默认值。公共 Operator ABI 与
Pipeline 配置格式保持不变。

接入适配层去掉重复声明。删除 `BizExposureDefinition`、`REGISTER_BIZ_EXPOSURE` 及
`IoBindingRegistry` 的 Exposure 接口，注册审计不再检查"曝光业务必须有绑定"。批次上限在绑定上
声明一次：转换器的 `max_batch_size` 默认改为 0（不设限），有效上限取绑定与两个转换器中正值的
最小值，三者显式为 0 时注册审计和部署准备报错。Binding 现默认使用框架标准批次上限 64，
只有实测确需更小值时才覆盖 `max_batch_size`。
`ValidateDecodeRequest`、`DecodeRequestRows` 删除批次上限参数，改读 Operator 填入的
`InputDecodeOptions::max_batch_size`。Catalog 中生产转换器的 `max_batch_size` 由 64 变为 0；各业务的
有效批次上限、Pipeline 配置格式与公共 Operator ABI 不变。

开发 Skills 按业务方案规划、Adapter、Map/LLM/Batch Node、Model 和 Backend 提供独立入口，
由业务需求生成组件增补与 DAG 建议，并复用现有作者 API、Catalog 和验证流程。

构建预设、验证脚本和 Kite CI 共用 `CMakePresets.json` 中的场景参数，脚本需要 CMake 3.19+。
默认门禁沿用 `build/`；新的 sanitizer 和真实模型构建分别使用 `build/sanitizers/` 与
`build/real-models/`，旧目录可按需重建或通过 sanitizer 目录变量复用。测试源码与 runner
归属集中在 `tests/RuntimeTests.cmake`，普通测试按目录自动收集 `test_*.cpp`，自定义 Node
测试无需额外维护源码清单；保留独立测试目标、CTest 分组及必需测试清单校验。

Studio 应用表单时，未修改的数值、布尔、枚举、数组和对象默认值继续保持未配置；已有显式值原样
保留，清空或选回“默认”可移除这些字段的覆盖。字符串清空仍表示显式空字符串。Backend 参数
收进“部署高级设置”，已有显式值时自动展开。

接入层输入长度上限集中在 `biz_input_constraints.h`，接受/拒绝边界、诊断和返回码不变；输出
转换器的容量字段由 ValueType 推导，显式列出时必须与 ValueType 一致。Catalog 中
`audio_result`、`audit_result`、`doc_answer` 输出转换器的容量字段按字典序报告。

`resolve-conf` 增加单次有效批次 `effective_process_batch_limit`、规范化池深
`effective_frame_depth` 和池深硬上限 `max_frame_depth_limit`。Operator 使用解析器给出的同一
批次值；Demo 超限后提示查询命令，原有错误和退出码不变。现有配置中的显式默认值保持原样。

请求编号回传移出业务端口：删除 `kRawRequestIds`，`DecodeRequestRows` / `EncodeResultRows`
去掉请求编号端口参数。`InputDecodeOptions` / `OutputEncodeOptions` 新增 `request_ids`，由
Operator 提供本次调用的编号表；自行组织转换的实现改用 `PublishRequestIds` / `RequestIds`，
直接调用转换器的代码需在选项中提供编号表。解码成功却少记或漏记编号时，在租用输出块前返回
`COMPANY_ALG_ERR_INVALID_INPUT`（`-3`）并报告转换器与数量。Catalog、Binding 端口映射和
Studio 的 `$ingress` 不再列出 `raw_request_ids`；Operator 输出的 `request_id`、公共 ABI
与配置格式不变。

`max_parallel_workers > 1` 时，Validator 将未声明并行安全的节点，以及会共享串行模型的
节点拆到单独的层顺序执行；原本因此在 Create 时返回 `-2` 的配置现在可以运行。
`NODE_NOT_PARALLEL_SAFE`、`SERIALIZED_MODEL_CONCURRENCY` 两个诊断码已删除，
`plan` 的 `layers` 与 `topological_order` 反映拆分后的执行顺序；原始层的写冲突仍会被拒绝。

开发工具目录调整：Node 脚手架与开发 Recipe 移至 `tools/scaffold_custom_node.py`、
`tools/dev_recipe.py`；根目录 `show` 软链接删除，Studio 改由 `./tools/pipeline_studio/server.py`
启动，参数不变。C++ 命令行工具源码由 `src/tools/` 改名为 `src/cli/`，可执行文件仍输出到
`build/`。架构图源文件改名为 `doc/architecture_classes.puml` 与 `doc/architecture_flow.puml`。

业务标识统一：每个业务的配置与 Demo 文件名和 `biz_name`（`<词根>`，不带版本号）使用同一词根，
中文名统一使用 BizDefinition 的显示名。
`biz_name` 变更为 `keyword_match`、`entity_extract`、`translate`、`doc_qa`、`cross_rerank`、
`dialogue_audit`、`ocr_invoice_qa`、`audio_asr_intent`；OCR 与语音方案的配置、Demo、数据集和 Profile 改用 `ocr_invoice_qa`、`audio_asr_intent` 词根。
Demo 结果目录随 `biz_name` 变化。正式上线前不保留旧名别名。
