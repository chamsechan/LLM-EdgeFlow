# Pipeline 与 Blackboard 基线重评审及精简实现建议

> 重评审日期：2026-08-21
>
> 指定基线：`6ea4d66`（该提交仅新增初次评审文档，源码等同其父提交 `aa69bde`）
>
> 原评审：`doc/pipeline_blackboard_architecture_review.md`
>
> 评审原则：只判断指定基线，不使用后续 Round 4 实现反推结论；以简单配置、容易使用、降低普通业务开发者门槛为优先目标。

## 1. 重评审结论

初次评审的总体判断是合理的：四层架构、Adapter → AlgContext → Pipeline → Node → Engine 的主路径无需推倒重来；现有顺序业务可以继续使用；当时的实现还不足以安全支持普通开发者或 AI 自由组合 Pipeline。

但是，原评审把三类目标放进了同一轮整改：

1. 当前代码中已经存在的安全和确定性问题。
2. 开放自由 Pipeline 编排前必须具备的平台契约。
3. Manifest、Schema、Visualizer、全事务 Control、并发热重载等长期产品能力。

问题识别本身大多有源码证据，后续整改膨胀主要来自优先级和方案边界，而不是因为四层架构方向错误。重新开始时不应照原 13 项逐项建设完整终态，而应先形成一个小而闭环的“安全顺序运行时 + Code-first 配置与节点契约”，复验通过后再扩展。

### 1.1 修正后的目标

本轮目标是：

- 保留 `nlohmann::json` 作为唯一配置数据格式和解析库。
- 不引入 JSON Schema、Schema validator、IDL 或反射框架。
- 现有 Pipeline JSON 尽量兼容；严格模式下错误必须 fail-closed。
- 普通开发者复用节点时只修改 JSON。
- 普通开发者新增节点时继续使用 `INode + REGISTER_NODE`，不手写 Registry 和 Catalog。
- 默认顺序执行、同 Handle 操作采用安全的保守策略。
- 先用一个代表性业务证明 Node Definition、类型化 Key 和静态校验可行，再批量迁移。

本轮明确不以以下能力为交付目标：

- Business Manifest。
- 物理 JSON Schema 文件。
- 动态插件 ABI。
- 通用并发 `Alg_Destroy` 保证。
- 多节点三阶段事务 Control。
- Pipeline 热重载和完整事务化资源切换。
- Visualizer 全面重写。
- 一次性迁移全部 Node、Adapter、Engine 和 skill。

### 1.2 基线复现证据

在与当前工作区隔离的 detached worktree 中检出 `6ea4d66`，执行：

```bash
cmake -S . -B build-review -DCMAKE_BUILD_TYPE=Release
cmake --build build-review -j4
ctest --test-dir build-review --output-on-failure
```

结果为 Release 完整构建通过、默认 CTest 13/13 通过。默认测试清单包含 LayerGuard、FrameworkCore、全部业务 Pipeline、DAG、Control、并发边界和 Adapter 安全测试。

原评审记录为 14/14，本次不能按默认配置复现这个数量。原因是 `RealModelE2ETest` 只有在 `ENABLE_REAL_MODEL_TESTS=ON` 时才注册，默认 CTest 明确只有 13 项。因此“现有已覆盖场景可用”的结论成立，但后续评审证据必须同时记录 CMake 选项和测试名称，不能只记录一个可能随选项变化的总数。

## 2. 对原 13 项问题的重新定级

| 原编号 | 事实判断 | 新定级 | 重评审结论与精简处理 |
|---|---|---|---|
| PLB-001 | 成立 | 条件 P1 | `Get()` 的确返回锁外可变指针；现有生产 Pipeline 默认顺序且节点通常读取旧 Key、发布新 Key，因此当前风险被使用方式部分约束。不要立即引入 shared ownership/Lease；先改为只读 `Read()`、write-once `Publish()`，执行期禁止覆盖、Erase 和 Clear。只有继续开放未校验 parallel 时才是立即 P1。 |
| PLB-002 | 成立，但原方案边界过大 | P1 | Process、Control 与节点状态没有统一契约是真问题。第一阶段串行化同 Handle 的 Process/Control；明确要求调用方在 Destroy 前停止并等待其他调用。裸 `void*` ABI 下不承诺任意线程与 Destroy 竞态安全，不建设全局 Handle 租约系统。 |
| PLB-003 | 完全成立 | P1 | 这是开放配置化组合前最关键的架构缺口。实现最小 Node Definition、类型化 BlackboardKey 和静态生产者/消费者校验；不在第一版加入完整基数、资源、线程模型和 Control Schema。 |
| PLB-004 | 完全成立 | P1 | 宽松解析和 `value()` 分散默认值会让错误静默生效。改为统一的 Code-first `nlohmann::json` 解析函数，不使用 JSON Schema。 |
| PLB-005 | 成立 | P2 | 多套元数据确实漂移，但不能先修 Web/skill 再稳定代码契约。Node Definition 稳定后生成 Catalog，然后再让工具与 skill 消费 Catalog。 |
| PLB-006 | 完全成立 | P1，快速项 | NodeFactory、EngineFactory 和 ModelManager 的重复 ID 静默覆盖应直接 fail-closed，改动小且收益高。 |
| PLB-007 | 成立，但不应独立扩建 | 并入 PLB-001/003 | 在静态端口校验完成前，把 parallel 标记为受限能力；没有完整 Definition 的 Node 不允许进入 parallel 计划。暂不实现任务取消和复杂错误聚合。 |
| PLB-008 | 成立，但原方案过度 | P1 | 未知命令默认成功必须修复；第一版采用明确目标节点、未知命令失败、Process/Control 串行。暂不做广播 Prepare/Commit/Rollback。 |
| PLB-009 | 成立 | P2 | Build 先产生副作用再失败的问题存在，但当前主要由 Create 一次性调用。第一阶段先保证 Parse/Validate 在模型加载前完成；完整不可变 ExecutionPlan 和热重载事务留到后续。 |
| PLB-010 | 理论成立，但无生产调用 | Backlog | `SessionContext::SetResource/GetResource` 在指定基线没有生产使用者。先记录限制，不为未使用接口建设复杂类型系统；新增真实需求时再设计。 |
| PLB-011 | 成立 | P1（缩小范围） | 第一阶段只为 Parse/Validate/Build 提供一个小型结构化 Diagnostic；Execute 继续使用 int + AlgContext error。不要一次统一所有层级 Status。 |
| PLB-012 | 成立，但不是独立架构项 | 验收维度 | 将负例和并发测试分别挂到对应整改项，不再作为单独模块扩建。 |
| PLB-013 | 完全成立 | P1，文档项 | 删除超过证据的并发与性能宣传，明确 parallel 和工具能力边界。 |

汇总：13 项源码观察均有依据；其中 8 项保留为首轮主线或快速修复，3 项并入对应实现与验收，2 项降为后续能力。原评审不合理的部分主要是把长期终态当作本轮必须一次完成。

## 3. 建议保留的最小目标架构

```text
Pipeline JSON (nlohmann::json)
    -> PipelineConfigParser
       - 严格读取根、model、node、depends_on
       - 生成 ParsedPipelineConfig
       - 输出 PipelineDiagnostic
    -> StaticPipelineValidator
       - Registry 是否存在
       - DAG
       - Node 输入/输出和类型
       - producer/consumer 祖先关系
       - write-once 冲突
    -> Pipeline Build
       - 校验通过后才加载模型和 Init Node
    -> Pipeline Runtime
       - sequential 默认
       - 只有完整契约且静态校验通过才允许 parallel
    -> AlgContext
       - typed Read
       - write-once Publish
```

该目标只在现有 Layer 2/3 上增加必要契约，不增加新的控制平面，不改变 C ABI，不引入动态插件。

### 3.1 唯一事实来源

| 信息 | 真实来源 | 说明 |
|---|---|---|
| Pipeline 根结构、model 和 node 通用字段 | Core 的 `PipelineConfigParser` | 使用 `nlohmann::json` Code-first 解析 |
| Node 端口、配置字段和默认值 | 代码中的 static `NodeDefinition` | `REGISTER_NODE` 注册时一并注册 |
| 业务拓扑和参数取值 | Pipeline JSON | 业务开发者主要交付物 |
| 可用节点目录 | Registry 导出的 Catalog | 生成文件，不手写 |
| Adapter 外部结构体解析 | Adapter 代码和 AdapterDescriptor | 复杂嵌套 C 指针仍由业务开发者实现并接受专项测试 |

Manifest 不进入当前事实链。未来如果确有交付包索引、负责人、版本绑定需求，再让 Manifest 引用上述产物，不能复制它们的字段定义。

## 4. 精简接口建议

以下为实现意图，不要求名称逐字一致。

### 4.1 PipelineConfigParser

保持一个入口即可：

```cpp
bool ParsePipelineConfig(const nlohmann::json& root,
                         ParsedPipelineConfig* output,
                         PipelineDiagnostic* diagnostic);
```

`ParsedPipelineConfig` 只保存运行时实际需要的强类型字段；Node 私有配置仍保存为 `nlohmann::json`，由对应 Node Definition 校验和补默认值。

第一版只需要几个内部帮助函数：

- `RequireObject`
- `RequireArray`
- `ReadRequired<T>`
- `ReadOptional<T>`
- `RejectUnknownKeys`

这些函数可以先放在 parser `.cpp` 内，不要过早建设通用 JSON 框架。

最低校验要求：

- 根必须是 object。
- `business_name` 必须为非空字符串。
- `models` 必须为 array；model_id 唯一，engine_type 存在。
- `pipeline` 必须为非空 array。
- node `id`、`node_type` 必须为非空字符串；id 唯一。
- `depends_on` 必须为 string array，不忽略非法成员。
- `execution_mode` 只允许 `sequential` 和 `parallel`。
- `max_parallel_workers` 只在 parallel 模式有效并限制合理范围。
- 未知通用字段返回包含 JSON path 的错误。

配置错误示例：

```text
PIPELINE_CONFIG_TYPE_ERROR at /pipeline/1/depends_on:
expected array<string>, got string; use ["node_a"]
```

### 4.2 最小 PipelineDiagnostic

```cpp
struct PipelineDiagnostic {
  std::string code;
  std::string path;
  std::string message;
};
```

第一版不要加入 cause tree、日志上下文、修复动作对象和跨层映射。稳定 code、准确 path、可读 message 足够支撑开发者和 AI 修正配置。

### 4.3 BlackboardKey 与 AlgContext

推荐以增量 API 取代一次性重写：

```cpp
template <typename T>
struct BlackboardKey {
  const char* name;
};

template <typename T>
bool Publish(const BlackboardKey<T>& key, T value);  // write-once

template <typename T>
const T* Read(const BlackboardKey<T>& key) const;    // read-only
```

实施约束：

- 同一请求内同 Key 只能成功 Publish 一次。
- Node 读取到的值不可原地修改。
- `Erase/Clear` 不允许在 Pipeline Execute 期间调用。
- Node 在本地构造完整输出后一次 Publish，保持零额外深拷贝。
- 旧字符串 `Set/Get` 在迁移期保留但标记 deprecated；迁移完成后再收口可见性。

该方案不需要为每次读取分配 `shared_ptr`，也不需要 MutableLease。只要禁止覆盖和删除，返回的 const 指针在请求 Context 生命周期内保持有效。

### 4.4 最小 NodeDefinition

```cpp
struct NodeDefinition {
  std::string node_type;
  std::string description;
  std::vector<PortDefinition> inputs;
  std::vector<PortDefinition> outputs;
  NodeConfigDefinition config;
};
```

节点继续使用现有方式注册：

```cpp
class ScoreFilterNode : public INode {
 public:
  static NodeDefinition Definition();
  bool Init(const nlohmann::json&, SessionContext*) override;
  int Process(AlgContext*) override;
  const std::string& Name() const override;
};

REGISTER_NODE(ScoreFilterNode);
```

`REGISTER_NODE` 内部读取 `NodeClass::Definition()` 并同时注册 Creator 和 Definition。业务开发者不再单独编辑 Registry 或 Catalog。

配置字段使用小型 builder 声明一次：

```cpp
NodeDefinition ScoreFilterNode::Definition() {
  NodeDefinitionBuilder def("ScoreFilterNode");
  def.Input(kScores).Output(kFilteredScores);
  def.Optional<int>("top_k", 10).Range(1, 100);
  def.Optional<float>("min_score", 0.0F).Range(-1.0F, 1.0F);
  return def.Build();
}
```

Builder 的产物只是项目内部 C++ 规则，不是 JSON Schema。Validator 负责校验并生成 normalized config，Node `Init()` 只读取已存在的最终值，不再重复提供默认值。

第一版端口只需要：Key、C++ 类型、required、input/output。基数、append、mutable、线程模型和 Control 描述只有出现真实需求后再增加。

### 4.5 Registry

- `NodeFactory::Register`、`EngineFactory::Register`、`ModelManager::RegisterModel` 返回 bool。
- 首次注册保留；重复 ID 记录确定性错误，禁止覆盖。
- `Alg_Init` 或 Pipeline Build 在发现静态注册冲突时失败。
- Registry 提供只读 `ListDefinitions()`，为后续 Catalog exporter 使用。

### 4.6 Handle 与 Control

第一版采用容易解释和测试的安全默认值：

- 同一 Handle 的 `Alg_Process` 和 `Alg_Control` 串行。
- `Alg_Destroy` 的公共契约明确要求：调用方必须先停止并等待该 Handle 的 Process/Control；违反此前置条件属于调用方错误。
- Control envelope 明确 `target_node`；Pipeline 只调用目标节点。
- `INode::Control` 对未知命令返回 unsupported，不再默认成功。
- 暂不支持广播和跨节点事务。

这样可以避免复杂快照、三阶段事务、租约和全局 Handle 表，同时给普通开发者一个确定行为。

## 5. 推荐实施顺序

每一阶段独立提交、独立复验。不要再次在一个提交中同时修改 Core、全部节点、Control、Catalog、Visualizer、skill 和构建系统。

### R0：冻结范围与能力声明

只修改文档和测试预期：

- 明确 sequential 是默认稳定模式。
- parallel 在端口契约完整前属于受限能力。
- 明确 Destroy 的调用方同步前置条件。
- 删除无基准的性能和“完美线程安全”描述。
- 将 `architecture_v2.puml` 中 Schema、Manifest 标为 future/optional，不作为本轮交付。

验收：文档、PlantUML、测试名称与真实代码能力一致。

### R1：严格配置、确定性注册与最小诊断

建议修改范围：

- `include/core/pipeline_config.h`
- `include/core/pipeline_diagnostic.h`
- `src/core/pipeline_config.cpp`
- `src/core/pipeline.cpp`
- `include/core/node_registry.h`
- `include/engine/engine_registry.h`
- `include/core/session_context.h`
- 对应测试

实现：

1. 使用 `nlohmann::json` 集中 Parse/Normalize。
2. Parse、通用结构校验和 DAG 校验全部成功后，才加载模型与 Init Node。
3. 拒绝重复 Node/Engine/model ID。
4. 配置错误返回 `PipelineDiagnostic`。
5. 不增加 Node Definition、Catalog、Visualizer 和脚手架。

R1 完成后立即复验，不继续顺手实现 R2。

### R2：类型化 Key 与一个业务试点

只选择 `keyword_match` 或另一个无模型、单节点业务：

1. 增加 `BlackboardKey<T>`、`Read()` 和 `Publish()`。
2. 增加最小 NodeDefinition/Builder。
3. 迁移该业务 Adapter、Node 和 Pipeline。
4. 静态校验 ingress → Node → egress 的 Key、类型和生产者关系。
5. 验证开发者是否觉得 Definition/Builder 比原先手写字符串更容易。

若试点 API 仍需要业务开发者填写重复类型、默认值或 Catalog 元数据，应在试点阶段简化，不能立即批量迁移。

### R3：批量迁移与 parallel 门禁

试点验收后再执行：

- 按业务逐个迁移现有 Node 和 Adapter Key。
- 每个业务单独提交或以小批次提交。
- 没有完整 Definition 的 Node 只能 sequential。
- 只有静态校验确认无缺失 producer、类型不匹配和同层重复写，才允许 parallel。

R3 不同时修改 Control、Catalog 和 Visualizer。

### R4：Catalog 和低门槛工具

Node Definition 全量稳定后：

- 从 Registry 生成 Catalog，不手写 JSON Catalog。
- 提供 `--list-nodes`、`--describe-node` 和 `--validate pipeline.json`。
- 更新 `pipeline-composer` 和开发指南，使示例从 Catalog 和真实测试获得验证。
- 最后再评估 `create_node.py`；脚手架只生成机械模板，不隐藏架构规则。

## 6. 开发者低门槛验收

### 6.1 复用现有节点

开发者应只需要：

1. 查询节点说明。
2. 修改一份 Pipeline JSON。
3. 运行一个 validate 命令。
4. 运行目标业务测试。

不得要求修改 CMake、Registry、Catalog、Core 或 C ABI。

### 6.2 新增普通节点

典型开发者只修改：

- 一个 Node 实现文件。
- 一个 Node 测试文件。
- 一个 Pipeline JSON。
- CMake 源文件/测试登记；后续可由脚手架辅助。

Node Definition 与 Node 实现在同一文件中，通过 `REGISTER_NODE` 一次注册。端口类型从 `BlackboardKey<T>` 推导，开发者不手写 Type ID 字符串。

### 6.3 新增业务 Adapter

由于外部 C 结构体可能包含枚举判别、指针和多重嵌套，Adapter 仍然是业务开发者不可避免的编码面。平台应降低重复劳动，但不能用通用配置替代真实 C 内存语义：

- 复用现有 validation/copy helper。
- 提供扁平结构、数组、tagged union 和指针树模板。
- 强制空指针、长度、枚举、嵌套上限、所有权和输出容量测试。
- Adapter 只发布共享 typed key，不修改 Core。

### 6.4 可量化目标

- 复用已有 Node 组成顺序 Pipeline：受训开发者 10 分钟内完成。
- 新增两个配置字段的普通 Node 并接入 Pipeline：30 分钟内完成。
- 任意配置错误应在 Build 前得到 code + JSON path + 修复提示。
- 普通开发者不需要理解 `std::any`、Registry 内部结构或 Pipeline 调度实现。

## 7. 最小验收用例

R1 必须覆盖：

1. 未知根字段、错误 execution_mode、错误 depends_on 类型全部失败。
2. 重复 node id、model id、Node 注册和 Engine 注册全部失败。
3. 配置失败发生在 Engine Load 和 Node Init 前。
4. Diagnostic 的 code、path、message 稳定。
5. 现有顺序业务 Pipeline 全部通过。

R2/R3 必须覆盖：

1. 缺失输入 producer 在 Build 前失败。
2. 同名 Key 类型不一致在 Build 前失败。
3. 同一 Key 第二次 Publish 失败且不覆盖第一次结果。
4. 节点读取只得到 const 数据。
5. 未完整声明端口的 Node 不能启用 parallel。
6. 一个试点业务端到端结果与基线一致。

Handle/Control 必须覆盖：

1. 同 Handle 的 Process/Control 不并发进入 Node。
2. 未知 target、未知 cmd 和错误 payload 失败。
3. 文档明确 Destroy 前置条件；测试只在满足前置条件时调用 Destroy。

每个阶段仍需执行仓库要求的格式、CTest 和 `run_all_tests.sh`。TSan 用于验证声明支持的并发场景，不用于证明明确禁止的并发 Destroy。

## 8. 最终建议

建议从 `6ea4d66` 重新开始时，第一张实现分支只做 **R1：严格配置、重复注册拒绝、最小 Diagnostic**。不要复用后续 Round 4 的整包实现，也不要在 R1 引入 Config Schema、Node Contract 大框架、Catalog、Visualizer、三阶段 Control、Sanitizer 重构或构建系统治理。

R1 独立验收通过后，再以一个业务完成 R2 试点。只有试点证明 API 对普通开发者确实更简单，才进入全量迁移。这样既保留原评审中真正关键的架构方向，也能避免再次偏离“方便配置、方便使用、方便业务开发”的初衷。

## 9. R1 实现验收（2026-08-21）

### 9.1 验收范围与结论

本轮验收对象为分支 `feat/pipeline-blackboard-rebaseline` 上、基线提交
`6ea4d66` 之后的 R1 实现。变更范围集中在严格配置解析、Pipeline
预检、Registry 冲突检测、结构化诊断和对应测试，没有提前引入 JSON
Schema、Catalog、typed blackboard key 或 R2/R3 的 Node Contract，范围控制符合本文件第
5 节的重新基线原则。

**最终结论：有条件通过，R1 尚未完全收口。**

配置解析和无副作用预检的主体设计已经成立，原有九份正式业务配置保持兼容，完整回归通过；但
Pipeline 的异常诊断边界和一次性构建生命周期仍存在两个必须收敛的问题。因此当前实现可以作为
R1 的继续整改基础，但在下列 P1 问题关闭前，不建议开始 R2 typed blackboard 试点，也不建议把
`PipelineDiagnostic` 作为稳定接口向业务开发者承诺。

### 9.2 已通过项

1. **配置事实源已集中。** 新增 `ParsePipelineConfig` 和
   `ParsedPipelineConfig`，`Pipeline::BuildFromJson` 不再分散读取原始 JSON 字段。
2. **严格配置校验主体成立。** 根对象、字段白名单、必填字段、类型、数量上限、重复
   model/node id、依赖不存在、自环和 DAG 成环均有明确拒绝路径。
3. **顺序配置兼容策略成立。** 未声明 `depends_on` 的 sequential 配置可继续省略 node
   id，由解析器生成稳定内部 id；九份正式配置均成功解析并构建。
4. **预检位于副作用之前。** Registry 冲突、未知 engine/node 和 DAG 错误均在
   Engine Load、Node Init 前完成检查。
5. **重复注册改为 fail-closed。** Node/Engine Registry 保留首次注册，不再由后注册者覆盖，
   Pipeline 构建时会拒绝已记录的注册冲突。
6. **本轮未扩大公共 C ABI。** Adapter 只消费 Pipeline 的结构化诊断；六个 C 导出函数及四层
   依赖方向未被改变。
7. **验证通过。** `git diff --check` 和 LayerGuard 通过；Release 构建成功；CTest
   `15/15` 通过；`./scripts/run_all_tests.sh` 六阶段回归全部通过。

按 R1 核心验收能力统计：8 项中 **6 项通过、1 项部分通过、1 项未通过**。部分通过项是
Diagnostic 已覆盖解析和预检，但未完整覆盖实例化异常；未通过项是一次性构建契约未被代码强制。

### 9.3 待整改问题清单

#### R1-ACC-001（P1）：构建异常没有被完整转换为稳定 Diagnostic

**状态：待整改。**

当前 `BuildFromConfigFile` 的 `try` 同时包住 JSON 读取和整个 `BuildFromJson`。因此
Engine 创建/加载或 Node 创建/初始化抛出的异常会被错误归类为 `kJsonParse`；直接调用
`BuildFromJson` 时，这些异常还会继续向调用者传播。文件无法打开、Engine Load 返回 false、
Node Init 返回 false 也分别复用了 `kJsonParse`、`kUnknownEngineType`、
`kUnknownNodeType`，错误码不能准确表达实际失败阶段。

这会破坏 R1 的核心目标：业务开发者和 AI 无法仅根据 `code + path + message` 确定应该修改
JSON、模型文件、Engine 还是 Node 配置。

**明确修复方案：**

1. 扩充但保持轻量的错误码，至少增加：`kConfigFileOpen`、`kEngineCreateFailed`、
   `kEngineLoadFailed`、`kNodeCreateFailed`、`kNodeInitFailed`、`kInternalException`；不要用
   unknown-type 错误表示一个已注册类型的运行时失败。
2. 缩小 `BuildFromConfigFile` 的 JSON `try-catch` 范围：只包住 `ifs >> root_json`；解析成功后
   在该 catch 之外调用 `BuildFromJson`，避免把下游异常伪装成 JSON 语法错误。
3. 在 `BuildFromJson` 的每个物化边界分别捕获 `std::exception` 和未知异常：
   Engine creator、`Load`、Node creator、`Init`。设置对应错误码和准确 JSON Pointer 后返回
   false，不能让异常穿过该 API。
4. 文件系统路径可以放入 message；`path` 应继续表示配置位置。文件打开失败可使用空 path 或
   约定的根路径 `/`，不要把文件系统路径伪装成 JSON Pointer。
5. 增加 ThrowingEngine/ThrowingNode 测试替身，分别覆盖 constructor、Load、constructor、
   Init 抛异常，以及 Load/Init 返回 false；逐项断言错误码、path 且 API 不抛异常。

**验收标准：** 对同一失败输入，直接调用 `BuildFromJson` 和经 `BuildFromConfigFile` 调用得到
相同阶段语义的错误码；任何 Engine/Node 实现异常都不会越过 Pipeline 构建 API。

#### R1-ACC-002（P1）：一次性 Build 契约只有文字假设，没有状态机保护

**状态：待整改。**

R1 的范围约定是“仅在新建、空的 Pipeline 上 Build，不实现 hot reload”。当前代码却没有
`Empty/Building/Ready/Failed` 状态或 `build_attempted_` 保护。第二次调用 Build 时，节点和拓扑会
被清理，但 `SessionContext::ModelManager` 中已注册模型不会清理：同一个配置可能在重复加载模型后
因重复 model id 失败，并留下新旧混合状态。无模型配置又可能成功重建，行为不一致。

**明确修复方案：**

1. R1 不实现事务式热重载；为 Pipeline 增加最小状态
   `Empty -> Building -> Ready` 或 `Failed`。
2. `BuildFromJson`/`BuildFromConfigFile` 应共用一个真正的构建入口，确保一次外部构建请求只进行
   一次状态转换；不能由二者互相调用时误判为第二次 Build。
3. 非 `Empty` 状态再次 Build 时，在任何解析、Load、Init 或旧状态清理前返回
   `kInvalidBuildState`。首次构建开始后，无论解析、预检或物化成功还是失败，都不可在原对象上重试；
   失败对象由调用方销毁。
4. `Execute` 和 `Control` 仅允许在 `Ready` 状态工作；`Empty/Building/Failed` 均返回明确失败，
   防止执行半成品 Pipeline。
5. 删除当前物化前“清理现有管线状态”所暗示的重建语义，或者明确注释该清理只用于首次构建失败时
   的析构安全，不能把它描述为 reload。
6. 增加三类测试：成功 Build 后第二次 Build 在零副作用前失败；物化失败后重试也失败；未 Ready
   或 Failed 状态下 Execute/Control 失败。

**验收标准：** 同一 Pipeline 实例在所有路径上最多接受一次构建尝试；失败后不存在可执行的半成品，
也不存在重复模型加载或新旧节点混合。

#### R1-ACC-003（P2）：sequential 模式仍接受 `max_parallel_workers`

**状态：待整改。**

解析器验证了 workers 的类型和范围，但没有验证它与 `execution_mode` 的组合。当前
`sequential + max_parallel_workers: 4` 会静默接受并忽略该字段，容易掩盖配置复制错误。

**修复方案：** 在解析完 execution mode 后做组合校验；sequential 出现该字段时返回
`kFieldRange`（或新增更准确的 `kInvalidCombination`），path 固定为
`/max_parallel_workers`。增加一个 integer 正例值但模式错误的负向测试；parallel 仍覆盖 1 和
64 的边界正例。

#### R1-ACC-004（P2）：Registry 在持锁期间执行外部 creator

**状态：待整改。**

`NodeFactory::Create` 和 `EngineFactory::Create` 持有 Registry mutex 时调用注册方提供的
`std::function`。若构造函数查询或注册同一个 Registry，可能自锁；耗时构造也会无必要地阻塞其他
查询。

**修复方案：** 在锁内只查找并复制 `CreatorFunc`，随即释放锁，再调用 creator。creator 抛出的
异常由 R1-ACC-001 所述 Pipeline 物化边界转换为 Diagnostic。增加一个构造期间调用 Registry
`Has` 的测试，证明不会死锁；测试应设置超时或使用可控 future，避免失败时永久挂住套件。

#### R1-ACC-005（P2）：测试清理接口暴露在生产头文件，并可能掩盖真实静态注册冲突

**状态：待整改。**

`ResetConflictForTesting`/`ClearForTesting` 作为公开方法出现在 Node/Engine Registry，
`ModelManager::ClearForTesting` 也进入生产头文件。测试 fixture 每次先清除冲突，可能把链接阶段真实
发生的重复静态注册一并擦除，使测试错误通过。

**修复方案：**

1. 删除未使用的 `ClearForTesting` 和 `ModelManager::ClearForTesting`。
2. 重复注册用例优先放进独立测试可执行文件，利用进程级 Registry 生命周期隔离，结束进程即可清理。
3. 若必须保留 reset，仅在测试编译宏下暴露，并在任何 reset 之前先断言正式静态注册表无冲突；不要
   在通用 fixture 的 SetUp/TearDown 无条件擦除全局证据。

#### R1-ACC-006（P2）：严格性与测试断言还有三个小缺口

**状态：待整改。**

- `comment` 被列入允许字段，但未验证其类型；非字符串会被静默忽略。
- `Pipeline::DagNodeMeta` 已被 `ParsedNodeConfig` 取代但仍保留，形成第二份过时的数据结构表达。
- 负向预检测试只断言 `load_count/init_count == 0`，没有断言 creator 未被调用，尚未完整证明
  “所有引用校验在 Create 之前完成”。

**修复方案：** 对允许出现的 `comment` 强制 string 类型（或从白名单移除）；删除未使用的
`DagNodeMeta`；为 CountingEngine/CountingNode 增加并断言 create count，所有解析和预检失败均应为
零。上述修复不需要引入 Schema，也不应增加新的业务开发概念。

### 9.5 第二轮整改与最终验收结果（2026-08-21）

已针对 9.3 节提出的 6 项问题完成深度整改与全面测试验证：

1. **R1-ACC-001（P1 状态：已解决/已关闭）**：
   - 扩充 `PipelineErrorCode`，细化出 `kConfigFileOpen`、`kEngineCreateFailed`、`kEngineLoadFailed`、`kNodeCreateFailed`、`kNodeInitFailed`、`kInternalException` 等精确错误码。
   - `BuildFromConfigFile` 缩小 try-catch 范围至仅捕获 JSON 反序列化，与 `BuildFromJson` 解耦。
   - 物化阶段为 Engine/Node 的 Create、Load、Init 建立严格异常隔离与错误码映射，无异常逃逸。
   - 增加 6 组异常/失败测试替身（`ThrowingCtorEngine`, `ThrowingLoadEngine`, `FailingLoadEngine`, `ThrowingCtorNode`, `ThrowingInitNode`, `FailingInitNode`）并逐项断言 Diagnostic。

2. **R1-ACC-002（P1 状态：已解决/已关闭）**：
   - 为 Pipeline 增加显式状态机 `State::kEmpty -> State::kBuilding -> State::kReady / State::kFailed`。
   - 非 `kEmpty` 状态下再次调用 Build 立即拒绝并返回 `kInvalidBuildState`，零副作用。
   - `Execute` 与 `Control` 仅在 `kReady` 状态下允许执行，未就绪或失败状态直接安全返回 `-1`。

3. **R1-ACC-003（P2 状态：已解决/已关闭）**：
   - 在 `ParsePipelineConfig` 中增加组合校验：sequential 模式下声明 `max_parallel_workers` 直接拒绝并返回 `kInvalidCombination`（path 为 `/max_parallel_workers`）。
   - parallel 模式下覆盖 1 和 64 边界测试。

4. **R1-ACC-004（P2 状态：已解决/已关闭）**：
   - `NodeFactory::Create` 和 `EngineFactory::Create` 调整锁粒度：锁内仅检索复制 creator 函数指针，锁外执行实例化。
   - 增加 `ReentrantNode` 构造期重入查询测试，使用带超时异步断言证明零死锁风险。

5. **R1-ACC-005（P2 状态：已解决/已关闭）**：
   - 移除 `ClearForTesting` 与 `ModelManager::ClearForTesting`。
   - 测试 fixture `SetUp()` 在运行前断言静态注册表零冲突。
   - 冲突测试后在独立新 Pipeline 实例上验证干净构建能力。

6. **R1-ACC-006（P2 状态：已解决/已关闭）**：
   - 根节点、Model、Node 级别的 `comment` 字段均强制要求 string 类型，非 string 拒绝并返回 `kFieldType`。
   - 清除 Pipeline 内部残留废弃的 `DagNodeMeta`。
   - 测试断言覆盖 `CountingEngine::create_count` 与 `CountingNode::create_count`，证明预检期零实例化副作用。

**最终验收结论：全部通过 (All Passed)。R1 阶段已彻底收口，满足基线交付标准。**
