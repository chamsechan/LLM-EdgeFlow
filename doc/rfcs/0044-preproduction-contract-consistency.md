# RFC 0044: 投产前跨实现契约一致性整改与实施指南

- **RFC 编号**：0044-preproduction-contract-consistency
- **创建日期**：2026-09-07
- **文档状态**：Completed
- **关联分支**：`fix/preproduction-contract-consistency`
- **目标版本**：v10.x / 首次生产接入前
- **负责人 / 作者**：LLM-EdgeFlow 维护团队 / Codex 辅助整理
- **审计基线**：`f1053271de6a281ed51400e8fd71b7d7177b002e`
- **关联决策**：补充 [RFC-0012](0012-node-authoring-experience.md)、
  [RFC-0018](0018-request-context-and-handle-concurrency-contracts.md)、
  [RFC-0021](0021-layer4-authoring-and-protocol-convergence.md)、
  [RFC-0023](0023-v6-contract-convergence.md)、
  [RFC-0043](0043-config-parameter-consistency.md)；
  继续遵守 [RFC-0029](0029-external-readiness-and-intranet-sdk-migration.md) 的环境边界。

## 1. 问题与范围

### 1.1 为什么现在需要修改

框架尚未投入生产，适合在外部调用方形成依赖之前明确共同契约。此次整改的目的，是让
同一接口在不同实现、缓存策略、入口和执行模式下提供一致的正确性保证。
缓存返回其他输入的结果、错误配置被接受、异常导致进程退出属于必要修复；
仅为使代码看起来相同而重写继承关系、错误码或 Backend 实现，不属于必要修改。

审计覆盖 12 个生产 Node（11 common、1 custom）、6 个 Model、4 个 Backend 源码实现、
7 个 Adapter 和 7 个 Operator Bridge。基线生产 `alg_pipeline_tool catalog` 实际注册
12 个 Node、6 个 Model、7 个 biz 和 2 个 Backend；另两个 Backend 受构建开关控制。
这些数量仅记录本次审计范围，实施时以目标构建的 Catalog 为准，不作为新的硬编码目录。

此前默认质量门禁 89/89 项 CTest 成功，但其中 8 个内部测试因可选 Backend 或真实模型
条件不足而跳过。本次针对性复现发现了门禁尚未覆盖的边界。
**§1–§4 记录审计基线及采用的决策；实施和验收结果见 §5.3，不代表目标硬件生产验收。**

### 1.2 问题清单与优先级

优先级用于安排本 RFC 的实施顺序，不等同于生产事故等级：
**A 为优先修复的结果正确性 / 进程可靠性问题；B 为首次生产接入前必须完成的契约修复。**
所有 A、B 项均属于本 RFC 完成范围；不以尚未投产为由保留已证实缺陷。

| ID | 优先级 | 已确认的问题与复现结果 | 主要归属 | 方案 |
| --- | --- | --- | --- | --- |
| C01 | A | session Embedding 缓存存在确定性身份碰撞，不同语料成功返回同一份向量 | 能力节点层 | §2.2 |
| C02 | A | Operator / ONNX / llama 的部分 `noexcept` 路径在诊断分配失败时 `terminate` | 接入适配层、模型执行层 | §2.3 |
| C03 | B | 一对多文本作为一对一业务出口，预检和执行成功，Adapter 打包才失败 | 流程编排层、接入适配层 | §2.4 |
| C04 | B | `channel_name` 257 字节：C 接受，Operator 拒绝；C 对该字段未做有界检查 | 接入适配层 | §2.5 |
| C05 | B | 合法采样率、零长度 PCM：C 接受，Operator 拒绝 | 接入适配层 | §2.5 |
| C06 | B | `metadata_type_id=4294967297` 被成功窄化为合法 ID `1` | 接入适配层 | §2.5 |
| C07 | B | TextChunk 非法 UTF-8 返回 `-4002`，请求状态仍为 `0 / OK`；执行模式影响诊断 | 能力节点层、流程编排层 | §2.6 |
| C08 | B | TextChunk 的 counts 丢失原 sub_id；同 req 的多个输入生成重复切片来源 | 能力节点层 | §2.7 |
| C09 | B | 同样有限的大数输入，BGE 成功产生零向量 / Inf / NaN，另一 Embedding 实现正常 | 模型执行层 | §2.8 |
| C10 | B | Node 与 Model/Backend 的 Definition 校验分叉；Node 配置回调非标准异常逸出 | 流程编排层、共享配置契约 | §2.9 |
| C11 | B | 直接 Node Init 接受严格预检拒绝的配置；`ValidateInput` 为未接入生命周期的接口 | 能力节点层、接入适配层 | §2.9、§2.10 |

C08 的默认业务入口通常使用 `sub_id=0`，其风险主要在节点复用和扩展组合。
C09 的复现使用中性协议测试替身返回有限极端数值，不代表真实模型已经输出此类数据。
C02 使用分配失败注入，属于低概率但影响进程存活的边界。
C11 不表示现有 Adapter 绕过了所有验证：内建字段校验实际位于 `Unpack`。

### 1.3 保持的边界与暂不实施项

- 保持接入适配层 / Integration → 流程编排层 / Orchestration →
  能力节点层 / Capability Nodes → 模型执行层 / Model Execution 的职责方向。
  共用的纯值类型和校验原语可位于现有 `include/contracts/`，不得依赖上述层的具体实现。
- 不增加业务 Node、模型能力、Backend、平台结构或第三方依赖；不把平台字段放入 Node。
- 不统一合理差异：OCR 多输出、Rerank 聚合、Backend 并发模式、C 固定容量输出与
  Operator 输出池、不同语义的错误码和生成参数默认值。
- 不新增通用配置语言、配置反射、统一业务调度 switch、全图基数推导系统或全新测试框架。
- 不在本次重写所有 Model 的构造方式，也不要求所有 Node 使用相同继承深度。
- 文本生成协议的可选 seed 支持差异列为后续能力描述议题；本次审计只有源码依据，
  不把未完成厂商运行验证的结论混入本 RFC 的已复现修复项。
- 缓存淘汰、容量预算和性能优化可在有真实负载数据后另行评估；缓存身份正确性必须先修复。
- 不请求、复制或集成公司内部 SDK；实际 SDK 集成和目标硬件验收继续由 RFC-0029 管理。

## 2. 决策与权衡

### 2.1 统一到哪一层

| 共同契约 | 唯一责任位置 | 其他层如何复用 |
| --- | --- | --- |
| 字段类型、默认值、数值范围、枚举及 Definition 自身合法性 | `include/contracts/` 的纯配置校验原语 | Validator 生成结构化诊断；Node 的防御性初始化调用同一原语 |
| DAG、端口连线、业务出口、模型绑定 | `PipelineValidator` | CLI、Studio、Build 消费同一结果；Node 不调用 Validator 做图规划 |
| C / Operator 对等业务输入约束 | Integration 的业务输入约束与校验辅助函数 | 两种表示各自检查指针/长度，再共用语义约束 |
| Node 返回码与本次执行诊断的汇总 | `Pipeline` 执行包装；Node 自身使用 `Fail/Require` | 顺序与并行复用同一调用包装 |
| Embedding 数值语义 | Model Execution 的共享数值辅助函数 | 两种 Embedding Model 调用；Node 不再次归一化 |
| 不跨边界抛异常 | 各公开边界所属层 | 内部可抛函数由边界保护；内部不可抛函数自行兑现保证 |

只在已有重复实现或明确复用点提取辅助函数。`contracts` 不引用 `PipelineDiagnostic`、
`NodeDefinition`、平台结构或 vendor 类型，避免为了复用造成向上依赖。

### 2.2 C01：缓存身份必须完整且无歧义

**现状位置：**[text_embedding_node.cpp](../../src/common_nodes/text_embedding_node.cpp)、
[SessionContext](../../include/core/session_context.h)。

当前 `ComputeDigest` 把 req_id、sub_id 和文本字符连续混合，缺少字段与条目边界。
下面两组合法配置语料由两个 `TextCorpusSourceNode` 产生后，可获得同样摘要：

```json
{"corpus_a": ["a", "b\u0000\u0001c"], "corpus_b": ["a\u0000\u0001b", "c"]}
```

两组来源和条数相同。用文本长度作为测试向量，A 应为 `[1,4]`，B 应为 `[4,1]`，
当前第二次实际返回 `[1,4]`。这个复现不能靠命中后检查 count/provenance 修复。

**采用方案：使用完整、带版本和长度边界的 session 内缓存键。**

1. 在能力节点层的局部支持代码构造二进制 `std::string`，包含版本标识、模型 ID、
   模型 revision、normalize 选项、条目数，以及逐条 req_id、sub_id、文本字节长度和原始字节。
   所有变长字段都显式写长度，整数采用固定宽度与明确字节序；嵌入 NUL、空文本和 Unicode
   字节均原样编码。禁止直接拼接带分隔符的 model ID/revision。
2. 直接用完整编码作为 `SessionResourceKey` 的身份；底层 unordered_map 的哈希只负责
   查找，其完整键相等比较负责正确性。第一版不再使用摘要字符串作为唯一身份，
   不引入外部哈希依赖，也不把“换一个更强哈希算法”当成完整修复。
3. 继续使用 `SessionContext::GetOrCreateResource` 的同键单次创建机制；缓存结果发布后
   按不可变值使用，不缓存 `AlgContext*`、输入指针或请求拥有的视图。
4. 保留 miss 路径的模型输出 count/provenance 检查；hit 路径也防御性检查对齐。
   精确相同输入才允许复用；文本、顺序、ID、选项或 revision 改变都不能复用旧值。
5. 推理失败、输出对齐失败不写入成功缓存，后续请求可以重试；同时命中者失败时必须
   得到失败码，不能发布空指针或部分输出。保持现有 session 销毁时释放资源的生命周期。

**代价与替代方案：**完整键增加一份与语料长度成比例的持久内存，并需要构造键。
目前缓存针对 session 静态语料，优先用简单、可证明的等值比较消除错结果。
日后确有内存压力，可改成“摘要索引 + 完整身份等值核对”的桶结构，不能退回只信摘要。
普通 request 路径不生成该键；不建立进程全局缓存。

### 2.3 C02：让 `noexcept` 的承诺覆盖整个入口

**现状位置：**[Operator ValueType 校验](../../src/adapter/operator/operator_value_type_registry.cpp)、
[ONNX Backend](../../src/engine/backends/onnxruntime/onnxruntime_backend.cpp)、
[llama Backend](../../src/engine/backends/llama_cpp/llama_cpp_backend.cpp)；
参考 [Whisper Backend](../../src/engine/backends/whisper_cpp/whisper_cpp_backend.cpp)
和 [Operator Control](../../src/adapter/operator/operator_control_registry.cpp) 已有完整保护。

采用两类边界，逐个函数归类，不用全库机械删除或添加 `noexcept`：

- 必须不可抛的公开边界：六个 `Alg_*`、Operator 导出入口、Backend `Load`、中性 session
  的不可抛执行接口、Model 不可抛能力接口。入口所有可能抛异常的工作均置于保护范围内，
  包括空参诊断、协议检查、路径转换、字符串构造和对象创建；同时捕获标准与未知异常。
- 私有辅助函数：若有 `std::string`、容器、文件系统等分配行为，优先允许异常传播到
  所属边界；只有调用契约确实需要不可抛时，才保留 `noexcept` 并自行完整捕获。
  审查 `ParseKey`、字符串/Buffer/Any 校验、输出池复制、注册失败记录等相同模式。

诊断辅助函数接收 `string_view` 或字面量，把赋值置于内部 try/catch。
不要在调用辅助函数之前拼接临时字符串，也不要在 catch 内无保护地分配错误消息。
资源耗尽时允许错误文字为空或退化为固定文字，但失败码必须保留、进程必须存活，
不得为了“保留详细消息”再次抛出异常。清理路径同样必须可安全执行。

不把不同层的所有错误码改成一个数字；继续遵守各边界已有返回码约定。
验收比较的是“正常返回失败、未发布结果、资源释放、异常未逸出”，不是诊断文字逐字相同。

### 2.4 C03：统一端口流检查，同时正确表达 Adapter 聚合

**现状位置：**[PipelineValidator](../../src/core/pipeline_validator.cpp)、
[Catalog 定义](../../include/core/pipeline_catalog.h)、
[Adapter 实现](../../src/adapter/adapters)、
[IndexResults](../../include/adapter/result_validation.h)。

1. 将现有端口兼容检查拆成可复用的纯检查部分与诊断构造部分，让 Node 消费端和 biz
   egress 都检查 type、cardinality、provenance、lifetime。未知/非法元数据在注册阶段拒绝；
   不在 Adapter、CLI 或 Studio 再做一份图规则。
2. 业务 egress 是 **Adapter 消费内部数据的端口**，不是公共 C 输出结构体数量声明。
   普通文本、文档、计数等一对一出口继续要求 `1:1 / preserve / request`。
3. 对已有 ranked 消费者，按真实打包行为声明 `N:1 / aggregate / request`：
   `dense_cross_rerank_scoring.ranked_results` 与
   `dialogue_compliance_audit_v1.matched_policy` 当前允许每个请求多个排名结果。
   CrossRerank 按请求聚合为一个包含排名数组的输出，保留所有合法候选；Compliance
   选择 `sub_id=0 / rank=1` 的首项。修改相应 Adapter 的 Definition 声明，
   保持外部 `OutputCardinality::kOneToOne`。禁止为了让门禁变绿把所有 egress 改成 `N:M`。
4. 保持当前消费者为 `N:1` 时可接受一项或多项的兼容方向。对于普通一对一出口，
   已明确声明的 `1:N` 生产者必须在预检被拒绝。有效 lifetime 先按配置解析，
   再共用已有生命周期兼容逻辑。
5. 保留 `IndexResults` 的运行时检查：req_id 合法、来源不重复、每个请求存在首项，
   非 ranked 出口要求 `sub_id=0`；有 rank 字段的业务继续检查 rank 语义。
   静态 cardinality 是粗粒度声明，不能证明任意 `N:M` 中间节点的真实输出条数，
   不得因此移除动态检查或声称预检可以证明所有结果合法。
6. egress 的 `required` 必须被尊重：必需出口缺失报错；可选出口缺失允许，存在时仍检查。
   本次不主动把现有必需出口改成可选，也不移除未直接序列化的业务契约端口。
7. 复用现有 `PORT_*_MISMATCH` 诊断码，路径指向生产节点的实际输出绑定位置，记录实际 key
   和 `$egress` 关联信息；缺失出口沿用 `MISSING_BIZ_OUTPUT`。错误在模型加载前可观察。

**实施注意：**不能仅把 Node 间的判断复制到 egress 后直接提交。
必须在同一变更中修正上述 ranked 消费端声明，并验证所有已注册业务的合法方案。
本次不增加 Pipeline JSON 字段，Catalog 的现有字段会更准确地表达出口语义。

### 2.5 C04—C06：共享业务输入约束和安全数值转换

**现状位置：**[Adapter 校验辅助](../../include/adapter/adapter_validation_helper.h)、
[Operator 内建 ValueType](../../src/adapter/operator/operator_builtin_value_types.cpp)、
[部署配置解析](../../src/adapter/operator/company_conf_resolver.cpp)。

在 Integration 内集中复用现有字段上限与语义校验函数，例如放入小型
`include/adapter/biz_input_validation.h`（拟新增路径，最终名称按实际类型确定）。
不增加通用反射表；C 指针结构与 Operator `CompanyString` 保留各自表示检查。

| 输入/字段 | 统一后的规则 |
| --- | --- |
| `channel_name` | 可缺省；存在时允许空串，最多 256 个有效字节；C 路径有界扫描，Operator 检查声明长度；不计 C 结尾 NUL |
| PCM 长度 | 允许 0；0 时允许 null buffer，不解引用；正长度要求有效 buffer，负值或超限拒绝 |
| PCM 采样率 | 即使零采样，也必须在当前业务支持的有效范围内；不要将空输入变成绕过采样率验证的路径 |
| 音频容量 | 检查样本数和乘字节数溢出；保留当前资源上限，不因空音频修复而扩大范围 |
| `metadata_type_id` | 转换前按原始有符号/无符号数值检查 `int32_t` 范围，再检查原值对应的白名单与 meta_num 组合；不通过 double 或先截断后验证 |

两个入口采用相同默认业务限制时，对等数据必须同判合法/非法。
Operator 显式配置更严格资源限制时可以拒绝 C 默认限制内的数据；这是部署约束差异，
测试应单独覆盖，不能为了“相同”忽略显式限制。
长度为负、指针与长度不一致等表示问题仍由各入口各自诊断。

将相邻 `meta_num`、capacity 等安全转换方法复用于 ID 转换，错误返回原有配置错误码，
诊断明确字段路径。`4294967297`、`-4294967295`、超过有符号范围的 JSON 无符号整数
均必须拒绝，不能静默解释成已有类型。

零采样允许规则补充落实 [RFC-0009](0009-company-string-and-slot-map-struct-binding.md)
和 [RFC-0036](0036-whisper-asr-backend.md) 已有语义；不额外定义“空音频必须有某个意图”。
ASR 的空文本与来源保持由模型契约验证，后续业务节点仍按自己的配置处理空文本。

### 2.6 C07：Node 失败诊断与执行模式无关

**现状位置：**[NodeBase](../../include/nodes/node_base.h)、
[Pipeline::Execute](../../src/core/pipeline.cpp)、
[AlgContext](../../include/core/alg_context.h)。

1. 修正 TextChunk 的非法 UTF-8、TextRuleMatch 的 regex 执行失败、TextRerank 的缺输入等
   分支，统一调用已有 `Fail/Require`。保留各节点原有错误码和语义，不要求重编号。
2. 明确节点责任：成功返回 0；失败返回非零，并在正常可分配条件下记录同码诊断。
   `Process(nullptr)` 仍返回无效上下文错误，不尝试写空对象。成功分支不得清除其他并行节点的错误。
3. Pipeline 中提取共用的单节点调用包装，由顺序、单节点层、并行任务三条路径共同调用。
   使用现有**当前线程错误快照**收集该次调用的诊断，返回码作为执行失败的权威依据。
   返回非零但缺少诊断时，生成包含节点身份的兜底信息；有诊断时保留原语义。
4. 不能用请求共享的 `GetErrorMessage()` 判断当前节点是否已经报错：同层其他线程可能
   正在写入。进入/离开一次调用时清理或取走本线程记录，避免复用 worker 的旧诊断串入新节点。
5. 包装对直接实现 `INode` 的测试/扩展节点也提供标准及未知异常保护，沿用 Core 已有
   执行异常失败码；Core 不包含 `nodes/node_error_codes.h` 或具体 Node 类型。
6. 并行情况下收集所有已提交任务，按确定的节点顺序选首个失败后设置最终请求状态。
   顺序执行在首次失败时停止。两种模式在执行同一失败节点时返回码和错误归属一致；
   不承诺二者具有相同副作用数量，也不引入并行失败的事务回滚。

资源耗尽时错误文字允许降级，遵守 §2.3；不能把“任何情况下都构造详细诊断”作为验收要求。
本次不为所有多输出 Node 引入事务黑板，保留请求失败后输出不可作为成功结果消费的规则。

### 2.7 C08：明确 TextChunk 的父项计数与子项编号

**现状位置：**[text_chunk_node.cpp](../../src/common_nodes/text_chunk_node.cpp)、
[来源类型](../../include/contracts/traceable_item.h)。

采用以下规则，不要求业务自定义更深的层级来源结构：

- `chunk_counts` 对每条输入产生一条计数，完整保留输入 `(req_id, sub_id)`，兑现 `1:1/preserve`。
- `chunks` 仍为 `1:N/generate_sub_id`。在一次 Process 中，为每个 req_id 维护独立的递增
  子编号，按输入顺序与切片顺序从 0 连续分配；不能对同 req 的每条输入重置计数器。
- 生成后的 sub_id 是该请求内切片索引，不等于父项 sub_id；保留当前扁平来源模型，
  不声称它能够独立还原任意多级父子关系。不同 req_id 的子编号可以相同。
- 相同输入 `(req_id, sub_id)` 重复出现时失败；唯一性校验、UTF-8 解析和编号溢出检查
  在发布结果前完成。拒绝计数或子编号超出对应整数类型的情况。
- 空文本仍产生一个空切片，计数为 1；合法单父项且父 sub_id 为 0 的现有路径结果不变。

例如 `chunk_size=2`、输入 `(7,4,"abc"),(7,5,"de")`，结果必须为：

```text
chunks       = (7,0,"ab"), (7,1,"c"), (7,2,"de")
chunk_counts = (7,4,2), (7,5,1)
```

不为支持多父项而放宽 Validator 的全部 `1:N → 1:1` 连接；节点的防御性运行能力
和图的静态兼容声明分别验证。真实方案需要多级分块语义时另行设计，而非隐含改变端口规则。

### 2.8 C09：统一 Embedding 的最终数值保证

**现状位置：**[BGE Embedding](../../src/engine/models/bge_embedding/bge_embedding_model.cpp)、
[Generated Embedding](../../src/engine/models/generated_text_embedding/generated_text_embedding_model.cpp)、
[IEmbeddingModel](../../include/engine/model_interface.h)。

Model Execution 内提取由两者共同使用的稳定数值辅助函数，建议放在
`src/engine/models/common/embedding_numeric_support.*`（拟新增共享目录），
不得放入其中一个消费模型的专属目录，也不让 Node 重复做归一化。

约定成功输出为非空维度、全有限数值，并在 normalize=true 时具有有效单位方向：

1. mean 池化用 double 累加，并在除以有效计数后再转换；有效元素筛选沿用各模型原有
   mask/生成 token 语义，不把 BGE 与 Generated 的预处理强行合并。
2. L2 计算采用 double 或稳定缩放实现，检查中间和最终有限性。不能先用 float 累加平方
   再把已经溢出的结果转换为 double。
3. `normalize=false`：允许有限零向量，但不允许成功返回 Inf/NaN。
4. `normalize=true`：范数为零或非有限时返回失败；不把零向量作为合法单位向量，
   不增加模型各自不同的魔法阈值。有限非零小向量使用稳定算法正常处理。
5. 转回 float 后再检查输出有限性。维度/shape 校验仍由具体 Model 承担，
   保留 `FixedBatchExecutor::Execute` 的 padding 移除、来源和失败输出清空语义。
6. `EmbeddingOptions.normalize` 仍是唯一归一化选择，继续落实 RFC-0043，
   不重新添加 BGE 模型配置中的旧 normalize 字段。

两种模型生成的向量空间不同，不能拿它们的真实业务 embedding 做逐项相等断言。
共同测试仅用可控中性协议输出验证数值保证；生产效果变化另用真实模型验收。

### 2.9 C10 / C11：共用 Schema 原语，保留 Init 防御边界

**现状位置：**[配置字段定义校验](../../include/contracts/config_schema_validation.h)、
[Node 注册](../../src/core/pipeline_catalog.cpp)、
[Validator](../../src/core/pipeline_validator.cpp)、
[Node 初始化上下文](../../include/core/node_interface.h)。

**Definition 校验：**删除 Catalog 内部重复的字段 Schema 判断，Node/Model/Backend
注册统一调用 `ValidateConfigFieldDefinitions`，包括非法 kind、重复字段、非有限上下界、
默认值类型/范围/枚举等检查。Node/Biz 的端口 Definition 也复用同一合法值检查，
Biz 注册不能只验证名称而允许空 key/type、重复端口或非法元数据。
保留注册冲突的 fail-closed 行为与原子性，不新增另一套 Catalog。

**配置值与直接 Init：**保留现有 `NodeInitContext`，不为本次整改删除直接初始化接口。
严格 Pipeline 入口仍先预检；直接初始化也必须具备文档约定的防御校验：

1. 将现有 `ValidateAndNormalizeConfig` 中无图语义的字段值检查/默认值注入抽成
   `contracts` 纯函数。结果仅包含字段、失败种类和说明；Orchestration 保留现有公共包装，
   将它映射为稳定 DiagnosticCode、JSON Pointer 和 suggestions。
2. 每个生产 Node 的字段只声明一份。普通 Node 用局部字段构造函数供
   `Make...Definition` 和 `InitNode` 共用；模型绑定节点复用 `ModelBoundNode` 已有的
   Definition 读取，在模型绑定前完成字段值校验，并将归一化配置传给 `InitModelNode`。
   初始化按现有类型读取归一化结果，不在多个继承层重复做同一字段检查。
   不向 NodeBase 新增 Catalog/Validator 查询，也不复制图规划。
3. 已有跨字段语义规则，例如 overlap < chunk_size、模板参数组合，继续使用 Node
   局部的纯解析/验证函数，同时供 Definition 回调和 Init 使用。
4. 值校验先完成，语义回调才执行；Node 和 Backend 回调对标准及未知异常都返回
   `INVALID_COMBINATION` 等一致语义诊断，不允许异常使 CLI 退出。
   精确类型/范围错误继续由字段原语生成，不能把所有错误笼统合并为一种类型错误。
5. JSON 的整数类型检查在 `get<int>()` 前执行；浮点 `2.5` 不能被直接 Init 截成整数 2。
   超范围有符号/无符号整数、C++ 内存 JSON 中的非有限数值同样拒绝。
6. 只统一纯配置合法性。模型注册存在性、必需输入的图生产者和业务出口闭合仍由
   Validator 负责；直接 Init 不被宣称等价于完整 Pipeline 验证。

覆盖全部生产 Node，而不只修复当前例子。至少固定以下拒绝结果：
`VectorTopKNode(top_k=-1, metric="typo")`、`TextCorpusSourceNode(corpus="typo")`、
`TextRuleMatchNode(rules="typo")`、`TextChunkNode(chunk_size=2.5)`、非法 TextRerank top_k。
测试替身未注册 Definition 时可继续直接 Init；公共原语的使用由生产实现和作者模板保证。

### 2.10 C11：删除未接入的 Adapter 字段预检接口

**采用决策：移除内部 `IBizAdapter::ValidateInput`，明确 `Unpack` 负责字段校验与转换。**

当前 7 个内建 Adapter 都不覆盖该接口，SharedRuntime 不调用；保留空实现会误导扩展作者。
直接把它接入运行时但仍返回默认成功无法解决问题；强制增加第二次完整扫描也没有收益。

统一生命周期为 `ValidateBatch → Unpack → Pipeline::Execute → Pack/PackResultBatch`：

- `ValidateBatch` 检查批次 envelope 与输出槽位容量。
- `Unpack` 无状态地验证字段并 CopyIn，不成功则该请求上下文不能进入 Pipeline。
- C 与 Operator 表示需要的校验通过 §2.5 的共享约束复用，不暴露“看似完整”的空预检接口。
- 迁移内部调用、示例、作者文档和相关测试；若工作区存在自定义 override，将其逻辑移入
  `Unpack` 或由 `Unpack` 调用的局部函数。禁止删除校验逻辑后仅为了编译通过移除 override。
- 此举是内部 C++ 扩展接口源码/虚表变更，消费者需要重编译；六个 C 导出函数和公开 C
  结构布局不变。不增加长期兼容空壳，也不让旧二进制 Adapter 与新库混用。

此决策仅取代此前文档中将独立 `ValidateInput` 列为字段验证生命周期步骤的部分，
保持 Adapter 校验、CopyIn、无状态和输出所有权等既有契约。

## 3. 兼容与迁移

### 3.1 尚未投产阶段采用的策略

错误结果、错误输入和未使用的内部接口不建立兼容分支。
影响合法数据的边界语义在一次原子变更中同步代码、Definition、示例和测试；
不增加 `legacy_*` 开关，也不通过放宽 Validator 来保留原缺陷。

| 变化 | 受影响消费者 | 必须执行的迁移 |
| --- | --- | --- |
| 缓存键内部格式 | session 缓存 | 重建句柄自然重建缓存；不存在磁盘缓存迁移 |
| 业务 egress 更严格 | 配置作者、Catalog/Studio 消费方 | 原生校验全部方案；普通出口不能依赖一对多结果；ranked 消费声明与 Adapter 同步更正 |
| channel 上限、零 PCM、ID 范围 | C / Operator 调用方、部署配置 | 超长渠道与非法 ID 明确失败；允许零 PCM；保留显式更严格的部署限制 |
| Node 错误诊断 | 日志/错误消费者 | 原节点错误码尽量保留；错误文字不再出现失败却为 OK，不承诺旧错误文本 |
| TextChunk 来源 | 自定义组合、单节点调用 | counts 使用原 sub_id；切片按 req 内顺序唯一编号；禁止依赖旧重复编号 |
| Embedding 零范数 | Model 调用方、测试替身 | normalize=true 的零向量改为失败；测试成功替身应产生非零有限向量 |
| 直接 Init 校验 | 单节点测试、脚手架、自定义 Node 作者 | 非法配置不再通过截断/静默忽略；使用注册 Definition 对照默认值与边界 |
| 删除 ValidateInput | 内部 C++ Adapter 扩展 | 校验迁入 Unpack、移除失实说明、全部重新编译 |

### 3.2 配置与文档更新位置

- 由实际注册 Definition 更新 Catalog；不手改 Studio、Skill 内的能力列表。
- 校验 `configs/`、`demo/fixtures/mock/` 和受影响 tests fixtures；使用测试注册的方案
  必须由 `alg_pipeline_tool_test` 验证，生产方案由生产工具验证。
- 当前构建缺少可选 Backend 的方案单独记录，不能用旧 build-kite/build-whisper 二进制
  的成功结果替代当前源码验证；可选 Backend 验证使用当前源码独立构建。
- 实施完成后更新 [开发者指南](../developer_guide.md)、
  [业务接入指南](../dev_guide/business_onboarding.md)、
  [自定义 Node 指南](../../src/custom_nodes/README.md) 及受影响脚手架。
  保持指南和技能中“Init 有防御校验”的要求与实际共用原语一致。
- 实际行为修改时才更新 [CHANGELOG](../CHANGELOG.md)。本 RFC 提案不把未来修复写成已交付。
  历史 RFC 和验收记录保留原貌，通过本 RFC 说明补充/取代范围。

### 3.3 回退边界

每个实施阶段保持代码、Definition、配置与测试为一个可审查单元。若需要回退，回退该
单元并重建句柄/内部扩展，不单独回退出口 Definition 或 Schema 检查来掩盖测试失败。
本 RFC 不涉及模型文件、持久业务数据或外部 SDK 的迁移，也不授权远程提交、PR 或合并。

## 4. 验证与完成条件

### 4.1 最小验收矩阵

以下项目为实施完成条件，最终证据见 §5.3。优先扩展已有责任套件，不要求新增测试可执行文件。
测试必须断言行为，不以扫描“是否出现某函数调用”替代运行验证。

| ID | 放入现有测试位置 | 必须观察到的结果 |
| --- | --- | --- |
| C01 | `tests/unit/nodes/test_text_embedding_node.cpp`、`tests/unit/core/test_node_ownership_and_reuse.cpp` | 两组上述碰撞语料返回各自正确向量，模型调用两次；同键并发只创建一次；空串/NUL/顺序/ID/normalize/revision 变化可区分；失败不缓存且可重试 |
| C01 组合 | `tests/integration/pipeline/test_pipeline_catalog_validator.cpp` 或上述已有 Node 复用套件 | 两个真实 Corpus Node + session Embedding，经真实 ValidatedPlan 初始化；可计数模型替身验证结果。禁止仅测试新编码函数自身 |
| C02 | `tests/unit/engine/test_engine_fault_tolerance_and_lifecycle.cpp`、已有 Backend suites、`tests/contract/abi/test_c_abi_safety.cpp`、`tests/unit/operator/test_operator_value_registry.cpp` | 分别在参数/路径诊断、catch 诊断、输出与清理处注入失败；入口正常返回错误/空 session、不 terminate、不发布成功输出；标准与非标准异常均被拦截 |
| C03 | `tests/integration/pipeline/test_pipeline_catalog_validator.cpp`、`tests/unit/core/test_validated_pipeline_plan.cpp`、`tests/contract/catalog/test_catalog_contract_ssot.cpp` | TextChunk chunks → doc_qa llm_answers 在加载前报 PORT_CARDINALITY_MISMATCH；Node 与 egress 的不兼容规则同判；可选出口语义、类型/生命周期诊断、非法 Biz 端口注册有覆盖 |
| C03 合法组合 | `tests/integration/runtime/test_all_biz_pipelines.cpp`、`tests/integration/pipeline/test_doc_qa_rerank.cpp` | 多排名结果通过新预检；CrossRerank 按请求保留完整排名数组，Compliance 选择首项；普通一对一结果与 request ID 保持；运行时仍拒绝动态来源错误 |
| C04/C05 | `tests/contract/abi/test_adapter_contract_security.cpp`、`tests/integration/operator/test_operator_api.cpp`、`tests/integration/runtime/test_different_io_modalities.cpp` | 两种入口对 256/257 字节、可选空渠道、零 PCM、非法采样率、正长度 null buffer 同判；单独验证 Operator 更严格显式限制；C ABI / Operator 的 ASR 路径均不错误拒绝空样本 |
| C06 | `tests/integration/operator/test_operator_api.cpp` 或已有配置解析责任用例 | 超 int32 正/负/无符号值拒绝，合法 ID 保持原值；meta_num/ID 不匹配仍失败，原始值不能经截断通过白名单 |
| C07 | `tests/unit/nodes/test_text_chunk_node.cpp`、规则/精排 suites、`tests/unit/core/test_dag_pipeline.cpp`、`tests/unit/core/test_node_base_contracts.cpp` | 非零返回与上下文错误码一致；顺序、单节点层、并行多节点层同一失败归属一致；多个节点不同错误不串线；已提交任务结束后才返回 |
| C08 | `tests/unit/nodes/test_text_chunk_node.cpp` | 非零父 sub_id 的 counts 保留；同 req 多父项切片不重号；不同 req 隔离、重复输入来源拒绝、空串/Unicode/overlap 保持；溢出检查可在小型计数辅助处模拟 |
| C09 | `tests/unit/engine/test_onnx_and_embedding_model.cpp`、`tests/unit/engine/test_model_backend_decoupling.cpp` | 两种模型在零向量、1e20、FLT_MAX、非有限输入和正常值下遵守同一数值规则；覆盖 BGE 2D/3D、mean/CLS；失败输出清空、正常来源保持；单位范数用浮点容差断言 |
| C10 | `tests/unit/core/test_definition_schema_validation.cpp`、`tests/contract/catalog/test_registry_conflict.cpp`、`tests/contract/catalog/test_model_backend_registry_conflict.cpp` | 同一非法 Schema 在三类注册均拒绝；值校验失败不调用语义回调；Node/Backend 回调抛 runtime_error 和整数均生成诊断；CLI/Build 同为失败 |
| C11 Node | 12 个 Node 的既有 suites、`tests/tooling/test_scaffold_custom_node.py` | 配置声明默认值与直接 Init 一致；上述错误配置、未知字段、错型/范围均拒绝；跨字段规则两入口一致；不把直接 Init 当成完整图验证 |
| C11 Adapter | `tests/unit/adapter/test_adapter_purity.cpp`、Adapter 安全和 Operator 既有 suites | 无残留无效 ValidateInput 调用/override；Unpack 仍拦截非法字段；失败时不执行 Pipeline；同一 Adapter 并发无请求状态泄漏 |

OOM 测试复用 [ScopedAllocationFailure](../../tests/support/scoped_allocation_failure.h)，
注入区间覆盖被测调用，必要时沿用仓库已有测试方式隔离进程终止；测试成功标准为正常返回，
不能把“触发死亡测试”作为修复后的预期。测试本身输出断言不应落在持续分配失败区间。
中性协议替身用于证明确定性契约，不需要下载权重或接入厂商 SDK。

### 4.2 方案执行与门禁

对实际受影响方案执行 `catalog`、`validate`、`plan`；再通过已有 Demo/ABI/Operator
集成用例执行并检查业务字段、来源与状态，不能只证明 JSON 能解析。
重点保留 doc_qa、cross_rerank、compliance、audio_asr 以及 custom Node 方案的运行证据。
Demo 使用同一被验证的配置，避免默认 Control 改写测试参数，遵循 RFC-0043 的既有方式。

各阶段开发时运行责任范围内的最小测试；提交一个完整实施阶段进行交付时，遵循
[CONTRIBUTING](../../CONTRIBUTING.md#6-run-one-canonical-delivery-gate)
执行一次 `./scripts/run_all_tests.sh`，不重复完整门禁。
维护以下证据等级，不能相互替代：

- 默认构建、所有注册 CTest、Schema/Catalog、双入口与中性协议测试。
- 当前源码的可选 Backend 构建/异常契约验证；未启用的 Backend 明确列出未验证范围。
- 有授权和资产时的真实模型 smoke/effects；BGE 数值修复不能仅凭 mock 宣称检索效果改善。
- 公司内部 SDK 和目标硬件验收，继续归 RFC-0029，不作为外部 workspace 可完成事项。

本 RFC 的源码契约完成要求：所有 A/B 项及相关默认门禁通过，可选实现的改动完成当前源码
能执行的构建与契约验证，明确保留真实资产和内部硬件限制。未完成项不能只写成“后续优化”
然后标记 Completed；本 RFC 完成也不等同于整个平台已获生产验收。

## 5. 实施与最终结果

### 5.1 推荐实施顺序

阶段名称是建议的本地提交单元，不是自动创建 PR 或远程交付授权。
每阶段先加入能复现旧行为的测试，再修改实现并核准受影响契约。

| 阶段 | 范围 | 前置与执行方向 | 阶段完成条件 |
| --- | --- | --- | --- |
| S1 | C01、C02 | 独立修复缓存身份与不可抛边界；保留每项独立可审查 diff，不等待配置整理 | 缓存碰撞复现变为正确结果；分配失败不 terminate；原正常调用保持 |
| S2 | C10、C11 Node | 先合并 Definition 校验，再提取纯字段值原语，迁移生产 Node 的 Init，最后统一回调异常映射 | 同 Schema/配置跨入口一致；不引入 Node → Validator 运行依赖；脚手架示例可用 |
| S3 | C03—C06、C11 Adapter | 在同一阶段核准 egress 消费语义、共享输入限制、安全转换、删除失实接口；ranked 声明与 Validator 原子变更 | 所有合法业务方案继续通过；错误组合前置拒绝；双入口边界一致；内部扩展重编译 |
| S4 | C07—C09 | Node Fail 分支与 Core 包装一起修复；独立修复 Chunk 来源和模型数值辅助 | 同一错误跨执行模式一致；来源唯一且计数可回溯；两种模型数值保证一致 |
| S5 | 全范围 | 串联 §4 业务运行证据，复核配置、指南、技能/脚手架、CHANGELOG，记录资产跳过项 | 清单全部勾选、默认门禁通过、实际行为与文档一致，才进入最终实施完成评审 |

S1 两项、S3 的纯输入修复、S4 的数值修复可以在不同隔离分支独立开发；
同一个文件或共享契约的改动按依赖顺序整合，不同时维护两套临时兼容行为。
预计改动量以责任文件与测试范围评估，不用固定天数替代完成条件。

### 5.2 实施检查表

- [x] C01 完整缓存身份与组合/并发/失败重试回归。
- [x] C02 受影响入口异常保护与分配失败验证。
- [x] C03 egress 流契约、ranked 声明与合法业务回归。
- [x] C04—C06 双入口字段边界和安全整数转换。
- [x] C07 节点失败诊断与统一执行包装。
- [x] C08 分块来源和计数契约。
- [x] C09 Embedding 数值/归一化契约。
- [x] C10 Definition/字段值/语义回调共用校验。
- [x] C11 Node 防御性 Init 与 Adapter 生命周期清理。
- [x] 受影响生产/测试方案的 validate、plan 与实际业务结果验证。
- [x] 作者文档、脚手架、实际变更的 CHANGELOG 同步。
- [x] 记录最终源码基线、门禁结果、可选构建证据与明确的未验证范围。

### 5.3 实施复核与验收结果（2026-09-08）

**结论：C01–C11 的修改必要且与既有分层相符；补修后完成本 RFC 的源码契约范围。**
实施基于提案提交 `77282bd` 和 §1 的源码基线，代码、测试及本验收记录在关联分支一同交付；
最终提交身份以 Git 历史为准。

复核保留了完整缓存身份、共享字段校验、统一执行包装和模型数值辅助函数。
它们各自消除已有重复规则或已复现错误；没有新增 Node、Model、Backend、依赖、配置语言、
继承体系或测试可执行文件。Backend 的较大 diff 主要来自完整 try/catch 覆盖及缩进。
同键缓存内存、业务错误码、模型能力差异和 Adapter 输出策略仍按 §1.3 的范围处理。

对初版实施直接补修了以下缺口：

| 缺口 | 最终处理与回归依据 |
| --- | --- |
| cache 长度先窄化到 uint32；组合回归仅直接调用 Embedding | 版本化小端 uint64 编码完整长度；真实 Corpus Node 经严格 Plan 初始化，覆盖碰撞文本、NUL、空串、Unicode、来源和选项变化，保留并发复用及失败重试测试 |
| catch 中先拼接临时字符串；输出池/运行时/注册失败的保护未完整覆盖 | 共用 `SetDiagnosticNoexcept(string_view)`，诊断赋值内部捕获；完善所属边界。分配注入限定在被测调用内，断言移到注入区间之外 |
| 可选 egress 类型错误被放过；Node 与 egress 重复流检查 | 共用 `ValidatePortFlowContract`，可选出口存在时检查类型；错误路径指向实际生产端绑定，保留 `$egress` 和既有诊断码 |
| 双入口限制仍散落为字面量；Operator PCM 未检查显式字节上限 | Integration 共用 `biz_input_constraints.h`；渠道 256/257、空渠道、PCM 长度/指针/采样率及更严格字节上限有对照回归；零 PCM 经两种完整入口成功执行 |
| signed integer 范围检查经过 double，空端口流元数据被接受 | signed/unsigned 原始整数分别转换比较；测试 2^53 附近的边界，Node 与 Biz 拒绝空 cardinality/provenance/lifetime |
| chunk counts 可发生有符号溢出，sub_id 最后一个合法值被提前排除 | int32 计数在递增前检查；uint64 中间计数在发布 uint32 sub_id 前检查；counts 保留父项来源；Definition 与 Init 共用字段列表 |
| 汇总并行失败时再次抛异常可提前离开，仍有任务访问请求 | 局部等待保护覆盖异常退出；测试诊断阶段抛出 bad_alloc 时，阻塞任务结束前 Execute 不返回；顺序、单节点并行和多节点失败归属均有回归 |
| 数值测试的新 suite 未进入 CTest 注册过滤器，仅测辅助函数 | 归入已注册 `OnnxAndEmbeddingModelTest`；通过真实模型类和中性 session 替身覆盖 BGE 2D/3D、CLS/mean、generated mean/last 的大数、零和非有限值，以及输出清理/来源/固定批次填充移除 |
| 变更记录含“64-bit 内存对齐”等失实表述，迁移说明缺失 | 删除无依据的保证，更新现行开发指南、技能参考与可编译 starter 的校验说明，明确 Unpack 生命周期及内部扩展重编译要求 |

关键回归位置：
[缓存组合](../../tests/unit/nodes/test_text_embedding_node.cpp)、
[分块来源](../../tests/unit/nodes/test_text_chunk_node.cpp)、
[字段与定义](../../tests/unit/core/test_definition_schema_validation.cpp)、
[出口规划](../../tests/unit/core/test_validated_pipeline_plan.cpp)、
[执行失败](../../tests/unit/core/test_dag_pipeline.cpp)、
[双入口/分配失败](../../tests/unit/operator/test_operator_value_registry.cpp)、
[BGE 数值](../../tests/unit/engine/test_onnx_and_embedding_model.cpp)、
[generated 数值/Backend 分配失败](../../tests/unit/engine/test_model_backend_decoupling.cpp)。
溢出分支采用源码边界复核，不声称实际分配了数十亿个切片进行压力验收。

验证记录：

- `./scripts/run_all_tests.sh`：89/89 CTest 通过，包括格式、分层、C11 ABI、所有默认构建、
  集成、Demo、脚手架和文档工具检查。新增用例加入既有 suite；核心 runner 复用既有
  allocation-failure 测试对象，没有增加测试可执行文件。
- Catalog：默认构建实际注册 12 Node、6 Model、7 biz、2 Backend；没有增加生产能力。
  使用各自匹配的生产/测试 Catalog 工具，对 24 份 Pipeline 执行 validate 和 plan，全部通过：
  默认/测试构建 17 份、Kite 构建 6 份、Whisper 构建 1 份。默认构建对未启用 Backend 的
  拒绝符合约束，未用测试注册覆盖生产配置。
- 从 doc_qa 组合构造 TextChunk chunks → llm_answers 的反例；生产 CLI 的 validate/plan
  均在 `/pipeline/0/ports/outputs/chunks` 报 `PORT_CARDINALITY_MISMATCH`，关联 `$egress`。
- 关闭默认 Control 后执行 `doc_qa_mock`、`doc_qa_rerank_mock`、`dialogue_audit_mock`、
  `audio_asr_mock`、`entity_extract_custom_mock`、`doc_qa_custom_mock`、`cross_rerank_onnx`
  七个现有 Profile。11 条结果全部成功，核对请求 ID、状态及回答/计数/质检/意图/实体字段；
  CrossRerank 保留全部三个候选排名。此项记录路径和字段正确性，不将 fixture 当成效果评估。
- 当前源码的 Kite 构建（ONNX + Kite，llama/Whisper 关闭）完成 core runner 和生产工具构建；
  DAG、ModelBackendDecoupling、ONNX/Embedding、LlamaCppBackend 四个相关 suite 通过。
  当前源码的 Whisper 构建（ONNX + llama + Whisper，Kite 关闭）完成相同目标构建，
  DAG、ModelBackendDecoupling、ONNX/Embedding、WhisperCppBackend 四个 suite 通过。
  两组开关遵守仓库 GGML 版本隔离约束。
- 使用本地已有 `qwen2.5-0.5b-instruct-q4_k_m.gguf` 额外执行并通过
  `RealGgufLoadAndTextGeneration`、`RealKiteSdkGenerationAndFixedSeedPolicy`、
  `RealKiteGeneratedTokenEmbeddings` 三项真实资产回归。没有引入或提交模型资产。

默认门禁仍有 8 个因可选 Backend / 资产开关而跳过的内部测试；上面的补充运行仅覆盖已列出的
对应项目，不把全部跳过项视作通过。Kite 视觉、所有真实业务效果、缓存负载/内存预算、
公司内部 SDK 及目标硬件生产验收没有在本次完成。它们按既有范围管理，本 RFC 不替代 RFC-0029。

现行编写规则见[开发指南](../developer_guide.md)及
[Node 概念说明](../dev_guide/custom_node_concepts.md)；远程 PR 和合并 CI 状态由 Git/GitHub
记录，用户本次额外授权的交付继续执行仓库标准 PR 流程。
