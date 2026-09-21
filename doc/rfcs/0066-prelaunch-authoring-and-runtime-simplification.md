# RFC 0066：上线前全业务开发简化——本次实施范围

- **RFC 编号**：0066-prelaunch-authoring-and-runtime-simplification
- **创建日期 / 本次修订**：2026-09-21
- **文档状态**：Completed
- **关联分支**：`refactor/all-biz-authoring`
- **目标版本**：投产前，按本次筛选范围交付
- **负责人 / 作者**：LLM-EdgeFlow maintainers
- **审查基线**：`2cb00622e4fffe69de176550e9bd47351b1e0cb1`
- **关联决策**：遵守 RFC-0059/0060 的独立转换与唯一 Operator 边界、RFC-0064/0065 的单一实现原则。

## 1. 本次决定

本次只推进：**类型化声明复用、少量公共函数提取、全部业务迁移与验证、轻量查询和指南整理**。
这是通用框架的作者接口简化，不是 Translate 业务优化。所有现有八个业务均在范围内；新增能力依据多种数据形态设计和验证。
全业务覆盖指每个业务采用适用的公共机制并完成验收，不要求音频、多槽、排名结果套进同一个文本转换模板。

本文完整替代此前宽泛候选计划；原 P0–P5 不再是本次实施清单。原十二项建议的未选部分见第 5 节，均非本轮交付前置。
用户本轮明确“不改变外部契约”。此前因文字歧义而冻结所有内部接口的假设取消：纯内部辅助接口可以调整，同步迁移仓内调用并删除旧实现。
SDK 调用接口及已向外部承诺的扩展接口仍受兼容保护；源码扩展头与 SDK 公开头的区别见[源码布局](../dev_guide/source_layout.md)。
用户已要求实施，下述范围已完成开发与逐阶段验证，证据及验证边界见第 7 节。

### 1.1 保持的外部契约

- Operator 函数表、签名、异常屏障、平台 DTO 布局、key/suffix、完整请求响应及业务 status。
- 现有接受/拒绝条件、optional 与 null 的区别、有效批次限制、容量默认、长度/NUL 规则、错误码、检查优先级和 SDK 原始诊断。
- 输入借用、输出租约、失败发布/回滚、池满等待、Destroy/DeInit 及输出释放顺序。
- Pipeline / conf 格式与路径解析，已有 biz/converter/binding/schema IDs、Catalog 字段和值、注册审计行为。
- 既有 CLI 的输入、输出和退出码。新 describe 命令为增量入口，单独定义其行为。

发现原有缺陷时单列修复，不用“简化”静默改变公开行为。纯内部测试接缝不必永久保留；若仅仓内使用，可同步调整测试及调用方。
不为兼容已删除的内部辅助代码增加一整套长期双轨接口。

## 2. 全业务覆盖矩阵

当前八个生产绑定分别使用八个输入、八个输出 converter ID。Entity 与 Keyword 输入同在 text_input.cpp，但载体和注册 ID 不同；共文件不等于共享协议。
以下表格用于迁移与测试，不能替代运行时 Catalog。外部逐请求响应不代表内部都是单端口、单结果或无展开。

| 业务与 biz ID | 必须保留的差异 | 本次通用机制的应用及重点验证 |
| --- | --- | --- |
| 翻译 `translate_v1` | entity 载体内是完整 JSON；提取 query，组装 translated | typed 端口、文本访问、单结果关联、字符串容量；完整 JSON 请求响应 |
| 实体抽取 `entity_extract_v1` | 同为 entity 载体，但输入是纯文本；输出 StructuredDocumentBatch | 复用文本机械操作，保持独立协议；结构化失败/fallback 拒绝 |
| 关键词 `keyword_match_v1` | keyword 专属载体；RuleMatchBatch 带命中与状态 | typed 声明、文本操作、结果写入；状态取自结果，不能统一写成功 |
| 文档问答 `smart_doc_qa_v1` | query 必需、doc 可空；answers/intent_matches/chunk_counts 三路结果 | 可选字段与多结果关联；每一路乱序、缺失、重复，标量与字符串输出 |
| 合规审核 `dialogue_compliance_audit_v1` | 可空 channel；verdict 与 ranked policy；非同名端口映射 | 保留风险枚举/分数/rank 语义；三字符串容量、fallback、非同名绑定 |
| 音频 `speech_audio_asr_intent_slot` | PCM 与采样率，自持有 AudioPcmBatch；转写和意图两路结果 | 输入复制生命周期、两路关联、状态及字符串输出；不套文本输入 helper |
| OCR `multimodal_ocr_invoice_qa` | frame + string 两个外部输入槽；ID 来自 frame；票据和 OCR boxes | 多槽访问、显式 ID 来源、两路关联、JSON 和 box count；缺槽/错型 |
| 重排 `dense_cross_rerank_scoring` | 一请求多个 candidates；内部展开、排名、original_sub_id；固定数组输出 | typed 声明、外层批次/ID、既有排名关联；不同展开数、乱序、重复/越界来源；不使用字符串 writer |

准确声明见 [biz 目录](../../src/adapter/biz/)。所有八个输出已使用 [IndexResults](../../include/adapter/result_validation.h)，本轮复用其算法。
Audit 已存在 matched_policies → matched_policy；默认同名映射不能吞掉该差异。
七个输出有字符串写入需求，Rerank 是合法例外；不能为统一代码形状给它增加无用的字符串抽象。

## 3. 本次实施工作包

### A. 类型化声明与登记去重（主线）

把逻辑端口的名字和 C++ 类型声明一次，Definition、回调访问和映射组装引用同一声明。
基于现有 BlackboardKey、PortDefinition、Bindings 增加最小辅助，不新增全局 key 注册表或一套端口类型系统。
业务 Blackboard key 与 converter 逻辑端口保持各自含义；非同名映射显式表达，不能直接拿常量绕过 bindings。

有限的普通工厂或组装函数复用已声明的类型、名称与批次值，减少 positional initializer 和重复填写。
不同来源的限制仍独立存在，运行时继续执行其交集，不能把所有 batch 上限强制合并为一个全局值。
BizDefinition、IoBinding、exposure 仍产生现行完整元数据，不能仅从 converter 的读写集合反推业务 ingress/egress。

业务专属代码可按修改内聚性同文件组织；共享实现和可复用 converter 保留独立引用。
先做声明共用和窄组装，不以 REGISTER_BIZ_PACK 宏或一业务一文件为交付指标，不新增 registry，也不要求固定输入/输出永久配对。
若登记辅助仅隐藏原有函数调用、没有删除重复事实，则保留显式登记即可。

**验收：**八个业务的声明/访问采用共享 typed 信息，非同名映射及跨绑定复用通过；现有 Catalog 和注册失败语义保持。

### B. 输入输出的机械操作去重（主线）

优先扩展现有 [converter_authoring](../../include/adapter/converter_authoring.h)、AdapterValidationHelper、ValueType 与 IndexResults。
只提取有多个真实消费者、规则确实相同的普通函数；不为每类操作分别建立对象层级。

| 交给公共函数 | 保留在业务函数 |
| --- | --- |
| 相同的槽位取得、指针/长度处理与自持有文本复制 | JSON 字段语义、可选字段业务默认、PCM/图片/候选的解释 |
| 通用批次/ID 操作和 Publish/Read 周边重复代码 | ID 来源、candidate 展开、sub_id、排序与聚合 |
| 现有 IndexResults 调用周边的相同准备/检查 | 多路结果如何组合、risk/rank/fallback/status 判断 |
| 依据有效池规格检查容量并写入字符串 | 响应字段与序列化、真实容量需求、非字符串结构填充 |

不以 Translate 的“一次文本解码 + 一次 JSON dump”定义通用驱动。复杂业务继续显式控制循环和结果组合。
Operator 已有预校验继续作为生产入口的统一安全边界；抽取时保持实际检查顺序，不再建立一套字段约束 DSL。
重复 batch 常量改为复用对应声明；有效批次仍取现有适用限制的交集。

生产输出已有规范化 Spec，writer 从中取容量，业务不再手抄 2047 等 fallback。
内部手工 view 的构造可统一补齐有效 Spec；若某入口属于已承诺的外部扩展契约，则继续保持其原有行为。
不能通过删掉错误用例或改变公开错误次序证明“等价”。writer 只处理真实容量，不推算模型输出上限，不重做内存池。

**验收：**所有适用消费者迁移，七个字符串输出与 Rerank 非字符串路径分别覆盖；重复代码删除，业务差异仍可直接阅读。

### C. 全业务迁移、验证与示例收敛（必做，不是只迁移试点）

八个业务全部进入 A/B 的审查和迁移清单，逐项记录采用的公共函数、保留的业务代码以及测试证据。
开发可以小步进行，但 Translate 通过不构成阶段总验收；设计定型前就必须看过多槽、多路结果、PCM 和排名展开。
不要求每个业务调用每个 helper；任何“不适用”须由真实数据形态解释。

复用现有 AdapterHarness、converter 单测、purity、Operator 契约与集成测试；只补缺失断言，不另造 NodeHarness/PipelineTestRig API。
已有基础包括 [复杂转换器测试](../../tests/unit/adapter/test_complex_converters.cpp)、[纯度与组合测试](../../tests/unit/adapter/test_adapter_purity.cpp) 和 [文本转换器测试](../../tests/unit/adapter/test_text_converters.cpp)。
可用确定性模型替身验证 SDK 编排，但输入输出须经过真实 converter；真实模型效果和目标硬件验收另列，不能被默认测试通过替代。

指南按任务分流：外部协议不变优先配置复用；新增算法走已有 Node Spec；新增协议才进入 converter/binding。
示例直接链接实际编译和测试的业务：覆盖纯文本、JSON、多结果、多槽、PCM、展开排名，不维护另一套独立模板源码。
更新接入指南、源码布局和目录说明，使推荐路径与本轮实现一致。

**验收：**第 2 节八行都有源码迁移/保留说明与验证结果；同步删除旧重复实现，现行文档不再要求作者重复填同一事实。

### D. 两个查询命令与现有容量预检入口（独立小项）

新增 describe-model、describe-backend，直接复用当前 ModelToJson/BackendToJson，按现有 describe-node 的方式提供名称查询与错误结果。
既有 catalog 输出不变；本次不做 catalog --summary 或第二套展示数据模型。

接入指南直接使用现有 catalog、validate、plan、resolve-conf --depth，不再增加新的总控命令。
resolve-conf 已输出有效池 type/allocator/params/metadata/capacities，示例使用与宿主一致的 depth。
普通作者复用已注册载体及 MakePooledOutputBinding，只了解必要容量覆盖与输出释放；自定义 allocator 留给特殊载体作者。
宿主的 required 输出 key 准备优先展示现有公共 Demo runner，不为此增加 SDK 函数或另一套宿主封装。

**验收：**新查询覆盖合法/未知名称；旧命令保持；文档说明容量预检能证明配置准备，不能保证任意未来响应都装得下。

## 4. 实施顺序和停止条件

| 阶段 | 工作 | 阶段完成证据 |
| --- | --- | --- |
| S0：全量基线 | 列出八业务声明、关键差异、公开行为与已有测试，记录重复项 | 先覆盖全部形态；不存在“先按 Translate 设计完再考虑其他业务” |
| S1：声明简化 | A；同步迁移全部业务的 typed 引用与适用登记组装 | Catalog、非同名映射、独立 converter、缺 binding 和冲突审计保持 |
| S2：机械步骤去重 | B；对全部八业务应用适合的函数并删除旧重复体 | 多槽/多结果/PCM/排名及七类字符串输出验证；生产成功和失败行为等价 |
| S3：交付收敛 | C/D 的剩余验证、查询、指南、示例和删除项 | 八业务逐项验收、单一实现、默认门禁；实际开发体验单独记录 |

D 可与 A/B 并行；它不成为主线的技术前置。每阶段提供可独立评审和回退的变更，不删除其他业务才能让某项通过。

停止抽象的条件：只有一个消费者、需要大量业务开关、参数传递比原代码更难理解，或抽象后仍须同步维护旧声明。
此时保留清楚的业务实现；不为追求同一模板或源码行数强制泛化。

## 5. 本次明确不实施

| 原候选 | 本次处理及原因 |
| --- | --- |
| 通用 NVI/Unary 编解码框架、字段约束 DSL | 不做；普通函数与现有校验足以承担本轮重复，避免新继承层和规则解释器 |
| 强制 REGISTER_BIZ_PACK、一业务一文件 | 不作为目标；只保留能减少重复事实的窄组装和合理内聚 |
| scaffold_biz / 新 recipe 生成器 | 延后；先稳定真实作者路径，用可运行业务作示例 |
| AutoInput 指针登记、实例反射、自注册端口 | 不做；已有静态 Spec，额外状态与生命周期负担不合算 |
| Node/Backend 跨层参数内核、全量参数迁移 | 延后；本轮不跨层扩建，也不改模型/后端协议 |
| RequireSession 泛化 | 延后；对普通业务作者收益低，不捆绑本次接入改造 |
| export-schema、Studio 连线/参数 UI、catalog summary | 延后；有独立价值，但不是本轮减少重复代码的必要条件 |
| NodeHarness::ForInstance、全局隔离接缝、PipelineTestRig | 延后；复用现有测试即可证明本轮改动，不扩展 Core 验证依赖 |
| SDK 错误原文重写或仅返回码 | 不做；公开诊断保持，不丢底层原因 |
| 所有权升级、跨 Destroy 保活、池满策略改变、自动容量预测 | 不做；涉及外部行为或无法可靠推导，不是本次机械去重 |

通用框架不要求所有层同时改动。保持四层职责、唯一 Validator、已有 Node Spec 和 Model/Backend 分工；本次实现主要落在 Integration、CLI 与相关测试/指南。

## 6. 验收与复杂度预算

### 6.1 契约和通用性

- 八个业务分别核对成功结果、外部 ID（含重复 ID）、业务 status、结果数量/顺序和完整响应字段。
- 覆盖批量交集边界、缺槽/错型、optional/null、零长度、NUL、超长、不同容量和无部分发布/租约回滚。
- 多错误同时存在时保持错误次序，例如非法输出槽与非法 JSON、缺结果与容量不足；错误码与原始诊断保持。
- 依第 2 节验证多路来源、PCM copy-in、多槽 ID 来源、非同名绑定、candidate 排名与原索引；不以 Translate 的成功样例代替。
- 现有 Catalog 的字段、值、顺序/省略规则、生产曝光及注册审计保持；允许的 converter 跨绑定复用继续成立。
- 生命周期与池行为保持；新 writer 不提高配置容量，不增加持有租约，也不承诺跨销毁保活。
- 在现有测试入口增加必要的非同名/复用组合用例，证明辅助代码不依赖八个 biz 名字或固定端口数；不新增虚构生产业务和注册。

### 6.2 是否真正更简单

- 比较八业务总重复量，并计入新增公共支持代码；不能只报告 Translate 缩短了多少行。
- 同一逻辑端口的名字/类型不再散落于 Definition、回调和映射的重复字面量。
- 公共 helper 有至少两个真实消费者，删除对应重复实现；辅助代码不硬编码业务 ID，不增加 registry/DSL/继承体系。
- 业务差异仍集中可读，排查单个错误不需要穿过多层包装；未来新增载体/多槽业务不需要修改中央按 biz 分发的代码。
- 必须声明的信息仍显式，不通过猜测同载体同协议、输出必成功或所有 mapping 同名换取行数减少。
- 更新后的指南可完成配置复用、普通接入和复杂接入任务；真实开发者试用的首次成功时间/求助记录独立于工程测试结果，无试用不声称体验已验证。

## 7. 实施与验收记录

### 7.1 已实现范围

- A：八输入、八输出、八 binding 复用 typed 声明；42 个映射改用 BindIoPort，16 个 Definition 的17个外部槽使用小工厂。
  Audit 的逻辑 kMatchedPolicies 与实际 kMatchedPolicy 显式对应；未增加注册表、聚合宏或永久配对限制。
- B：八输入共用 context/批次检查和受检槽访问，七个文本相关业务使用结构校验/自持有复制，PCM 保留专用复制。
  实体/关键词相同代码收敛为文件内模板。八输出21处读取、七输出11处字符串写入使用普通函数，IndexResults 及业务语义保留。
- C：所有八业务均迁移并通过既有 purity/复杂 converter/OperatorGolden 覆盖；新增非同名 typed 映射、精确容量、NUL、错误优先级和整批失败回滚断言。
  Harness 用实际字符数组大小减一提供容量；空/缺 Spec 不再靠业务默认值掩盖，原 GetSlotCapacity/CopyToOperatorString 接口行为保持。
  现行接入指南、源码布局和三目录说明均更新，示例引用实际参与编译测试的八业务实现。
- D：describe-model / describe-backend 直接序列化 Catalog；全部6模型、2后端的查询与 Catalog 对应项一致，未知名称及参数数量负例已覆盖。

生产字符串 writer 要求有效 Spec 与既有[输出分配规范](../dev_guide/operator_output_allocation.md#转换与有效期)一致。
本次同步修正内部手工视图；没有向缺规格的缓冲区猜测容量，也没有改 SDK 的输出准备协议。
后续评审消除了 writer 的 `find + at` 重复查找，缺 Spec / 字段仍返回原有诊断；
指南明确了同名槽位工厂的适用范围及 `KeySuffix()` 的回退规则。

### 7.2 阶段证据

| 阶段 | 实际结果 |
| --- | --- |
| S0 | 原构建16项 Adapter/IO/Operator CTest通过，八项 OperatorGolden 实际通过、无跳过；保存全量/八业务 Catalog 与12个 Node 查询 |
| S1 | 构建成功；16项基线与22项 CLI 共38/38通过；全部原查询与S0逐字节一致 |
| S2 | 构建成功；加入3项 Demo 的41/41阶段测试通过，无GTest跳过；全部原查询继续与S0逐字节一致 |
| 配置与计划 | 17个生产Pipeline的34次validate/plan中20次通过；9个Mock Pipeline使用test工具18/18通过；合并覆盖全部八业务 |
| 容量预检 | 指南中的 resolve-conf configs/pipeline_keyword_match_rules.conf --root . --depth 1 返回0 |
| 独立评审 | 核对全部生产/测试diff，未发现阻断缺陷；业务分支、校验次序、诊断字段与资源生命周期保留 |
| writer 分配计数 | GCC 13.3 / libstdc++、C++17，直接调用修正前后 writer，各字段64次成功写入且排除视图构造；现有长字段各128→64次分配，短字段0→0；这是局部计数，不代表整批编码耗时 |

当前构建未启用 kite_llm / whisper_cpp，以下7个生产配置的14次校验/计划返回 UNKNOWN_BACKEND，未通过部分不能计作成功：
pipeline_audio_asr_cpu、pipeline_dialogue_audit_kite、pipeline_doc_qa_kite、pipeline_doc_qa_kite_generated_embeddings、
pipeline_doc_qa_rerank_kite、pipeline_entity_extract_kite、pipeline_ocr_doc_qa_kite。
未修改这些配置或启用额外 Backend；Mock 验证不能替代真实后端/模型/硬件验收。
OperatorGolden 的 CrossRerank 使用 ONNX 小型 fixture，其余采用 Mock 或规则路径。

原始基线和阶段日志保存在本地 /tmp/all-biz-authoring-baseline、/tmp/all-biz-authoring-s1、/tmp/all-biz-authoring-s2 及对应阶段日志。
最终按 [CONTRIBUTING](../../CONTRIBUTING.md#6-run-one-canonical-delivery-gate)运行当前交付版本门禁，结果在交付回复报告；失败时恢复实施状态后修复。

### 7.3 复杂度与体验边界

统计八业务所在23个cpp及三个受影响作者头：基线2956行，当前2721行，**计入公共支持代码后净减少235行**。
其中输入937→772、输出1095→887、binding 481→465；测试与文档增量不计入此生产代码指标。
未新增 registry、DSL、继承体系、业务分派或源文件模板集合，源码组织仍沿用现行职责目录。
工程迁移与验证已完成；未开展真实开发者试用，首次成功时间、求助次数与理解成本改善仍待实际体验记录，不能用Agent测试代替。
