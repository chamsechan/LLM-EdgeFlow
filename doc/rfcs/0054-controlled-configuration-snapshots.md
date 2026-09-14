# RFC 0054：Control 作者接口与不可变配置快照

- **RFC 编号**：0054-controlled-configuration-snapshots
- **创建日期**：2026-09-14
- **文档状态**：Proposed
- **关联分支**：`docs/framework-authoring-rfcs`；建议实施分支 `refactor/control-snapshots`
- **目标版本**：下一次投产前开发接口版本
- **负责人 / 作者**：LLM-EdgeFlow contributors
- **设计基线**：`3fb4ba5be18f00fd8855b7d2de900e80ad203335`
- **关联决策**：补充 RFC-0018、0041、0044；实现 RFC-0052 第 9.3 节延期的普通参数更新支持。

本文是待实施规格；新增接口名称均为拟议名称。与
[RFC-0053](0053-function-oriented-adapter-authoring.md) 和
[RFC-0055](0055-traceable-batch-operations.md) 独立，不以它们先完成为编译依赖。
开发与交付流程遵循 [CONTRIBUTING](../../CONTRIBUTING.md)。

## 1. 问题与范围

### 1.1 当前基础与缺口

[RFC-0041](0041-control-authoring.md) 已提供 Control schema 校验、实例定向、Operator/Demo
透传和编译 starter。本篇复用这些能力，解决节点内部更新机制仍由作者重复维护的问题。

| 现状 | 源码依据 | 本次处理 |
| --- | --- | --- |
| starter 手写解析、shared_mutex、临时新值和请求快照 | [starter_control_node.cpp](../../dev_support/node_authoring/starter_control_node.cpp) | 改为声明更新命令和普通参数 |
| TextTemplate 读旧配置、解锁编译、再整体替换 | [text_template_node.cpp](../../src/common_nodes/text_template_node.cpp) | 使用统一 writer 串行事务，防止直接并发 writer 基于同一旧值覆盖其他字段 |
| TextRuleMatch 在提交锁内替换已提供字段，Process 持读锁匹配 | [text_rule_match_node.cpp](../../src/common_nodes/text_rule_match_node.cpp) | 保持现有补丁语义，统一为不可变快照 |
| 函数式 Node 参数 Init 后不可变，Control 默认 Unsupported | [function_node.h](../../include/nodes/function_node.h) | 添加显式 opt-in 的普通字段更新组合 |
| 参数已有类型绑定，Control 已有结构校验 | [parameter_binding.h](../../include/nodes/parameter_binding.h)、[control_payload.h](../../include/contracts/control_payload.h) | 共用字段事实和原 schema 执行器，不造第二套校验语言 |

### 1.2 必需范围

1. 可供高级 Node 和函数式 Node 共用的单实例配置快照组件。
2. 普通字段命令声明：显式 PatchFields 与 ReplaceFields，复用初始化参数中的字段绑定。
3. 复杂业务保留 typed 更新函数入口，处理规则编译、模板校验等实际语义。
4. 迁移 Control starter、TextTemplateNode、TextRuleMatchNode；增加函数式 Map/Batch opt-in 例子。
5. 验证失败保留旧配置、单请求一致快照、两个 writer 不丢更新和旧快照寿命。

不涉及模型加载/替换、Backend 资源、外部文件监视、配置持久化、请求状态存储、跨节点事务、
异步推理或全局调度。本篇只处理可在 CPU 内构造、校验并发布的 owned 配置及不可变派生值。

## 2. 公共并发契约与责任边界

**同一公共 handle 的 Process/Control 继续串行。**
[C ABI 声明](../../include/edgeflow/c_api.h)与
[C ABI 实现](../../src/adapter/c_api_adapter.cpp)、
[Operator 实现](../../src/adapter/operator/operator_adapter.cpp)已经通过同一 handle 锁维持此契约。
本篇不移除这些锁，也不以快照宣称同 handle 获得并发推理吞吐。

[SharedAlgorithmRuntime](../../src/adapter/shared_algorithm_runtime.cpp) →
[Pipeline::Control](../../src/core/pipeline.cpp) →
[NodeBase::Control](../../include/nodes/node_base.h)没有额外逐节点 writer 调度器。
新组件明确保证自身读/更新的并发正确性，便于直接 Node 测试和高级调用；其他节点及依赖不会
因此自动获得线程安全性。`parallel_safe` 仍由作者根据完整算法和依赖显式声明。

| 所属层 | 责任 |
| --- | --- |
| 接入适配层 / Integration | 保留 Control 原始文本透传、handle 锁与异常屏障 |
| 流程编排层 / Orchestration | 保留命令注册信息、目标实例定位和既有 best-effort 广播结果 |
| 能力节点层 / Capability Nodes | 新增快照、命令作者接口与业务更新包装 |
| contracts | 继续提供现有字段及 Control schema 规则；不依赖 Node 实现 |

快照组件位于 `include/nodes/configuration_snapshot.h`，作者命令组合位于
`include/nodes/control_authoring.h`，经现有 authoring 入口暴露。
Core 不包含这些头；更新广播没有跨节点回滚保证。

## 3. 状态模型与精确更新算法

### 3.1 State 与快照

每个 Node 实例持有一个 `ConfigurationSnapshot<State>`，内部为 writer mutex 和
`std::shared_ptr<const State>`。State 包含 owned 配置及必要的只读派生值，例如编译后的模板
token 和规则集合。运行中请求数据、模型句柄与可变外部服务不放入 State。

拟议接口形状：

```cpp
std::shared_ptr<const State> Read() const;
NodeControlResult Update(BuildNextFn build_next);
// build_next(const State& current) -> NodeResult<State>
```

这是机制层接口；普通作者通过后述命令声明使用它，无需手写锁或 shared_ptr。
Init 完整构造第一个 State 后才能成功；未初始化不能 Read/Update 成功。
Init 不与 Process/Control 并发，销毁时没有未完成的外部调用，继续服从既有生命周期。

C++17 使用 `std::atomic_load_explicit` / `std::atomic_store_explicit` 的 shared_ptr 自由函数，
读 acquire、发布 release；不使用 C++20 的 `std::atomic<std::shared_ptr<T>>`，不声称 lock-free。
发布后不得经可变别名修改 State；可复用的内部 shared_ptr 也必须指向不可变对象。
旧快照由在途读者持有到使用结束，释放时不依赖已经结束的临时输入或 NodeInitContext。

不要求 State 整体可复制。TextRuleMatch 的 CompiledTextRegex 是 move-only；试点须将普通
配置与不可变 prepared block 分开，未修改的编译块通过 `shared_ptr<const ...>` 共享，修改
的块重新编译后移入新 State。禁止复制底层 regex 句柄、const_cast 修改已发布块，或为了
满足模板而将 vendor 资源变成不受控的共享可变对象。

### 3.2 Process

1. Process 进入业务执行前只读取一次 shared_ptr 快照。
2. 用其中同一份参数处理整个请求批次，包括循环、条件模型调用及后处理。
3. 保持局部 shared_ptr 到业务调用结束；中途不再次 Read，不将 State 裸引用保存到请求外。
4. 新配置发布不修改正在执行的快照；在发布之后开始读取快照的请求使用新状态。

函数式作者仍接收 `const Parameters&`；快照存活由 AuthorNode 包装承担。高级 Node 在
ProcessNode 开始取得只读快照，并将其传给普通业务函数。

### 3.3 Control

1. 识别命令；未知命令仍返回 Unsupported，不触碰状态。
2. 可在 writer 锁外完成原始文本解析、结构验证和与旧状态无关的 typed command 转换。
3. 获取该实例 writer mutex，**在此之后**读取当前快照。
4. 基于最新状态构建完整候选：复制配置、应用显式补丁、编译派生值、验证业务与连接约束。
5. 所有工作成功后构造新的 immutable shared_ptr，并一次发布；随后返回 Handled。
6. 解析、复制、业务校验、编译或分配失败均不发布；保留旧快照并返回既有 Failed 结果。
   异常由既有 NodeBase 屏障转换，RAII 释放 writer 锁，不能越过公共边界。

writer 锁覆盖读旧值、合并、编译、发布全段。首版接受较慢更新阻塞另一个 writer，读者继续
使用旧快照；不采用锁外读取旧值后整体提交，也不引入版本 CAS 重试。
更新函数不得递归调用同一实例 Update 或触发节点执行，避免重入死锁和副作用。

发布是单实例更新的线性化点。两个不同字段补丁按取得 writer 锁的顺序应用；同字段后一个
成功更新覆盖前一个。锁的公平性、跨实例顺序及广播全局一致性不在保证内。
成功返回对象及必要分配在发布前准备；发布之后只做不抛异常的操作。不能先提交新状态，
再因诊断/日志/返回值构造失败向调用者报告 Failed。

## 4. 普通参数作者接口

### 4.1 有限字段命令

在 RFC-0052 的 Spec 上提供可选 `WithControls(...)` 组合，示意如下：

```cpp
// Proposed shape; command IDs remain explicitly selected and registered.
spec.WithControls({
    ReplaceFields(kSetPrefix, "set_prefix", {"prefix"}),
});
```

字段名引用 Spec 已有 `Parameters<T>` 成员绑定；同一份绑定生成初始化 Definition、Control
payload schema 和写入 typed 参数的操作。不重新手写 JSON 类型、默认值及范围。
声明时拒绝未知字段、重复字段、重复命令 ID、不可安全更新的字段和不支持的 schema 映射。
命令 ID 继续按现有 Catalog/注册规则选择，不自动分配新范围。

| 策略 | 缺失字段 | 提供字段 | 其他规则 |
| --- | --- | --- | --- |
| PatchFields | 保留当前值；至少提供一个允许字段 | 用新值替换该字段 | 不填初始化默认值；不做对象深合并 |
| ReplaceFields | 声明集合中每个字段均必须提供 | 一次替换该集合 | 集合外配置保留；并非替换整个 Node 的部署配置 |

两者均拒绝未知字段、显式 null 和错误类型，除非另一个明确的 typed 自定义命令契约允许。
首版自动字段命令覆盖现有绑定支持的字符串、bool、整数、double、字符串数组；复杂嵌套结构
走自定义命令。数组整体替换；空数组和空字符串是否有效由字段/业务规则决定，不视作缺失。

生成 schema 使用现有 Control 校验支持的关键字。`default` 在 Control schema 中只具说明
意义，不能因初始化有默认值而自动填补补丁。初始化的 normalize 和 Control 的 presence-aware
assign 是不同步骤；复用字段规则，不对 patch 调用完整 Parse 来重置缺失成员。

初始配置与更新后的完整参数必须调用同一份 typed 语义验证/准备函数。新增受控参数的
`Prepare(T* next, const BindingFacts&, diagnostic)` 扩展点，用普通成员完成跨字段检查及派生值
重建；每次均从当前配置重新生成相关派生值，禁止在旧派生集合上累加。
既有只有 JSON 语义 parser 的高级 Node 可先使用第 4.2 节路径，不强制序列化 State 回 JSON。

受控组合的完整调用顺序固定如下：初始化先执行原字段 normalize、WithParser（如有）和
普通成员赋值，再 `Prepare → Parameters.Validate → 有计划时 ValidateBindings`；Control
先基于最新快照复制普通配置、仅给出现字段赋值，再运行同一条 Prepare/Validate/Bindings
链。任一步失败均不发布。纯 Prepare 只构造 owned CPU 值，不执行外部 I/O 或加载模型；
Definition 的预检回调也使用这条语义链，保持静态预检与 Init/Control 一致。

普通字段命令只能选择有成员绑定的字段，不能直接控制 WithParser-only 字段。首版对同时
具有 WithParser 和普通字段 Control 的 Spec，要求显式 typed Prepare 负责从当前普通配置
重建受其影响的派生值；未提供时在声明阶段拒绝该组合。作者可声明一个无派生依赖的空
Prepare，但必须用测试证明更新不留旧派生值。复杂 parser 的初始 JSON 不保存在快照里，
也不通过把 State 序列化回 JSON 来模拟更新。

模型绑定字段、端口绑定、parallel_safe、biz 名称及其他部署/拓扑字段不能进入普通命令的
更新集合。声明期依据 Spec 的模型依赖和绑定事实检查，而非维护手工字段黑名单。

### 4.2 复杂更新仍是普通函数

模板和规则使用已有 payload schema，经 `ParseControlPayload` 转成 owned typed command。
业务实现下面的普通更新函数形状：

```cpp
NodeResult<TemplateState> BuildNextTemplate(
    const TemplateState& current,
    const TemplateUpdate& update,
    const BindingFacts& bindings);
```

`TemplateUpdate` 必须记录字段是否出现；不能用空字符串或默认 false 代替 presence。
框架负责路由、解析边界、writer 锁与发布；函数负责 values 合并、模板编译和语义校验。
现有复杂 schema 不为统一字段 DSL 而重写，原命令的空对象/未知字段/类型及错误码语义保持。

### 4.3 连接事实的寿命

涉及模板变量是否有输入连接时，Init 从已验证计划复制输入 logical_name 集合，形成 owned
`BindingFacts`；保留“无计划”和“有计划但没有连接”的区别，复用当前 TextTemplate 的策略。
不保存 NodeInitContext、plan/config/diagnostic 的借用指针。
BindingFacts 在 Init 后不可变；Control 不更改端口、不调用 PipelineValidator。

## 5. 与现有 Node 作者接口的衔接

先让快照组件可由高级 Node 组合使用，再将 `WithControls` 接入 AuthorNode。
Map/Batch 继续使用同一套输入绑定、模型绑定、异常屏障、结果校验和输出发布机制。
需要提取公共生命周期函数时只提取一次，不为受控 Node 复制完整 Process 实现。

- 无 WithControls 的 Spec 保持 Init 后不可变和 Unsupported 行为，无额外每请求快照成本。
- 有 WithControls 的 Spec 由同一声明生成 `control_commands`，使用既有注册/Catalog 路径。
- 使用快照不自动把 parallel_safe 改为 true；常驻逻辑对象及可变捕获仍不属于简化入口。
- 多模型依赖与运行时资源始终由原机制管理；快照更新不能绕过模型能力或并发预检。
- NodeHarness 增加调用现有 Control 并观察结果的薄入口即可，不建设第二套 Control 测试运行器。

不改变 C ABI、Pipeline JSON 或 Control wire schema 的总体格式，不要求 Catalog schema 升级。
新自动字段命令的 schema 应能由现有 Studio/Explain 正常展示；新增元数据必须先明确消费者，
不能要求 UI 推断框架没有声明的更新语义。

## 6. 迁移规格

| 对象 | 实施要求 | 保持的外部行为 |
| --- | --- | --- |
| Control starter | prefix 字段 + ReplaceFields；业务只处理普通 prefix 参数 | 原命令 ID、名称、64 UTF-8 字节限制和缺字段拒绝 |
| TextTemplateNode | 状态收拢为配置与编译 token；自定义 typed 更新函数；移除贯穿 Process 的读锁 | values 按现有规则合并；prompt_id；attributes 已连接时行为；模板/连接错误保持旧配置 |
| TextRuleMatchNode | 将 categories/rules 与必要配置放入快照；构造完整候选后发布 | 缺 categories/rules 时各自保留；数组替换；组合更新任一非法则全部不更新 |
| 函数式例子 | 一个 Map 更新字段例子，一个 Batch 整批一致例子 | 输入输出和来源仍按 RFC-0052 验证 |

允许逐节点迁移，保留高级 Node 原扩展接口，不要求全部节点受控。
回退单个试点时恢复该节点的旧内部实现，公共命令不变；保留其他节点仍使用的快照组件。
历史 RFC 保留原设计背景，本篇仅接续其延期部分。

## 7. 验证与完成条件

### 7.1 必须有直接并发证据

公共 C ABI 线程测试会被 handle 锁串行化，不能证明快照组件自身正确。以下测试必须直接调用
组件或迁移节点，使用条件变量/屏障安排时序，不依赖 sleep 和线程运气：

| 场景 | 独立期望 |
| --- | --- |
| writer A 更新 prefix，writer B 更新 suffix | 两个成功更新后两个字段均保留；B 不能提交基于 A 之前的旧整体状态 |
| writer 编译/校验/分配阶段失败 | 无新快照发布；旧参数、旧派生值均可继续执行 |
| reader 已取旧快照，writer 发布新值 | 旧 reader 整批使用旧版本并安全完成；后续 reader 使用新版本 |
| 一批多样本处理中发生发布 | 该批结果不能混用前缀、规则或模板版本 |
| 两个同字段更新 | 按受控时序确认最后成功提交生效 |
| 更新字段缺失/空值/未知字段 | Patch 与 Replace 的 presence 规则各自成立；默认值不覆盖缺失字段 |
| Init 失败、未初始化、未知命令 | 保持节点生命周期与 Unsupported/Failed 契约 |

### 7.2 测试所有者与验证层级

- [test_function_node.cpp](../../tests/unit/nodes/test_function_node.cpp)：组合声明、字段 schema、
  默认 Unsupported、单请求参数快照；机制用例可加同 runner 下的新测试源文件。
- [test_text_template_node.cpp](../../tests/unit/nodes/test_text_template_node.cpp) 与
  [test_text_rule_match_node.cpp](../../tests/unit/nodes/test_text_rule_match_node.cpp)：业务结果、
  presence、非法更新不改变状态和迁移回归。
- [test_runtime_control_and_hot_swap.cpp](../../tests/integration/runtime/test_runtime_control_and_hot_swap.cpp)：
  实例定向、未知目标、广播部分失败保持 best-effort、公共同 handle 串行行为。
- [Catalog/Validator](../../tests/integration/pipeline/test_pipeline_catalog_validator.cpp)：
  生成命令可见、非法/冲突声明拒绝、原生元数据仍为唯一来源。

实施时对直接并发组件/Node 套件执行可用的线程检查器专项并记录精确构建参数；线程检查器
不能运行时记录环境限制，确定时序的并发测试仍必须通过。测试不要求真实模型资源。
测量迁移节点的代表性批次耗时及快照分配/持有情况，与 M0 同环境基线比较；不设置脱离基线
的收益百分比，不用公共入口吞吐证明节点读写并发。

作者体验任务：增加一个普通可更新字段，修改参数成员、一次字段声明/命令引用和独立期望，
不编辑锁、shared_ptr、Process 快照搬运或命令路由。真实试用与工程自测分别记录。

## 8. 实施步骤与阶段出口

| 阶段 | 工作 | 出口条件 |
| --- | --- | --- |
| M0 基线 | 新构建 Catalog、原命令 schema/行为、现有测试及代表性执行成本；补齐 presence 特征测试 | 明确同 handle 串行、广播非事务及各业务合并语义 |
| M1 快照机制 | ConfigurationSnapshot、owned 状态与 writer 更新；直接并发测试 | 无丢更新；失败不发布；旧 reader 寿命正确 |
| M2 高级节点试点 | 迁移 TextTemplate/Rule，提取 typed update 与 BindingFacts | 既有业务/Control 测试及直接节点并发测试通过 |
| M3 作者组合 | 普通字段 schema 投影、presence-aware assign、typed Prepare、WithControls | 无控制的 Node 保持原路径；初始化与更新复用语义规则 |
| M4 示例与工具 | 迁移 starter，增加 Map/Batch 可编译例子，最小 Harness 入口 | 例子在现有 runner 编译；Catalog、定向 Control 与业务执行可观察 |
| M5 验证交付 | 更新 first_control/作者指南与必要 Changelog；性能/线程检查专项及体验记录 | 按 CONTRIBUTING 完成一次 canonical gate；如实记录专项和试用限制 |

生命周期、并发和 AuthorNode 变更需独立 Reviewer。只有所有必需工程出口满足才能声明工程
交付；真实试用尚未进行时保留状态和待办，不以 Agent 自测关闭体验验收。

## 9. 实施与最终结果记录

| 项目 | 当前状态 |
| --- | --- |
| 设计文档 | Proposed；未改变运行时并发契约 |
| 生产实现与迁移 | 未开始 |
| 工程、并发与性能验证 | 待实施后填写命令、基线和结果 |
| 开发者试用 | 待记录 |
| 完成条件 | M0–M5 必需交付、验证、现行指南及体验结果记录完成；按 CONTRIBUTING 更新状态 |

当前文档通过检查不代表快照组件已经实现。实施差异、验证结果和保留边界直接维护在本文。
