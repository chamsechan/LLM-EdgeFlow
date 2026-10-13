# 项目精简审查与实施计划

> 状态：第一部分正在本地分支 `refactor/project-simplification` 分阶段实施，验收状态见 1.2。第二、三部分不纳入本轮。
>
> 代码基线：`main@9cfba93b`；审查日期：2026-10-13。
>
> 本文件是跨阶段工作计划。现行规则仍以 [CONTRIBUTING](../CONTRIBUTING.md)、[架构与开发指南](../doc/README.md) 为准。实施中形成的规则随代码写回所属指南；最后一个阶段完成后，按仓库约定删除本计划。

## 阅读与实施方式

本次目标是利用尚未上线的窗口，删除废弃实现和不需要的兼容分支，缩减没有实际需求的泛化，合并真正相同的实现。**不以删除行数为验收指标，也不把当前仍有效的校验、所有权或业务差异当成兼容负担。**

原始审查意见的 A/B 编号用于追溯，P/R/D 编号用于执行和决策：

- **第一部分：29 项优先实施项。** 双方意见重合，或者同事提出且经本助手复核认可的具体范围。按九个阶段实施。
- **第二部分：2 项本助手仍建议实施的补充项。** 不混入双方已形成共同意见的第一批。
- **第三部分：待定方案、待选行为及明确不采纳的删除建议。** 在对应结论改变前，不纳入第一批。

“来源”统一从用户视角标注为“本助手”“你的同事”“双方”。附件中 A 项主要来自本助手、B 项主要来自你的同事；“双方”表示两份意见都支持该问题或方向，**不表示双方已逐字确认本文件的全部实施细节**。最终执行范围以本次复核后的表述为准。

“未上线、不需要兼容”意味着：批准收窄的接口直接修改，并一次更新调用、文档和测试，不添加旧字段别名或迁移适配层，也不为已删除的字段、别名或平台值编写“仍被拒绝/不再出现”的测试。它不意味着所有现行契约都应删除。A1、A11、A12、B9、B11a、P24 等包含接口或接受范围的变化，不能统称“契约不变”。原 D13 已并入 P24，不再单列为待决策项；其余编号保持稳定。

## 第一部分：优先实施项

### 1.1 最终清单

表中风险是该项按限定范围实施的工程风险。“低”不等于可以跳过引用更新或验证。

| 编号 / 原编号 | 优先实施的具体内容 | 证据与范围边界 | 风险 / 阶段 | 来源 |
| --- | --- | --- | --- | --- |
| P01 / B1 | 清除旧 BizDefinition 清单和 shell/Python 守卫残留，恢复当前 Converter 的依赖检查。 | [node_core_contracts.txt](../cmake_ext/node_core_contracts.txt) 仍列已删除头文件。Python 规则迁至 `src/adapter/input/`、`output/`；shell 清理规则 1 的旧头名、规则 3 的退役路径检查、规则 8 的 `biz/business` token，精确范围见 S1。补当前 Converter→Engine/Node 正反例。不是删除所有 `biz` 命名；悬空头视图在 CMake 配置时生成。 | 低—中 / S1 | 你的同事，本助手认可并收窄 |
| P02 / B2 | 删除已弃用的枚举、字段和类型登记。 | 删除 `kAutoClosed`、`RankedCandidate::metadata`、无消费者的 `vector<uint64_t>` 登记、空 `text_rerank` 命名空间、无对应 binding 的 `DECLARE_EXTERNAL_TYPE_TRAITS(int)`。同步测试、脚手架及文档引用。 | 低 / S1 | 你的同事，本助手认可 |
| P03 / A2、B3 部分 | 删除 tokenizer 反向词表与零调用接口，以及 BGE 模型无用访问器。 | [bert_wordpiece_tokenizer.h](../src/engine/models/bge_common/bert_wordpiece_tokenizer.h) 的 `inv_vocab_` 只写不读；清除无调用的访问器、`LoadFromTokens` 及相应实现。保留真实编码流程、正向词表和运行时需要的状态。 | 低 / S1 | 双方；反向词表由本助手发现 |
| P04 / A3 | 删除旧验证包装入口。 | [pipeline_remediation.cpp](../src/cli/pipeline_remediation.cpp) 中两参数 `ExplainPipeline`、其专用 `ValidateWithRemediation`，以及只被这些包装调用的 `PipelineValidator::Validate`。保留 `ValidateAndPlan`、`ValidateParsedAndPlan` 及其现有行为。 | 低 / S1 | 双方；连带范围由你的同事补充 |
| P05 / A4 | 删除冗余 Registry 查询接口。 | [model_registry.h](../include/engine/model_registry.h)、[backend_registry.h](../include/engine/backend_registry.h)、[node_registry.h](../include/core/node_registry.h) 中无生产消费者的 `Has/ListTypes/ListImplNames`。测试和真实模型测试改用 `Find/ListDefinitions`；只删 `registry_support` 中失去消费者的相应函数。 | 低 / S1 | 双方；连带范围由你的同事补充 |
| P06 / A5 | 删除手工端口绑定残留。 | [node_base.h](../include/nodes/node_base.h) 中无调用的 `default_key`、接收 `BlackboardKey` 的构造函数和对应 `Require()`。保留逻辑端口、运行时绑定及有消费者的读取能力。 | 低 / S1 | 双方 |
| P07 / B3 其余 | 删除经全仓引用确认的零调用 API 与只写字段。 | `UnsupportedBiz`、字符串版 `ComputeOutputPoolPayloadBytes`、`TraceableItem::Map`、`PrintBanner/PrintDivider`、`model_root_path`；`GetPipeline` 的测试改用行为观察。`CreateFromIoPlan` 有生产调用，不在删除范围。 | 低 / S1 | 你的同事，本助手认可并修正 |
| P08 / B4、B8、B13 | 修正文档残留，压缩迁移历史，集中维护完整契约表。 | 修正失效需求编号、旧路径和误导注释；把 [CHANGELOG](../doc/CHANGELOG.md) 的 Unreleased 收敛为当前能力与验证边界；合并 `adapter_templates` 跳转页并更新链接。完整字段/default 表集中到所属指南，读者需要的概述、示例仍保留。出现次数不作为删除依据。 | 低 / S9 | 你的同事，本助手认可并收窄 |
| P09 / B6 | 删除没有独立职责的 `run_all_demos.sh`。 | 该脚本仅包装 `alg_demo --suite`；统一使用现有 Demo 入口，检查隐藏目录与说明文件的引用。这里是主动收敛开发入口，不是认定无人会手工使用。 | 低 / S2 | 你的同事，本助手认可 |
| P10 / A6 | 将配置快照接口收敛到当前值输入、值结果的用法。 | [configuration_snapshot.h](../include/nodes/configuration_snapshot.h) 保留现有生产需要的值初始化及 `const Params& → NodeResult<Params>` 更新，删除仅测试支持的指针变体等。保留互斥、原子快照、旧读者生命周期和失败不替换。 | 中 / S4 | 双方；由本助手提出 |
| P11 / A7 部分 | 共享输出 JSON 容量参数声明。 | `src/adapter/output/` 的八个 Converter 重复声明 `result_json_max_bytes` 及默认 `8191`。提取一份普通参数类型和声明；Catalog、默认值、容量含义与失败行为保持一致。多端口行拼装另见 D02。 | 低 / S3 | 双方 |
| P12 / B9 | 合并 Converter 公共 Options/Definition 字段，删除与 `type` 重复的 `slot.type_suffix`。 | [io_converter.h](../include/adapter/io_converter.h) 的公共字段可复用非虚基类，方向回调仍分别声明。现有审计要求两份类型名相等；删除重复字段后，不一致的状态不再存在。Catalog 会输出、Studio 会展示该字段，需统一改用 `type`，同步作者接口、测试与指南。保留 binding 的 `canonical_suffix` 及其存在性、方向、值类型校验。 | 中 / S3 | 你的同事，本助手认可并补充影响面 |
| P13 / B10 | 抽取输入 Converter 与 binding 中的相同操作。 | `audit_input.cpp` / `doc_query_input.cpp` 共享两路文本展开、发布步骤；`operator_builtin_value_types.cpp` 共享 keyword/entity 的载体构造步骤。保留不同宿主类型、业务映射、逻辑端口和诊断位置，不引入万能 DTO。 | 低—中 / S3 | 你的同事，本助手认可 |
| P14 / A8 部分 | 抽取 Demo entity/keyword 文本读取与载体构造辅助函数。 | `demo/input/` 的重复步骤可以共享；字符串与 vector 的存储仍由覆盖 SDK 调用的对象持有。回退开关和样例不随此项删除，见 D03。 | 低 / S2 | 双方 |
| P15 / A9、B25 部分 | 精简 NodeFailure 无消费者字段，并集中格式化来源信息。 | [node_result.h](../include/nodes/node_result.h) 删除无运行时消费者的 `kind/stage/model_slot/source_location` 等作者层负担；`req_id/sub_id` 等来源保存在 `BatchFailureDetail`，最终格式化一次，删除文本扫描去重。**业务、模型和异常的原始 message 保留**。 | 中 / S4 | 双方；A9 由本助手提出，B25 由你的同事提出 |
| P16 / A11 | 删除没有执行效果的 mock `prompt_id`。 | `ControlSwitchPromptParam::prompt_id` 只验证长度，未转发给节点。同步 [operator_control_registry.cpp](../src/adapter/operator/mock/operator_control_registry.cpp)、mock 公共头、测试和指南；模板切换仍工作。不据此推断真实内网 SDK 结构。 | 中 / S3 | 双方；由本助手提出 |
| P17 / A12 | 删除不可用的 `buffer/any` 输入 binding 注册及专用死代码。 | [operator_builtin_value_types.cpp](../src/adapter/operator/mock/operator_builtin_value_types.cpp) 的相应 binding 没有读取回调，不能通过 Converter 审计。保留 frame metadata 使用的 Any 校验；输出 allocator/metadata 另见 D18。 | 中 / S3 | 双方 |
| P18 / B11a | 删除 `CPU_GENERIC` 平台别名。 | 四个 Backend 接受它，但当前生产路径不产生它。[test_kite_backend.cpp](../tests/unit/engine/test_kite_backend.cpp) 的 `cpu_generic` 用例位于 `HAVE_KITELLM` 条件编译之外，默认门禁会执行。删除接受分支，默认测试改用规范拼写；保留现有 `cpu` 归一及设备编号检查。大小写契约另见 D06。 | 低 / S1 | 你的同事，本助手认可并确认默认覆盖 |
| P19 / B12 部分 | 将 `IsNodeResult` / `IsNodeResultType` 收敛为一份 trait。 | 两者表达相同类型判定，更新使用方，删除另一份定义。不同输入抽象的对齐校验另见 D11，不在该机械修改中强行合并。 | 低 / S4 | 你的同事，本助手认可并拆分 |
| P20 / B15a | 共享字段校验错误到 Core 诊断码的映射。 | [deployment_preparation.cpp](../src/adapter/deployment_preparation.cpp) 与 [pipeline_validator.cpp](../src/core/pipeline_validator.cpp) 重复映射 `ConfigFieldErrorKind`。共享返回现有 Core 诊断枚举的函数；各层继续负责自己的路径、建议和文本。Integration 专属码不搬进 Core。 | 低 / S7 | 你的同事，本助手认可 |
| P21 / B17 | 去掉输出池内部已完成校验后的重复校验。 | [operator_output_pool.cpp](../src/adapter/operator/operator_output_pool.cpp) 已得到有效 spec 后，使用私有计算函数，避免公共预算函数再校验一次。`ResolveOutputPoolSpec` 主要是校验和复制，并非四次 JSON 解析。所有独立 API 的入参校验、实际深度一致性及两级预算仍保留。 | 低—中 / S7 | 你的同事，本助手认可并收窄 |
| P22 / B20 | 收敛注册路径的重复异常处理。 | [model_registry.cpp](../src/engine/runtime/model_registry.cpp)、[backend_registry.cpp](../src/engine/runtime/backend_registry.cpp) 和共享注册辅助逻辑中，合并可由同一层处理的回调复制、创建异常。保留失败后拒绝使用无效注册的状态、分配失败保护和锁外调用 creator；不删除不同 API 边界的异常屏障。 | 中 / S5 | 你的同事，本助手认可并收窄 |
| P23 / B23 | 删除无更新需求的模型 revision 链路。 | [session_context.h](../include/core/session_context.h)、[session_resources.h](../include/nodes/session_resources.h) 及 Embedding 缓存：删除 revision 字段、生成拼接、读写/转发 API 和缓存键中的 revision。**ModelRegistration 仍保留期望 impl_name/model_type 与实例，不能只剩 name/model**；保留身份验证和批量原子安装。 | 中 / S6 | 你的同事，本助手认可并修正 |
| P24 / B25、原 D13 | 直接删除无语义消费者的 `severity` 诊断字段和 `supports_hot_swap` Catalog 字段。 | 全仓未发现工具或运行时据此作判断；`supports_hot_swap` 仅有格式断言，且底层默认 false、AuthorNode 才填 true，并非全局常量。成员、构造参数/实参、赋值、五处 JSON 输出和相应格式预期同改；保留 Control 能力和诊断 code/path/message/facts。明确收窄工具 JSON 契约，不做“删存储但保留输出”的中间重构。 | 低—中 / S4 | 你的同事提出，本助手复核认可直接删除 |
| P25 / B29a 部分 | 仅在错误语义和数值均一致的分支，用既有具名常量替换裸码。 | 逐分支核对 [error_codes.h](../include/platform_mock/error_codes.h) 的含义，禁止按数字批量替换。pool 缺池 `-5`、resolver 阶段分码 `-3/-2`、facade 空输出指针 `-1` 均不能套用同值异义的 mock 常量。facade 已通过 Adapter 头使用这些常量，不排除在范围外；不新增 mock 依赖，也不在本项清理已有传递依赖。PR 列出逐项映射；不能证明同义的裸码保留，错误码中立化另议。 | 低 / S7 | 你的同事，本助手认可并修正替换条件 |
| P26 / B31 部分 | 将单条 registry 注册改为直接插入，去掉整表复制。 | [operator_value_type_registry.cpp](../src/adapter/operator/operator_value_type_registry.cpp) 的单条 binding/allocator 注册在锁内先检查冻结和重复冲突，再构造候选、单次插入。现有故障测试依赖复制旧表元素，必须改为对新候选构造或插入注入失败；继续验证表不变、不误记冲突、允许重试。Copy 查询的独立所有权和 ModelManager 批量事务保留。 | 中 / S5 | 你的同事，本助手认可并收窄 |
| P27 / A1 | 删除 Studio HTTP 请求字段别名及冲突处理分支。 | [server.py](../tools/pipeline_studio/server.py) 统一使用页面现有的 `revision`、`conf_name`、`filename`；删除 `expected_revision/conf_path/pipeline_name` 别名。只调整对应 HTTP 请求协议，不机械重命名同名内部变量。 | 低 / S2 | 双方；由本助手提出 |
| P28 / B32 | 合并重叠的分层守卫和纯文字元检查。 | 把相同依赖规则集中维护；删除旧实现前，用允许/禁止的真实代码样例证明新守卫覆盖等价。保留解析后路径检查、include 视图、编译正反例、C11 和 llama 语义检查。不得只为删测试而降低边界保障。 | 中 / S9 | 你的同事，本助手认可限定范围 |
| P29 / B14 | 复用路径处理原语，保留三类独立策略。 | 优先复用 [path_utils.h](../include/contracts/path_utils.h) 中已有能力，再提取确实相同的规范化、目录包含、符号链接处理。`cfg_file_name`、`pipe_path`、模型路径分别保留对绝对路径、`..`、存在性的当前规定。不引入通用“沙箱策略引擎”。 | 中 / S8 | 你的同事，本助手认可修订方案 |

### 1.2 实施总顺序

推荐顺序：**S1 → S2 → S3 → S4 → S5 → S6 → S7 → S8 → S9**。每个阶段都应形成可独立验收的修改；代码与对应契约测试、现行文档同阶段完成。

阶段按验收单元划分，不要求机械地凑成一个巨大 PR。若一个阶段拆成多个 PR，各 PR 均需可单独构建和验证；不要把头文件删除、消费者迁移、测试修复拆成无法独立工作的提交链。后续授权远程交付时，按 CONTRIBUTING 逐阶段合入并确认对应 main CI，再从最新主线开启下一阶段。本计划本身不执行远程交付。

| 阶段 | 覆盖项 | 主要结果 | 开始条件 | 状态 |
| --- | --- | --- | --- | --- |
| S1 清理确定废弃的声明与入口 | P01—P07、P18 | 旧概念不再进入构建，死 API 删除，现有消费者使用保留接口 | 记录实施基线，确认无新的生产消费者 | 已验收 |
| S2 精简 Studio / Demo 开发入口 | P09、P14、P27 | 单一请求字段和 Demo 入口，文本输入步骤复用 | S1 验收通过 | 已验收 |
| S3 精简 Adapter 声明与载体辅助逻辑 | P11—P13、P16—P17 | 公共参数/声明复用，mock 与 Catalog 移除选定冗余 | S2 验收通过；明确表内协议修改面 | 未开始 |
| S4 收敛 Node 作者层和诊断 | P10、P15、P19、P24 | 减少模板分支和无效字段，删除选定工具 JSON 字段 | S3 验收通过；先明确 D09 在本轮保留还是删除 | 未开始 |
| S5 简化单条注册与异常路径 | P22、P26 | 不复制整个 registry，重复 catch 收敛 | S4 验收通过；具备故障注入覆盖 | 未开始 |
| S6 删除模型 revision | P23 | 会话内不可变模型不再维护无用版本链路 | S5 验收通过；身份和缓存隔离测试保留 | 未开始 |
| S7 收敛部署诊断和输出池内部计算 | P20—P21、P25 | 同义映射集中，消除一个内部重复校验，错误码可读 | S6 验收通过 | 未开始 |
| S8 复用路径处理原语 | P29 | 共用机制，保留三类策略差异 | S7 验收通过；已有路径行为矩阵 | 未开始 |
| S9 收敛守卫并完成文档整理 | P08、P28 | 一处维护相同规则，文档描述当前能力 | S8 验收通过；先证明新守卫覆盖 | 未开始 |

### 1.3 各阶段实施步骤与验收

#### S1：清理确定废弃的声明与入口

已完成本地验收：P01—P07、P18 全部落地；守卫自测 7 项通过；G8 在启用 Whisper 的独立构建中成功链接 `test_real_models_e2e`（未运行真实模型）。统一门禁 395 项，394 通过、1 项因未提供 GGUF 跳过；两轮独立只读复核无阻塞项。

**修改范围：** `cmake_ext/node_core_contracts.txt`、`scripts/check_layer_isolation.sh`、`scripts/check_layer_dependencies.py` 和守卫自测，相关 `include/` 头、`src/engine/models/`、`src/engine/backends/`、`src/cli/pipeline_remediation.*`，以及直接消费者和测试。

1. 对 P02—P07 的每个符号检查源码、测试、示例、脚手架、隐藏配置和真实模型测试。没有生产调用不等于没有需要同步修改的作者示例。
2. 修正头视图清单；Python Converter 规则从退役的 `src/adapter/biz/` 迁至 `src/adapter/input/`、`output/`。自测补齐 input/output 分别依赖 Engine/Node 的四个禁止样例及允许依赖样例；现有 platform_mock 反例仍保留。
3. shell 规则 1 仅移除 `company_alg_interface.h` 和 `edgeflow/c_api.h/.hpp` 的旧匹配，保留当前 Integration 路径；删除规则 3 对退役 `biz/business` 路径的检查，保留负责现行 `custom_nodes` 边界的规则 3b。两者不是正则等价覆盖。规则 8 仅删 `biz/`、`business/`，保留 `adapter/core/engine/src`；规则 2 的 Converter 依赖检查仍保留。然后重新运行 CMake 配置，确认旧头文件不会进入新生成的视图。
4. 删除已确认的死声明、实现及其专用测试；仍有价值的测试改用保留接口证明相同能力。删除 `CPU_GENERIC` 的接受分支；将 Kite 默认用例中的 `{1, "cpu_generic"}` 改用 `{1, "cpu"}`，继续验证 CPU 设备编号约束。此项不依赖启用 Kite SDK，也不改变 D06 尚未决定的大小写归一契约。
5. 更新受影响的源码作者示例，不保留旧类型别名或转发包装；按 G8 在独立目录启用真实模型测试目标并完成只编译验证。

**验收条件：**

- 当前 Converter 中故意引入 Engine/Node 依赖时，守卫能拒绝；允许依赖不被误判。
- BGE 编码结果与模型语义测试通过；反向词表不再存储，不用估算内存数字代替证据。
- 默认 G1 中的 `KiteBackendTest.UnsupportedProtocolAndNonCpuTargetFailBeforeResources` 继续执行；合法拼写下的设备编号校验保留。P18 的默认测试覆盖不能替代 P05 所需的 G8 非默认消费者编译。
- Registry 的排序、冲突、重入及注册失败测试仍有对应覆盖；G8 成功编译 `test_real_models_e2e`，并覆盖开启 Whisper 的分支。默认门禁不开此目标，不能代替这项证据；只编译不要求模型权重，也不等于真实模型运行通过。
- `ValidateAndPlan` / `ValidateParsedAndPlan` 和 CLI 验证仍工作；`CreateFromIoPlan` 保留。
- 对已删除符号全仓搜索无活动引用；计划中的文字记录不算残留实现。

**聚焦验证：** G1、G4、G6、G7 中受影响的用例，以及 G8 的非默认只编译检查；完成后运行统一交付门禁。

#### S2：精简 Studio / Demo 开发入口

已完成本地验收：P09、P14、P27 全部落地；真实 HTTP 聚焦测试 14 通过、1 可选浏览器跳过；统一门禁 395 项，394 通过、1 项因未提供 GGUF 跳过，包含 SharedDemoConsumer 与 Demo smoke。

**修改范围：** `tools/pipeline_studio/server.py`、相关 HTTP 测试与工具说明，`demo/input/`、Demo 测试，`scripts/run_all_demos.sh`。

1. HTTP 处理只从规范字段取值，删除别名与别名冲突逻辑。旧字段不再提供别名功能；沿用各端点现有未知字段处理方式，不另建兼容解析器。
2. entity/keyword 提取少量共享读取和构造辅助函数；调用方继续决定具体载体类型和业务映射。
3. 删除包装脚本，把发现的使用说明指向 `alg_demo --suite`。

**验收条件：**

- 规范请求的 preview/apply/save/关联操作不受影响；测试只覆盖规范字段。
- revision 冲突和工具 fingerprint 校验仍生效。
- Demo 缓冲区在 SDK 调用期间有效；entity、keyword 和 audio smoke 均能运行。
- **不删除** `--allow-fallback-sample`、音频 PCM 构造或任何样例；这些属于 D03。

**聚焦验证：** G4、G5；完成后运行统一交付门禁。

#### S3：精简 Adapter 声明与载体辅助逻辑

**修改范围：** `include/adapter/io_converter.h`、`converter_authoring.h`，`src/adapter/input/`、`output/`、注册/Catalog 实现、mock binding/control，`include/platform_mock/`，Studio 的 Catalog 消费代码。

1. 先提取共享输出容量参数、Options 和 Definition 公共字段，保持输入/输出方向的回调类型明确；不引入虚继承或新的注册框架。
2. 删除 `slot.type_suffix` 和只用于填充它的 slot 构造参数，以 Converter 的 `type` 作为唯一类型名；同时删除两份类型名必须相等的冗余检查。一次更新 Authoring、Catalog、Studio 展示、指南、相关 skill 引用及专门制造两字段不一致的测试。注册审计中的 binding 存在性、方向、值类型和服务校验仍保留；执行前在修改说明记录作者接口与 Catalog 字段的变化。
3. 提取 audit/doc_query 的文本展开步骤、keyword/entity 的载体构造步骤；维持现有所有权、端口映射和诊断位置。
4. 删除 mock `prompt_id` 及不可用的 buffer/any 输入注册，更新相关字段、声明、注册和测试消费者；保留现有 Control 命令和 frame 使用的 Any 类型信息。

**验收条件：**

- 八个输出 Converter 的默认值、容量边界、序列化结果和超限错误保持一致。
- Catalog 仅发生本阶段明确列出的字段/注册删除；`type` 能支撑 Studio 展示与选择。
- binding 的 `canonical_suffix`、frame 的 Any 校验以及输出 metadata 机制仍存在。binding 缺失、方向不符、宿主值类型不符仍被拒绝；不能因为删除 `type_suffix` 相等性检查而删除其他类型约束。
- 切换 prompt 模板的有效控制路径仍工作；业务完整请求/响应保持由 Adapter 处理。
- 公共 mock 头和 SDK 消费者可编译；不借本次改动推断或接入内部真实 SDK。

**聚焦验证：** G2、G4；完成后运行统一交付门禁。

#### S4：收敛 Node 作者层和诊断

本轮决定：D09 的 Join/Group/Select/Scatter 保留；P15 同步迁移其 Failure 签名与来源格式化，不执行第三部分的 API 删除。

**修改范围：** `include/nodes/configuration_snapshot.h`、`node_result.h`、相关 trait 定义、Node 调用方、`tools/scaffold_custom_node.py` 及作者示例；另含 `include/core/node_definition.h`、`pipeline_validator.h`、Control 作者声明、诊断/Catalog 的五处 JSON 输出和相应测试。

1. 先记录 D09 的本轮取舍：若删除 Join/Group/Select/Scatter，先按确定范围删除其实现和专属消费者，再执行 P15；若本轮保留，所有保留函数都要同步迁移共享 Failure 签名和格式化行为。不能只改 Split/Map 而让其他函数继续调用已删除的公共签名，也不为已决定删除的函数重写诊断。
2. 配置快照仅保留当前生产使用的初始化和更新签名，删除只为泛化而存在的模板分支。
3. 合并同义 NodeResult trait，删除 NodeFailure 无消费者的元数据字段及传递样板；同步脚手架中 `NodeErrorKind::kBusinessError` 等生成模板、作者示例和签名诊断用例。
4. 来源信息在 `BatchFailureDetail` 中维护，统一在输出诊断时格式化；业务 message 保留，不再扫描 message 推断是否已经含有来源。
5. 删除 `severity` 成员/赋值及 Validator、CLI、authoring、document validation 的四处诊断 JSON 字段；删除 `supports_hot_swap` 成员、构造参数/AuthorNode 实参、Catalog 输出字段及相应格式断言。不保留常量输出或兼容别名。

**验收条件：**

- 并发读取能持续持有旧快照；失败更新不发布新值；成功更新整体可见。
- 分片、模型失败、异常失败的诊断同时包含业务原因与准确来源，来源只格式化一次。
- 多输出节点仍在校验全部结果后发布，失败不留下部分输出。
- 诊断 JSON 不再输出 `severity`，Catalog 不再输出 `supports_hot_swap`；Control 执行、诊断 code/path/message/facts、错误传播和失败分类不变。仍保留 Control 信封及 PortGroupConstraint。
- G7 的脚手架生成代码可编译；合法 Run 签名通过，错误签名仍给出有用的编译期提示。

**聚焦验证：** G3、G7，并运行 G4 的 Catalog/CLI 用例；完成后运行统一交付门禁。

#### S5：简化单条注册与异常路径

**修改范围：** `src/engine/runtime/` 中的 Registry 创建/注册逻辑，`src/adapter/operator/operator_value_type_registry.cpp`，相关注册测试。

1. 对单条 binding/allocator 注册，在锁内先检查冻结和重复冲突，再构造候选、单次插入，删除整表 copy-and-swap。候选构造必须在修改容器前完成，也不能抢在冲突检查之前抛异常而漏记冲突。
2. 合并 ModelRegistry 创建路径中可统一处理的异常分支，异常处理仍覆盖回调复制和 creator 调用；creator 继续在锁外执行。
3. [现有 registry 故障测试](../tests/unit/operator/test_operator_value_registry.cpp) 的 `ThrowingCopiesLeaveBindingAndAllocatorMapsUnchangedAndRetryable` 把抛异常回调装在已经注册的元素上，确实依赖复制旧表。单条注册改造后，分别对新 binding、新 allocator 的候选构造或插入注入失败，继续断言旧记录不变、新记录未出现、没有误记冲突、撤销故障后可重试。不要保留已经失效的旧注入点，也不要删除这些行为断言；P22 的 creator 抛异常覆盖继续保留。

**验收条件：**

- binding 与 allocator 两条注册路径都证明：候选构造或插入失败时不留下半条记录，旧记录保持有效，不把资源故障记成契约冲突，撤销故障后可重试；冻结后注册被拒绝，真实重复冲突仍被记录。
- 诊断字符串构造失败时，也不能让无效注册进入可用状态。
- creator 可重入查询而不死锁；标准异常和未知异常仍被当前 API 边界收住。
- Copy 查询仍提供独立所有权，Prepare 不借用冻结前可能变化的注册内容。
- `ModelManager::RegisterBatch` 的批量事务保持原实现语义，不套用单条插入优化。

**聚焦验证：** G1、G2 的注册与故障注入用例；完成后运行统一交付门禁。

#### S6：删除模型 revision

**修改范围：** `include/core/session_context.h`、`include/nodes/session_resources.h`、模型安装调用方、Embedding 会话缓存及测试。

1. 删除 revision 存储、JSON 拼接、读取/更新 API 和 Node facade 转发。
2. 缓存键去掉 revision 部分，保留模型名、缓存输入参数及既有会话隔离。
3. `ModelRegistration` 至少保留 `model_name/impl_name/model_type/model`。其他描述字段仅在确认它们专用于 revision 生成后删除。
4. 把 revision 更新测试改为当前不可变注册、缓存隔离和失败重试的行为测试。

**验收条件：**

- Factory 和 ModelManager 的实例身份检查均保留；错误实现/类型/并发声明继续被拒绝。
- 重复模型名被拒绝；批次中一个模型失败时，整个批次不安装。
- 不同模型名、不同 Session 不共享错误的缓存结果；相同模型和有效输入仍复用缓存。
- 失败的缓存创建可以重试；缓存值不携带前一次请求的错误 provenance。

**聚焦验证：** G1 的 Factory/ModelManager/Session 用例与 G3 的 Embedding 用例；完成后运行统一交付门禁。

#### S7：收敛部署诊断和输出池内部计算

**修改范围：** `deployment_preparation.cpp`、`pipeline_validator.cpp`、诊断声明，`operator_output_pool.cpp`、`operator_value_type_registry.cpp`，Adapter 错误码使用处。

1. 将两个同义字段错误映射集中到一个 Core 可拥有、Integration 可向下依赖的函数，保留路径拼接、未知字段建议和原有错误语义。
2. 分离输出池“校验”和“已校验 spec 的纯预算计算”。纯计算函数仅供内部已验证路径使用；公共入口仍自行验证。
3. 对每个候选替换分支确认错误语义和数值同时匹配，并在 PR/修改说明列出“位置与条件、当前含义、旧数值、所用常量、对外映射”。facade 已通过 Adapter 头使用 mock 常量，其同义分支可纳入；不新增 mock include 或扩大语义依赖，已有传递依赖清理另议。不能确认同义的裸码保持原样，不以禁止新增 include 代替语义审查。

**验收条件：**

- 同一字段错误映射一致，路径与建议仍有上下文。
- 独立调用公共预算/创建 API 时，无效 spec、溢出、容量和深度问题仍被拒绝。
- 单池预算、全句柄总预算和实际分配深度匹配仍被检查。
- 错误返回值、部署错误与 Pipeline 错误分类不变。输出池缺池的 `-5` 不命名为 `UNSUPPORTED_BIZ`，resolver 的 `-3/-2` 不改为其他层错误语义，facade 空输出指针的 `-1` 不命名为 `INVALID_HANDLE`；现有协议壳映射仍被测试覆盖。
- 不缓存可变计划的“已验证”标记，不删除诊断双通道；这两类更大改动另见 D16、D17。

**聚焦验证：** G1、G2、G4 的 Validator/部署/输出池用例；完成后运行统一交付门禁。

#### S8：复用路径处理原语

**修改范围：** `include/contracts/path_utils.h` 及当前配置入口、Pipeline 引用、模型文件解析的直接调用方。

1. 先将三类现有规则整理成行为测试矩阵：相对/绝对路径、`..`、不存在的文件、普通文件/目录、符号链接及根目录逃逸。
2. 保留每类路径的策略分支，只提取重复的路径机制；优先调用已有工具函数，避免新增一套泛化策略配置。
3. 保持错误位置和处理阶段，使配置错误仍能指回原字段。

**验收条件：**

- `cfg_file_name` 仍要求受约束的相对现有文件。
- `pipe_path` 仍按当前根目录包含规则处理允许的绝对路径。
- 模型文件仍保持对 `..` 和文件存在性的独立规则。
- 符号链接逃逸、目录前缀误判、盘符/UNC 等输入校验没有因为宿主平台选择而丢失。
- 新旧实现在上述矩阵上的接受/拒绝结果一致。

**聚焦验证：** G2 的文件参数/配置安全用例及 G4 的部署 CLI 用例；完成后运行统一交付门禁。

#### S9：收敛守卫并完成文档整理

**修改范围：** `scripts/check_*`、相关架构测试、`doc/` 中涉及的现行指南和索引。未完成的旧 plans 不在自动删除范围。

1. 对重叠守卫逐条列出“原规则 → 唯一维护位置 → 正反例测试”；先确认覆盖，再删重复实现。
2. 删除仅锁定某一段文字/命令拼写、而没有独立契约价值的元检查，保留行为与语义测试。
3. 更新失效编号、路径和注释；合并跳转页；让完整契约表只在所属指南维护，其他文档保留必要概述和准确链接。
4. 将 Unreleased 改写为当前能力基线与验证限制，删除重复迁移叙述；不把未验证硬件写成已支持。

**验收条件：**

- Engine/vendor、Node→Integration、Demo、custom_nodes 等禁止依赖均有有效反例覆盖。
- include 视图隔离、编译正反例、C11、llama 语义及文档链接检查继续有效。
- 文档与 S1—S8 的实际接口一致；没有恢复已删旧名的兼容说明。
- 不依据“未被 README 索引”认定旧计划已完成；D04 未决定前不删除相应计划。
- 对原 A/B 意见逐项核对去向，每项均映射到 P/R/D 或明确保留；包括 D25 对 B30 的装配与 CLI 入口讨论，不能只核对已经列出的 P 项。

**聚焦验证：** G6、G7 及修改过的检查器自测；完成后运行统一交付门禁。

### 1.4 验证命令

以下名称已按本次基线的 [RuntimeTests.cmake](../tests/RuntimeTests.cmake) 和现有测试核对。它们用于开发反馈，不取代统一门禁；实施时若测试名已改变，先核实新名称，不接受“零测试通过”。

首次或 CMake/头视图改变后，从仓库根目录配置：

```bash
./scripts/configure_build.sh "$PWD" "$PWD/build" dev-gate
```

以下命令均从仓库根目录执行，可直接从 Markdown 源文件或渲染页复制。`-j4` 可按本机资源调整。G1—G7 使用默认 `build`，G8 使用独立目录，避免改变默认门禁配置。

**G1：Engine / Core。** 冲突测试由 CTest 为每个用例启动独立进程，避免污染注册单例。

```bash
cmake --build build --target edgeflow_test_core_runner test_model_backend_registry_conflict test_registry_conflict -j4
./build/edgeflow_test_core_runner
ctest --test-dir build -R '^(ModelExecutionRegistryTest|RegistrationStartupTest)[.]' --output-on-failure --no-tests=error
```

**G2：Adapter / Operator。** 涉及公共边界时，执行最后一条 SDK 检查。

```bash
cmake --build build --target edgeflow_test_adapter_runner test_cpp_operator_sdk alg_sdk -j4
./build/edgeflow_test_adapter_runner
ctest --test-dir build -R '^(CppOperatorSdkTest|SdkExportSurfaceTest)$' --output-on-failure --no-tests=error
```

**G3：Node / 参数 / Control。** 覆盖 AuthorNode、ConfigurationSnapshot、模型调用、参数、批处理来源与受影响实际 Node。

```bash
cmake --build build --target edgeflow_test_nodes_runner -j4
./build/edgeflow_test_nodes_runner
```

**G4：Studio / CLI / Catalog。**

```bash
cmake --build build --target alg_pipeline_tool alg_pipeline_tool_test alg_show alg_demo edgeflow_test_tooling_runner -j4
ctest --test-dir build -R '^(PipelineStudioServerTest|PipelineToolValidateTest|PipelineToolCatalogTest)$' --output-on-failure --no-tests=error
./build/edgeflow_test_tooling_runner
```

**G5：Demo。**

```bash
cmake --build build --target alg_demo edgeflow_test_tooling_runner -j4
./build/edgeflow_test_tooling_runner --gtest_filter='SharedDemoConsumerTest.*'
ctest --test-dir build -R '^(DemoSmokeTest|JsonPromptDemoTest)$' --output-on-failure --no-tests=error
```

**G6：守卫 / 文档。** 先完成 CMake 配置；修改检查器时，补跑其对应自测。

```bash
ctest --test-dir build -R '^(CMakeStructureTest|LayerGuardTest|LayerGuardSelfTest|GovernanceConsistencyTest|ArchitectureDocsDriftTest|DocLinksTest)$' --output-on-failure --no-tests=error
```

**G7：作者接口 / 脚手架。** 覆盖签名提示、生成模板及开发步骤中的消费者；P15/P19 修改后必须执行。

```bash
cmake --build build --target alg_pipeline_tool alg_pipeline_tool_test alg_demo -j4
ctest --test-dir build -R '^(SpecSignatureDiagnosticsTest|CustomNodeScaffoldTest|DevRecipeTest)$' --output-on-failure --no-tests=error
```

**G8：真实模型测试源码的非默认只编译检查。** S1 必须补此项。`default-cpu` 启用 ONNX Runtime、llama.cpp 和 Whisper，`ENABLE_REAL_MODEL_TESTS=ON` 生成默认门禁不构建的 `test_real_models_e2e`。只编译目标及依赖，不运行 E2E；无需模型权重，仍需相应第三方依赖可获取、可编译。

```bash
./scripts/configure_build.sh "$PWD" "$PWD/build/variants/simplification-e2e-compile" default-cpu -DENABLE_REAL_MODEL_TESTS=ON
cmake --build build/variants/simplification-e2e-compile --target test_real_models_e2e -j4
```

如果 G8 因工具链或依赖不可用而无法完成，应明确记录未验证，并由启用相同能力的 CI 对同一提交构建成功后才接受 S1；不能用默认门禁通过或真实模型测试因缺少权重跳过替代编译成功。

每个阶段的代码、测试和文档都完成后，本地交付统一执行：

```bash
./scripts/run_all_tests.sh
```

授权远程交付时改用 CONTRIBUTING 指定的交付入口，其内部执行同一门禁，不再重复跑两遍完整门禁。记录实际命令、通过/失败、skip 及所缺资产。默认门禁不证明真实模型、真实浏览器、公司内部 SDK 或目标硬件已经验收；涉及这些路径时同步消费者，并如实记录非默认验证结果。

### 1.5 阶段完成与回退

- 每阶段验收针对行为和约束，不只是“编译通过”或删了多少行。纯机械删除若已有测试充分覆盖，不额外制造只验证实现细节的测试。
- 执行前复核本阶段符号是否新增消费者。若发现新的使用场景，暂停对应删除并更新本计划的范围，不靠兼容包装掩盖冲突。
- 阶段失败只修复本阶段引入的问题；无关基线失败单独记录。未通过不能标为完成，也不进入依赖它的下一阶段。
- 回退以该阶段完整变更为单位，同时恢复接口、调用方、测试和文档；不在业务代码中增加“旧/新双模式”开关作为回退机制。
- S9 完成前核对本清单所有 P 项均有结果，第二、三部分分别实施、明确保留或从本轮范围中移出。最终规则留在现行指南，过程留在 Git/PR，工作计划不长期充当另一份架构规范。

## 第二部分：本助手仍建议实施的补充项

这些项有具体收益，建议在第一部分完成后单独实施。它们不是对第一批的隐含追加，也不要求为等待它们而延后第一批。

| 编号 / 既往编号 | 建议与依据 | 实施边界及验收 | 风险 | 来源 |
| --- | --- | --- | --- | --- |
| R01 / C1 | 删除 `FixedBatchExecutor::ExecuteItems` 的旧异常码兼容参数和内层重复 catch。[fixed_batch_executor.h](../include/engine/fixed_batch_executor.h) 明确写着保留模型既有异常映射；外层 `Execute` 已统一捕获回调异常。 | 收敛异常返回约定，更新 Qwen 等调用与测试。保留主动业务失败码透传、padding 移除、`req_id/sub_id` 来源、任意分片失败清空全部输出。明确这会改变 Qwen 的异常错误码，不宣称完全无行为变化。用批处理和各模型测试验收。 | 中 | 本助手补充 |
| R02 / C4 | Studio 同名保存已关联方案时，只替换 Pipeline JSON，保留指向不变的正式 `.conf` 原字节。[server.py](../tools/pipeline_studio/server.py) 的 `update_solution` 当前重写两份文件，并为第二次替换失败回滚第一份。 | 保留 `solution_lock`、保存前/提交前的双 revision 与路径检查、原生部署预检和 JSON 原子替换；预检使用的临时 `.conf` 仍需要。两个 revision 中只更新 Pipeline revision，conf revision 仍对应原字节；成功后继续更新 profile，返回 conf 信息与实际保留文件一致。新建/另存仍创建配套文件。测试验证 conf 原字节不变、外部冲突、JSON 替换失败及新建失败清理。 | 中 | 本助手补充 |

## 第三部分：争议、待决策与不采纳项

### 3.1 先明确方案或行为，再进入实施

这里区分“工程方案待明确”和“产品/契约选择待明确”。前者可以由实施者根据现有约束完成设计；后者需要在变更说明中明确选择的行为，不能由“精简”自动推出删哪一种能力。

| 编号 / 原编号 | 复核结论与待明确事项 | 当前处理建议 | 来源 |
| --- | --- | --- | --- |
| D01 / A10 | Studio Python JSON Patch 与 C++ patch 重复，且对操作和数组下标的接受范围存在偏差。改由原生工具执行值得做，但需定义输入/输出协议、失败回传和 preview/apply 的唯一补丁应用位置。 | 原生工具统一语义是推荐方向；先定协议和错误测试，保留 revision、fingerprint 与预览一致性，再删除 Python 实现。 | 本助手提出，双方认可问题 |
| D02 / A7 其余 | 多端口输出确有重复，但 audit 的 ranked 索引先按 req_id 选择 `sub_id==0` 的项，再检查其 `rank==1`，不能改为搜索任意 rank=1 的项；其他端口的数量和索引规则也需分别保留。直接 zip 或统一索引会改结果。 | 先明确每个端口的键、数量与选择策略，再设计小型行遍历 helper；保留各业务组装函数。不能按“低风险 40 行复制”直接替换。 | 双方；本助手补充边界 |
| D03 / A8 其余 | “回退从不触发，所以可删开关和七处样例”不成立。`audio_input.cpp` 仍会为 audio smoke 生成 PCM，即使文本回退未启用也不能据此删除该路径。 | 若决定不提供样例回退，先补独立音频 fixture、迁移相关 profile/测试，再删开关与对应样例。当前只执行 P14。 | 你的同事提出删除，本助手不认可直接删除 |
| D04 / B5 | 三份旧计划并非无引用、全部完成：存在互相引用，以及 CODEOWNERS、O-2、真实开发者试用、延期跨请求状态等未结项。 | 先逐项决定完成、放弃或继续；仅保留仍有效的最小工作计划并补索引，再删已结束内容。不在本任务中自动建远程 issue。 | 你的同事提出，本助手修正依据 |
| D05 / B7 | `*_default` 与 `*_cpu` 使用相同 Backend 不等于行为一致；参数、温度、失败/fallback 策略不同，dialogue_audit 还缺少对应 CPU 版本。 | 逐方案选定要保留的行为，更新动态枚举与指南后合并；不按文件名批量删一组。 | 你的同事 |
| D06 / B11b | 大小写归一重复可以共享；彻底删除归一则会收窄可接受输入。RuntimeOptions/ExecutionTarget 当前仍能从其他入口收到小写值。 | 在“共享归一函数”和“只接受规范值”之间明确选择；不同 Backend 的设备/平台支持规则继续独立。 | 你的同事 |
| D07 / B16 | Process 的载体校验有重复，但直接删除内部校验会把 runtime 的独立调用和自定义回调当成可信输入。业务语义检查、facade 批次检查也不完全相同。 | 先确定载体校验的唯一责任层及可信值传递方式，再去重；保留 rerank 64 KiB 等更严格业务约束。 | 你的同事，本助手认可问题但要求明确边界 |
| D08 / B18 | Control 重复解析/校验值得收敛，但通用 `ValidateAndNormalizeFields` 会补默认值，不能直接替代局部更新校验；现有 Schema 还承担作者声明检查。 | 定义“只校验出现字段、不补默认、拒绝未知字段、至少一字段”的 patch 校验，并复用已解析对象。保留路由、所有广播目标修改前的结构预检及嵌套声明约束后，才删重复引擎。 | 你的同事 |
| D09 / B21 | Join/Group/Select/Scatter 没有生产调用，但有公开作者用法、测试和三个编译 starter；不属于无消费者私有函数。P15 的共享 Failure 签名和诊断改动会涉及这些函数。 | **S4 开始前**明确本轮删除或保留。若删，先处理实现、专属测试/文档/starter，再执行 P15；若保留，统一迁移所有保留消费者，不能仅改 Split/Map。不得连带删除 Split/Map 使用的来源键、失败原因等公共部件。 | 你的同事，本助手认可前置依赖 |
| D10 / B22 | PortGroupConstraint 目前用得少，但仍提供 Validator/Catalog 的机器可读能力。移到单一 Node Prepare 会失去这部分契约。 | 选择保留机器可读声明，或接受能力收缩。若收缩，再迁移 text_template 的检查并证明独立参数验证与构建错误时机符合新约定。 | 你的同事 |
| D11 / B12 其余 | 两处对齐逻辑分别面向类型擦除的 `IProvenanceReader` 与具体 vector，算法相近不代表能直接替换接口。 | 可设计共享的比较原语；先覆盖空锚点、多输出、来源不一致和发布前全量验证，再删除重复遍历。trait 合并先执行 P19。 | 你的同事，本助手拆分 |
| D12 / B24 | Pipeline 状态机在防重复 Build、未就绪 Execute/Control 和失败回收上有实际作用。完整对象工厂可简化状态，但会改变创建 API。 | 先确定工厂 API，保持 SessionContext 与计划地址稳定、失败暂存回收及执行目标的归属；不能先删状态检查再补生命周期设计。 | 你的同事 |
| D14 / B27 | `"$edgeflow_control": 1` 是当前信封识别条件，`SEM1` 是缓存键的命名空间标记；没有证据说明它们是兼容多版本的分支。 | 优先保留。若决定换成无版本判别符，明确新协议和碰撞隔离后统一更新，不把删除数字当成消除兼容实现。 | 你的同事，本助手不认可其兼容性定性 |
| D15 / B28 | Holder/Clone 可以研究精简，但输入输出含实例绑定状态，部分参数声明已不可变共享；NodeBase 还承担非模板运行时与异常职责。 | 单独设计“不可变声明 + 每实例绑定”，证明复制、所有权、绑定隔离、生命周期后再实施。仅一个生产子类不足以证明应合并 NodeBase。 | 你的同事 |
| D16 / B29a 其余 | string 诊断和结构化诊断不总是相同信息；格式化文本可能包含 code/path/context，`pipeline_diagnostic` 还影响 `-3/-2` 分类。 | 推荐内部传结构化诊断、在边界格式化，但先明确最终诊断对象与错误分类责任；不能直接用 `diagnostic.message` 代替现有完整文本。第一批只执行 P25。 | 你的同事，本助手收窄 |
| D17 / B17 扩大范围 | 跨 Prepare/IoPlan/Create 缓存已验证结果可减少更多检查，但当前计划和 spec 可修改，缓存结论可能失效。 | 第一批仅去掉同一内部可信调用链的一次重复。若进一步缓存，先确定不可变 prepared 对象和每个公共 API 的校验责任。 | 你的同事方向，本助手补充边界 |
| D18 / B33 | 具名输出 allocator 与 metadata 无当前生产消费者，但已有扩展契约及测试；删除会影响同一宿主结构多种嵌套布局的扩展方式。 | 明确不保留具名布局选择与 metadata 扩展后，再删除对应机制、测试和作者说明；普通新宿主载体仍可通过新增 binding 扩展，不必一并限制为 JSON 字符串。 | 你的同事 |
| D19 / B34 | macOS 有固定依赖包/checksum、AppleClang 和 Metal/Accelerate 路径；Ubuntu CI 不证明项目仅支持 Linux。 | 先明确运行平台和开发宿主范围。若统一 Linux，再删其他平台支持；路径输入对盘符/UNC 的拒绝与此无关，仍保留。能力收缩风险为中。 | 你的同事 |
| D20 / B35a、B35b | 不能断言 `structured_data` 与 `json_payload` 永远等价。[Invoice 测试](../tests/unit/adapter/test_io_converters.cpp) 有只填 payload 的合法构造；结构化字段默认为空对象。还需处理序列化失败与原始字节保证。 | 先选权威表示和构造不变量，梳理全部生产者、消费者及失败路径，再合并表示或删除 parse。不得删掉 payload-only 测试来伪造保行为。 | 你的同事提出，本助手不认可直接替换 |
| D21 / A13 | 日志等级 set/get 确实忽略 `name`；但它们属于当前导出接口，`logPrint` 的 name 仍有含义。仅凭函数名也不能证明其来自真实外部厂商。 | 本轮保留。若改成全局等级接口，另行明确公共 ABI 与宏的改法；不删除输出日志标签。 | 本助手提出，你的同事建议保留；本轮采纳暂缓 |
| D22 / C2 | 通用自回归生成抽象只有 llama 一个生产实现，值得评估收回 Backend 内部；改用原生 sampler 还涉及随机种子、停止与上下文行为。 | 将“移动私有实现”和“更换采样算法”分开评估。先选可接受的生成行为并补测试，不因单一实现直接替换算法。 | 本助手补充 |
| D23 / C3 | `.conf` 主要封装 `pipe_path`，存在简化空间，但它是现有部署入口，原计划也将其视为平台契约。 | 本轮保留。只有明确采用 JSON 作为外部部署入口后才能删除；R02 不依赖此项决定。 | 本助手补充 |
| D24 / C5、B19 参数子项 | Validator 已解析参数而物化阶段再次解析，确有重复；传递解析值会影响 ValidatedPipelinePlan 与 Engine 输入边界。 | 若推进，采用 Engine 可消费的中性、不可变准备结果，不能让 Engine 反向依赖 Core。保留实际实例身份、协议与并发检查。 | 你的同事提出，本助手认可问题；数据流方案待定 |
| D25 / B30 | 装配包装入口较多，CLI `validate/plan` 与部署解析存在平行验证路径；`validate-io` 和 `resolve-conf` 重叠，但根目录约束、文件解析方式、输出池深度/预算及返回内容不同。入口数量不等于独立实现数量。 | 先画出现有调用链并明确唯一准备职责、CLI 输入与输出契约，再决定合并哪些入口；保留路径策略和池预算校验，同步 Studio、dev_recipe 与文档消费者。不得将 `validate-io` 当作无条件可替换的别名直接删。 | 你的同事；本助手补回遗漏项 |

### 3.2 已核实应保留，或不采纳当前删除理由

这些项不是排在后面“最终仍要删”，而是本次明确从实施范围排除；只有新需求或新证据才重新讨论。

| 原编号 / 事项 | 最终结论 | 证据或理由 | 来源 |
| --- | --- | --- | --- |
| B19：删除 Factory 身份/类型/并发比较 | **不采纳。** | 注册允许 Definition 与 creator 返回实例不一致。[Factory 测试](../tests/unit/engine/test_model_runtime_factory.cpp) 有错误 impl/type/concurrency 的反例，比较不是恒等式。P23 不得绕过它。 | 你的同事提出，本助手否决 |
| B26：Profile 字段生成改成各处普通常量 | **不采纳当前替代方案。** | [profile_fields.json](../demo/common/profile_fields.json) 同时供 Python 读取和 CMake 生成 C++，是在维护跨语言单一来源。开发 CLI 引用 Demo 支持代码本身不能证明 SDK 反向依赖。若将来移动共享声明归属，仍需保留单一来源和打包可用性。 | 你的同事提出，本助手不认可直接删除 |
| B31：合并或删除 Copy 查询 | **保留独立所有权的查询能力。** | Prepare 在冻结前消费拥有数据的快照；借用指针和拥有副本不是同义 API。可共享查找内部实现，不能用 Get 指针直接替代 Copy。 | 你的同事提出，本助手收窄 |
| B15b：把全部部署诊断码并入 Core 枚举 | **不采纳向 Core 注入 Integration 语义。** | 接入层错误码应由接入层拥有；P20 只共享已有 Core 字段校验错误的映射。若统一表示，应选中性结构，保持依赖方向。 | 你的同事提出，本助手明确边界 |
| 删除 CONTRIBUTING 中 `biz` 命名规则 | **保留。** | 业务标识命名与废弃的 BizDefinition 类型不是同一概念。 | 你的同事复核后撤回 |
| 删除 `CreateFromIoPlan` | **保留。** | 当前 `Create` 在生产路径调用它。 | 你的同事复核后撤回 |
| 删除 `FieldBuilder::Maximum`、`InputFlow::PreserveByRequest` | **本轮保留。** | 属于作者接口的对称表达，当前删除收益不足。 | 你的同事自查撤回 |
| 删除 `generate_acceptance_evidence.sh` | **保留。** | CI 有实际调用。 | 你的同事复核后撤回 |
| 合并 `ExternalOutputSlot` 四行包装 | **本轮保留。** | 缺少值得承担改动成本的具体收益。 | 你的同事复核后撤回 |
| 删除 facade 或协议壳的一个异常屏障 | **保留各自 API 屏障。** | facade 是可独立调用的 `noexcept` API；不同边界的异常保障不等于重复 catch。 | 你的同事自查撤回 |
| 合并三套失败阶段枚举 | **本轮暂缓。** | 当前架构将错误码中立化另行处理，不能借删除字段跨过该设计。 | 你的同事自查暂缓 |
| 删除可选宿主槽、保留名 `common` | **本轮保留。** | 属于现有可加扩展契约；仅测试使用不足以认定是废弃概念。 | 你的同事复核后保留 |

实施时，第一部分以 P 编号追踪，第二部分单独选择 R 项，第三部分先记录 D 项或保留项的新决策再改变范围。**执行范围和顺序以本文件为准；B5、B19、B26、B35a 等已转入待决策或明确保留的内容，不得作为“顺手清理”重新并入优先阶段。**
