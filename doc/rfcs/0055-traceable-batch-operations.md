# RFC 0055：批次关联、分组、选择回填与拆分公共工具

- **RFC 编号**：0055-traceable-batch-operations
- **创建日期**：2026-09-14
- **文档状态**：Completed
- **关联分支**：`refactor/traceable-batch-operations`
- **目标版本**：下一次投产前开发接口版本；保持 Catalog v3
- **负责人 / 作者**：LLM-EdgeFlow contributors
- **设计基线**：`3fb4ba5be18f00fd8855b7d2de900e80ad203335`
- **关联决策**：补充 RFC-0012、0022、0039、0052；实现 RFC-0052 第 8.3/10 节的一部分局部来源工具，保留独立 Filter/Group Node 作者策略的延期边界。

本文规格已完成实施，新增公共工具位于 `include/nodes/traceable_batch_operations.h`。
与 [RFC-0053](0053-function-oriented-adapter-authoring.md)、
[RFC-0054](0054-controlled-configuration-snapshots.md) 独立。
开发与交付流程遵循 [CONTRIBUTING](../../CONTRIBUTING.md)。

## 1. 问题、范围与现有基础

RFC-0052 已让一对一 Map、自由 Batch、模型调用与参数绑定使用普通函数。业务一旦需要多输入
关联、按请求分组或只对部分项再次推理，作者仍需手工建立来源索引并确保结果回填正确。

| 现有实现 | 已有机制或剩余工作 |
| --- | --- |
| [traceable_algorithms.h](../../include/nodes/traceable_algorithms.h) | MapPayloads 已保存编号，支持普通值及 NodeResult；本篇不重做 |
| [traceable_batch_validation.h](../../include/nodes/traceable_batch_validation.h) | 已有等长逐项保序检查；不将它当作唯一编号或任意 Join 证明 |
| [TextTemplateNode](../../src/common_nodes/text_template_node.cpp) | 手工收集样本键、按请求组织 context/matches/document |
| [PromptGuidedLlmNode](../../src/custom_nodes/prompt_guided_llm_node.cpp) | 按 request 关联参考内容；不能按两个数组的位置 zip |
| [VectorTopKNode](../../src/common_nodes/vector_top_k_node.cpp) | 自建候选文本索引和请求分组；还含 shared 候选等业务语义 |
| [TextChunkNode](../../src/common_nodes/text_chunk_node.cpp) | 手工拒绝重复 key、分配子编号、处理溢出、生成父项计数 |

### 1.1 必需交付

1. `JoinByItem`：按完整来源 key 的精确/左关联视图。
2. `GroupByRequest`：显式 anchor 决定请求集合的分组视图。
3. `SelectBatch` 与 `ScatterReplace`：选择子批次、检查模型结果并恢复完整批次。
4. `SplitPayloads`：载荷拆分、按请求分配子编号和父项计数。
5. 编译示例、独立行为测试，以及 TextChunk 拆分试点迁移。

这些是普通 C++ 函数/视图，不注册 Node、不读取 Context、不调度模型、不发布 Blackboard。
算法仍自由使用 if/for 和已有 typed model calls；公共工具不默认重试、降级、排序或补答案。

### 1.2 明确的首期边界

本篇不新增独立 Filter/Join/Group/Split Node，也不新增 Filter 输出策略、Catalog 数量枚举、
父 lineage、子 Pipeline、通用多输出事务或完整业务重排算法。
局部选择后通过 Scatter 恢复完整批次，继续满足 RFC-0052 的 PreservedOutput。
TextChunk 仍使用高级 Node 发布既有两个独立端口。

原因是“局部函数过滤一些项”与“Pipeline 向下游传递不完整请求集合”具有不同契约。
本篇完成普通工具即可结项，不以另一次 Catalog/Validator 设计为隐藏前置条件。

## 2. 所属层、类型与统一约定

新增运行时工具属于能力节点层 / Capability Nodes，建议放入
`include/nodes/traceable_batch_operations.h`，与现有 `traceable_algorithms.h` 并列；
必要非模板实现放 `src/common_nodes/support/`。从 `nodes/authoring.h` 提供稳定入口。
Core、Model Execution、Integration 不反向包含该工具。本篇不迁移 Adapter 的 IndexResults，
也不把 NodeResult 引入 contracts。

### 2.1 key 与错误

沿用 [TraceableItem](../../include/contracts/traceable_item.h) 的 `uint32_t req_id/sub_id`。
完整 key 为二元组，不将 vector 下标当业务身份，也不将外部 request ID 当批内 req_id。
工具可以构造内部哈希索引，但不会改变调用者输入顺序。

所有操作返回 `NodeResult<T>`。成功空结果是合法值；失败不返回可被当成有效结果的部分值。
在 `nodes/node_result.h` 为 NodeFailure 增加可选 `BatchFailureDetail`，包含操作名、reason
枚举和可选的完整来源 key。reason 至少区分 duplicate、missing、unknown、count_mismatch、
sub_id_overflow、count_overflow、callback_failed。原构造函数保持可用，其他失败无此 detail。
普通函数式作者沿用既有 author-node 数值错误路径，诊断包含可读 key；不要求作者自行分配
错误码，不把失败立即写入 AlgContext。高级迁移节点可按结构化 reason 映射历史错误码，
禁止从 message 字符串反推原因。callback failure 保留原 cause_code/内容，并补当前父项来源。

不改变 MapPayloads 或已有模型包装的重复 key 行为。只有调用本篇严格工具时才施加其唯一性
契约；不能以迁移 helper 为由默默扩大整个框架的输入拒绝范围。

### 2.2 视图与所有权

Join/Group/Selection 视图借用 `const` 输入批次，只持有索引和只读引用；输入必须在视图使用
期间存活，且不能修改、移动或使 vector 元素地址失效。拒绝从临时/rvalue batch 构造借用视图。
来自 AlgContext 的不可变批次可安全用于一次 Run 内的这些视图。

视图不能进入 Session、Node 成员或异步任务。`Materialize` 显式创建 owned 子批次供模型使用；
Scatter/Split 返回 owned 输出。首版允许为保持失败安全复制完整输出批次，不做复杂的原位更新
或隐藏共享写时复制；在代表性批次上记录成本。

以下签名只规定作者可观察的形状；实现可用有限模板表达载荷类型，不增加运行时反射：

```cpp
JoinByItem(left, right, JoinMode::kExact);   // -> NodeResult<ItemJoinView<...>>
GroupByRequest(anchor, members);            // -> NodeResult<RequestGroupView<...>>
SelectBatch(anchor, predicate);             // -> NodeResult<Selection<...>>
ScatterReplace(selection, replacements);    // -> NodeResult<same-type full batch>
SplitPayloads(input, split_one);            // -> NodeResult<SplitResult<...>>
```

## 3. JoinByItem：完整 key 关联

左批次是输出顺序的 anchor。两侧均要求完整 key 唯一；重复时整体失败。

| 模式 | 右侧缺少左侧 key | 右侧出现左侧没有的 key | 行顺序 |
| --- | --- | --- | --- |
| kExact | 失败 | 失败 | 严格按左批次顺序 |
| kLeft | 行内 right 指针为空 | 失败 | 严格按左批次顺序 |

首版不提供丢弃右侧多余项的开关；调用者需要子集时先显式选择，避免把接错输入静默吞掉。
两侧均为空时成功；左空右非空在两种模式下都失败。左非空右空只在 kLeft 成功。

返回行含左项 const 引用和右项 const 指针。框架不填默认载荷，不按相同 req_id 自动做
一对多关联，也不按输入数组的第 i 项配对。
预期时间 O(L+R)、额外索引空间 O(L+R)；实际输出按左侧遍历，不能迭代 unordered_map 输出。

## 4. GroupByRequest：anchor 决定完整请求集合

anchor 是明确的 TraceableBatch；允许同一 req_id 有多个不同 sub_id，但完整 key 不得重复。
按 anchor 首次出现的 req_id 顺序建立请求组，每组同时提供该请求的 anchor 项视图和 members
项视图。members 的完整 key 同样必须唯一。

1. members 中没有对应 anchor req_id 的项整体失败。
2. anchor 中没有 members 的请求保留空组，不因空组丢掉请求。
3. 每组 members 按原 members 批次的相对顺序排列，不按 sub_id 自动排序。
4. 空 anchor/空 members 成功；空 anchor/非空 members 失败。
5. 工具只返回组视图，不自动产生输出 TraceableItem，也不猜测聚合输出的 sub_id。

聚合算法可在普通函数中选择输出载荷及来源规则；需要每个原输入一个输出时，按原 anchor
顺序逐项查询其请求组，或按原 anchor 位置回填，保留完整 key。不能直接按组遍历生成一对一
输出：anchor 为 A0、B0、A1 时，输出必须仍是 A0、B0、A1，不能变成 A0、A1、B0。
组视图须支持按 req_id 查询或保留原 anchor 位置；组内 anchor 本身也保持输入相对顺序。
需要每请求一项时使用已有高级 Node 的显式 N:1 契约。
`req_id=0` 不在本工具中被解释为共享语料；shared candidates 是业务显式策略。
预期 O(A+M) 时间和空间。

## 5. SelectBatch 与 ScatterReplace：局部推理后恢复全量

### 5.1 选择

`SelectBatch` 首先拒绝 anchor 的重复完整 key，再逐项调用只接收载荷的 predicate。
predicate 首版支持 bool 和 `NodeResult<bool>`；错误/异常沿用普通 Node 失败路径。
选中项保持原 key 和相对顺序。全选、全不选、空 anchor 都有明确定义。

`Selection` 由工厂构造，私有保存借用 anchor 及选中位置；调用者不能任意填充位置表。
它提供只读选中项遍历和显式 `Materialize()`，后者复制选中项为同类型 owned 批次。
原位置只用于恢复顺序，结果关联仍验证完整来源 key。

### 5.2 回填

`ScatterReplace(selection, replacements)` 首期仅支持与 anchor 相同的载荷类型。
它不接受另一个任意 anchor 参数，从 Selection 取得原始完整批次，并执行：

1. 检查替换结果完整 key 唯一。
2. 替换结果的 key 集合必须与 selection 完全相等；未知、未选中、重复或缺失均失败。
3. 替换结果允许乱序，按 key 定位到选中位置；不能只因数量相同而接受。
4. 全部校验成功后构造完整输出，原顺序和所有 req_id/sub_id 不变。
5. 未选中项保留原载荷，选中项使用对应替换载荷；失败不修改 anchor、replacements 或已有结果。

空 selection 配空 replacements 成功并返回完整原样批次；空 selection 配非空结果失败。
首版不提供“缺少结果就保留旧值”的静默降级策略。

### 5.3 可编译示例必须证明的流程

自由 Batch 示例先对完整输入执行一次生成，再选择需要修正的输出项，第二次只对选中项推理，
随后 Scatter 回第一次完整输出并返回。模型的同步调用和最终 PreservedOutput 校验保持原样。
选择结果为空时示例明确跳过第二次调用；不自动重试失败的模型调用。

使用独立 Mock 脚本验证：第一轮的完整 prompt、第二轮的确切子批次、实际调用次数、最终各
位置的期望文本。不能只断言最后输出长度正确。任一调用失败时返回失败；业务若另有降级，
必须显式表达并满足其外部状态契约。

## 6. SplitPayloads：编号与计数由框架处理

业务回调形状为 `split_one(const InputPayload&) -> NodeResult<vector<OutputPayload>>`，
允许普通 vector 返回值的薄适配。它只决定如何切分载荷及子项顺序。
框架返回普通 `SplitResult`，含 children 批次和每个父输入对应的 `Int32Batch counts`。

1. 输入完整 key 重复则失败。
2. 按父输入顺序处理；children 顺序为父输入顺序下的子项顺序。
3. 每个 req_id 用独立的宽整数计数器，从 0 开始连续分配 uint32 sub_id；同请求的多个父项
   共用这个计数器，即使父项在输入中不连续也不能重置。
4. 使用子编号前检查 uint32 容量；每个父项的 count 写入 Int32 前检查容量，不能先窄化再检查。
5. counts 的顺序与输入相同，key 保留父项原 req_id/sub_id；count 是该父项的子项数量。
6. 回调返回零个子项是合法值，count=0，不消耗子编号；空输入返回两个空批次。
7. 载荷拆分失败、编号/计数溢出或分配异常不返回部分成功结果、不发布任何输出。

空文本的业务行为由 splitter 明确实现：TextChunk 当前空字符串产生一个空文本子项，迁移
后保持此行为，不因通用 helper 允许零项而修改它。UTF-8 校验、码点边界、chunk_size/overlap
规则和错误码继续由 TextChunk 的纯拆分函数负责。

其中 UTF-8 等载荷语义错误由纯回调返回；编号类错误由 helper 产生，在 TextChunkNode 边界
使用结构化 detail 映射：duplicate → 既有 `-4003`，sub_id_overflow → `-4004`，
count_overflow → `-4005`；回调的 UTF-8 `-4002` 保留并由 helper 补原父项 req_id/sub_id。
错误常量继续由现有 node_error::text_chunk 持有，helper 不依赖 TextChunk 的业务错误表。
新旧诊断的已有关键字段和数值码由迁移测试锁定，不把来源/溢出错误都改成通用内部失败。

children 的新 sub_id 不携带任意父 lineage。counts 记录一层父项计数，不承诺仅凭 children
的两个编号可以逆向恢复多层拆分。需要持久父子图或跨层关联的新数据类型另行设计。

TextChunkNode 保留原 `1:N/generate_sub_id` children 与 `1:1/preserve` counts 两个输出端口，
在 helper 成功后沿用原高级 Node 发布顺序。这里是局部结果构造的失败安全，不是两个
Blackboard key 原子发布保证。

## 7. Pipeline 与 Catalog 兼容边界

[PipelineCatalog](../../src/core/pipeline_catalog.cpp) 当前数量枚举为 `1:1/1:N/N:1/N:M`，
来源枚举为 `preserve/generate_sub_id/aggregate/independent`。
[PipelineValidator](../../src/core/pipeline_validator.cpp) 的数量兼容允许相同值、任一端 N:M
或消费端 N:1；来源消费端 preserve/aggregate 有既有宽泛兼容规则。
实施 M0 应用目标构建 Catalog 和源码确认这些基线，而非从本文推断未来版本的可用能力。

本篇不修改这些枚举或兼容表。新增一个所谓 `0:1/filter` 字符串不足以证明过滤后的请求
完整性；若直接允许它连接普通 Map，Map 再声明一对一可能掩盖上游已经缺项的事实。

- Select/Join/Group 是 Run 内部操作，不改变端口 Definition。
- 最终返回 PreservedOutput 时仍须与显式 anchor 等长逐项对齐；子批次必须显式恢复全量。
- 新工具不会取消 Adapter 的运行时完整性检查。
- 独立 Filter/Group 节点及新的输出策略，需要后续 RFC 同时规定消费者、全链完整性传播、
  Definition 生成、AuthorNode 运行时校验和 Catalog 消费者迁移，不列为本篇完成条件。

当前 `BatchSpec/MakeBatchSpec/AuthorNode` 直接绑定 PreservedOutput，并非现成的任意输出
policy 插槽；本轮不为局部 helper 重构它。这样可以让普通批次工具先独立落地。

## 8. 试点、源码与兼容迁移

| 必需试点 | 交付形态 | 选择理由 |
| --- | --- | --- |
| Join | 同来源问题/属性两批次的可编译自由 Batch 例子 | 精确覆盖乱序、缺失和左关联，不把复杂业务历史重复策略强行统一 |
| Group | 一个明确 anchor、含空请求组的参考文本例子 | 证明多输入按 request 关联，输出仍可保持每个 anchor 的来源 |
| Select/Scatter | 条件第二次 LLM 推理例子与 Mock | 证明模型只调用子批次、结果完整恢复 |
| Split | 迁移 TextChunkNode | 当前重复、溢出和计数语义已明确，可对照原输出 |

前三个例子放在现有 `dev_support/node_authoring/`，作为现有测试 runner 编译的示例，
不为展示 helper 向生产 Catalog 添加示例业务 Node。TextTemplate、VectorTopK、TextRerank
存在各自的重复项/排序/缺失规则，本篇不要求整体迁移；后续迁移必须先锁定行为。

不改变既有公共 ABI、TraceableItem 布局、Pipeline JSON、生产 Node 的端口/参数或模型能力。
可逐个替换 helper 调用回原函数实现；TextChunk 仍注册原节点名，不能双重注册。
本篇不扩展 scaffold 的公开选项；用编译示例和作者指南暴露工具即可。

## 9. 验证矩阵

为新算法增加同一 Node runner 下的 focused 测试源文件，复用
[test_function_node.cpp](../../tests/unit/nodes/test_function_node.cpp) 的模型/端口夹具。
不新建测试可执行文件，不从实现生成期望。

| 操作 | 必须覆盖的独立断言 |
| --- | --- |
| Join | 两侧乱序但正确对应；左序稳定；左右重复分别失败；exact 缺失/额外失败；left 缺失为空、额外失败；空批次组合 |
| Group | 多父项同请求、交错输入；anchor 首现顺序；members 相对顺序；空组保留；未知请求/完整 key 重复失败；零请求；A0/B0/A1 分组后的一对一输出仍保持该原序 |
| Select/Scatter | 全选/全不选/部分选择；第二轮只处理所选项；乱序替换成功；重复/未知/未选中/缺失失败；未选中载荷不变；失败时原批次不变 |
| Split | 同请求多父项及交错请求；连续子编号；父 key 计数；零子项、空输入；回调失败；uint32/Int32 边界 |
| 视图/所有权 | 从临时输入构造被编译拒绝；owned Materialize 不依赖 Selection 存活；借用视图在输入活跃期的读操作正确 |
| 函数式集成 | 最终保序校验仍能拒绝直接返回子批次；错误未提前污染 Context；原 Map/LLM 空批次语义保持 |

溢出测试通过编号分配器的内部测试接缝设置近边界计数，或直接测受检转换，不分配数十亿
元素；测试接缝不成为作者可任意指定来源编号的公开配置。

扩展 [test_text_chunk_node.cpp](../../tests/unit/nodes/test_text_chunk_node.cpp)，逐项比较迁移前
已知文本、空字符串、UTF-8 错误、overlap 和多父项输入的 children/counts。
现有 [Catalog/Validator 套件](../../tests/integration/pipeline/test_pipeline_catalog_validator.cpp)
继续证明 TextChunk 端口及原数量兼容未变。对其实际影响的 Pipeline 执行 validate/plan，并
运行现有对应 Demo-supported 方案检查请求 ID、状态、输出内容与计数用途。

用可用的 ASan/UBSan 配置检查 focused 视图/索引/拆分测试，记录具体环境及任何限制。
补充确定种子的随机排列测试，对照简单人工规则/独立参考算法验证来源，而非让两个封装
调用同一内部 helper 相互作证。记录索引构建次数与代表性批次耗时/内存，避免每个请求项
重新建全量索引导致 O(N²) 行为。

作者体验任务：实现带参考分组和条件二次推理的 Node，不手写 key 拼接、来源 unordered_map、
替换结果关联或编号分配；作者仍要明确 anchor 和缺失策略。记录求助、修改位置和实际模型
调用次数，真实试用与工程检查分别记录。

## 10. 实施步骤与阶段出口

| 阶段 | 工作 | 出口条件 |
| --- | --- | --- |
| M0 基线 | 新构建 Catalog；确认复用工具；锁定 TextChunk 行为、示例输入期望和性能基线 | 不修改生产来源契约；测试所有者与样例清楚 |
| M1 索引与视图 | 统一内部完整 key 索引；实现 Join/Group；所有权及失败规则 | 顺序/重复/缺失/空组测试通过，无 Context 依赖 |
| M2 选择回填 | Selection 工厂、Materialize、ScatterReplace 与错误映射 | 全量恢复、乱序替换和失败不修改输入通过 |
| M3 拆分试点 | SplitResult、受检编号/计数；提取 TextChunk 纯拆分函数并迁移 | 原输出/端口/错误码不变，溢出和空字符串测试通过 |
| M4 作者例子 | Join/Group 自由 Batch、条件二次生成、独立 Mock 期望 | 编译和运行证明实际 prompt、调用次数及最终完整来源 |
| M5 验证交付 | 更新作者概念/示例导航与必要 Changelog；方案执行、sanitizer、性能及体验记录 | 按 CONTRIBUTING 完成一次 canonical gate；记录未覆盖环境和试用状态 |

借用生命周期、来源/数量语义与 TextChunk 迁移需独立 Reviewer。
大规模生产节点迁移、新端口类型和独立 Filter Node 不影响本篇工程出口。
体验记录缺失时保留待办，不把 Agent 自测写作真实开发者已通过。

## 11. 实施与最终结果记录

| 项目 | 当前状态 |
| --- | --- |
| 设计与工具实现 | Completed；范围限定为局部工具及 TextChunk 试点 |
| 工程、所有权与性能验证 | 45 项批次工具测试、TextChunk/函数式 Node 验证、ASan/UBSan、代表性成本测量与 canonical gate；具体命令、结果与限制见下方记录 |
| 作者指南 | 已补普通 Batch 工具入口、借用生命周期、失败诊断及三个编译示例导航 |
| 真实开发者试用 | 待办；三个 starter 的 Agent 工程检查不替代组合任务的真实试用 |
| 工程完成条件 | M0–M5 工程实现和验证完成；真实试用按第 10 节保留待办，不宣称已通过 |

[修复、验证与测量记录](reviews/0055-traceable-batch-verification.md) 包含运行环境、
const 右值及非法谓词的编译拒绝、诊断完整 key 回归、Int32/uint32 边界、独立 Reviewer
复核、sanitizer、Pipeline/Demo、哈希索引次数、耗时/内存与未覆盖范围。

- **M0–M1**：Catalog 确认 TextChunk 既有端口；Join/Group 覆盖重复、缺失、空组、乱序及
  const/non-const 临时对象拒绝，保持 anchor 顺序与借用生命周期规则。
- **M2**：Selection 只能由工厂产生，Materialize 拥有数据；Scatter 按完整 key 恢复全量。
  谓词错误返回类型编译失败，回调失败在 AuthorNode 边界补完整来源且保留原错误码/内容。
- **M3**：TextChunk 复用 SplitPayloads，保持 UTF-8、空字符串、overlap、来源及历史错误码。
  Int32 受检转换与 UINT32_MAX 接缝分别验证容量边界，不分配数十亿子项。
- **M4**：Join、Group、Select/Scatter 编译示例由 NodeHarness 与 Mock 模型测试，断言输入、
  实际调用次数及最终文本；缺失真实开发者组合任务体验明确列为待办。
- **M5**：补齐作者概念/示例导航、方案执行与成本记录。Debug ASan/UBSan 聚焦检查和
  Release 默认后端 canonical gate 分别运行，不将单次门禁写成 Debug/Release 双配置验收。
