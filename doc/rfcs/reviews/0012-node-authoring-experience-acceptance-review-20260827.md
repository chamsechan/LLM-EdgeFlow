# RFC-0012 Node 架构改造验收与整改指南（2026-08-27）

## 1. 验收结论

**结论：架构方向通过，当前实现不通过交付验收。RFC-0012 应保持
`In Implementation`，暂不合入 `main`。**

本次实现已经证明 26 个 Biz Node 可以迁移为 11 类 Common Node 和 Pipeline 配置的
组合。Node 的合理划分原则仍然是：

> **以 I/O 语义契约组织 Node，以单一操作语义定义 Node，以执行契约约束 Node；
> 业务差异优先进入配置和 Pipeline。**

不建议退回“一个业务一个 Node”的结构，也不应为了保持 Common Node 数量不变而把
不同操作或不同生命周期硬塞进同一个类。当前需要修复的是契约完整性、业务兼容性、
层次边界和测试覆盖，而不是推翻公共 Node 方向。

## 2. 验收候选与验证证据

| 项目 | 结果 |
| :--- | :--- |
| 分支 | `feat/node-authoring-experience` |
| 实现候选 | `f43493d5fb60addc87fe9c6ff3e62506ee274c59` |
| CMake 配置和编译 | PASS |
| CTest | PASS，37/37 |
| 11 个 Pipeline `validate` / `plan` | PASS |
| 9 个 smoke profile | PASS |
| `./scripts/run_all_tests.sh` | PASS，六阶段全部通过 |
| 格式与 `git diff --check` | PASS |
| 四层架构和 RFC 契约验收 | **FAIL** |
| 远端 PR / CI / main 合并 | NOT VERIFIED |

测试全部通过只说明已有断言没有失败，不能证明关键契约已被覆盖。本报告中的非法可选
端口绑定、空输入模板、Control 未处理、AudioAsrIntent 槽位回退等问题均未被现有测试
发现。

## 3. 已确认成立的设计

以下内容可以保留并继续演进：

1. 26 个业务专属 Node 已退出 Catalog，首批 Catalog 收敛为 11 类 Common Node；
2. 同一种 Node Type 可以在一个 DAG 中通过实例级端口绑定多次使用；
3. Pipeline JSON 已成为业务组合、Prompt、规则、TopK 和模型绑定的主要承载体；
4. Adapter 已开始直接完成 C ABI 与内部契约之间的输入输出映射；
5. Catalog、Validator、Planner、smoke 和现有业务 Pipeline 均能运行；
6. CPU、CUDA、AX650 等计算平台仍留在 Engine/Profile 层，没有进入 Node 分类。

因此，整改应在现有分支上增量完成，不需要恢复已删除的整套 Biz Node 目录。

## 4. 阻断问题与具体修改方案

### NA-001（P0）：Validator 放过显式绑定的 optional 端口

#### 现状

`PipelineValidator` 遇到 `required == false` 的输入端口后直接跳过检查。实测以下两类
非法配置均返回 `{"ok": true}`：

- 把 `TextTemplate.context: RankedTextBatch` 绑定到 `TextBatch` 输出；
- 删除 `TextTemplateNode` 的全部动态输入绑定。

#### 修改方案

1. 把“是否必须绑定”和“绑定后是否合法”拆开：
   - 未绑定时，仅 `required=true` 报错；
   - 一旦显式绑定，无论 required 与否，都必须检查生产者、输出端口、类型和依赖关系；
2. 在 `NodeDefinition` 增加可序列化的端口组合约束，建议至少支持：
   - `at_least_one_of`；
   - `exactly_one_of`；
   - `all_or_none`；
3. 为端口补充 cardinality 和 provenance policy，供 Validator、Studio 和运行时共同使用；
4. `TextTemplateNode` 声明至少一个动态输入；`TextRerankNode` 声明合法的 query、pair、
   candidate 组合，不在 Process 中静默猜测。

#### 必测用例

- optional 端口未绑定：按定义允许通过；
- optional 端口绑定到不存在的节点、端口或错误类型：必须失败；
- Template 无任何动态输入：必须失败；
- Rerank 缺 query 或 candidate：必须失败；
- CLI validate、plan、Pipeline Build 对同一 fixture 给出一致诊断。

### NA-002（P0）：AudioAsrIntent 对外 Operator 行为回退

#### 现状

原 `SlotExtractNode` 会输出导航目的地、避让选项、空调温度、风速等 `slots`。迁移后的
`TextRuleMatchNode` 只产生 intent/category、matched word 和 matches，smoke 输出已经
不再包含 slots；现有测试只断言 intent。

#### 修改方案

扩展规则节点的通用能力，而不是增加 `audio_asr_intent` 业务模式：

```json
{
  "rules": [
    {
      "id": "navigation",
      "strategy": "regex",
      "pattern": "导航到(?<destination>.+)",
      "category": "NAVIGATION",
      "score": 1.0,
      "constants": {"avoid_toll": false}
    }
  ]
}
```

`RuleMatchItem` 应能承载命名 captures、常量字段、rule id、category 和 score。支持的
strategy 必须是封闭枚举，例如 `contains|exact|regex`，Definition、Validator 和运行时
实现保持一致。

如果实现过程中发现“分类匹配”和“字段抽取”的 cardinality、失败语义明显不同，应新增
通用 `TextPatternExtractNode`，而不是让 `TextRuleMatchNode` 逐渐变成业务 DSL。节点数量
不是 KPI。

#### 必测用例

- 对迁移前的导航和 HVAC 输入做 golden test；
- intent、confidence、全部 slot 名称和值与旧 Operator 行为一致；
- 无匹配、多个匹配、regex 非法和缺失捕获字段均有明确行为。

### NA-003（P0）：Control 路由和返回语义不完整

#### 现状

`NodeControlStatus` 已存在，但 `NodeDefinition` 没有声明 supported commands；Pipeline
仍把命令发给全部 Node，所有节点都返回 Unsupported 时仍返回成功。未知命令返回成功
甚至被现有测试固定下来。

#### 修改方案

1. 在 `NodeDefinition` 增加 `ControlCommandDefinition`：命令 ID、名称、payload schema、
   是否支持热更新；
2. 使用具名命令常量，禁止在节点内散落 `cmd == 1/2/3`；
3. Pipeline 只向声明支持该命令的 Node 分发；
4. 聚合语义固定为：
   - 任一目标节点失败：Failure；
   - 至少一个节点处理且无失败：Handled；
   - 没有节点声明或处理：Unsupported；
5. Layer 1 将三种状态稳定映射到 C ABI 返回码，不把 Unsupported 伪装为成功。

#### 必测用例

- prompt 更新只命中模板/生成节点；
- rule 更新只命中规则节点；
- 阈值更新只命中声明支持阈值的节点；
- 未知命令返回 Unsupported；
- 多节点中任一失败时 Pipeline 返回失败。

### NA-004（P0）：结构化输出不具备结构、诊断和 fail-closed 语义

#### 现状

`StructuredDocumentBatch` 当前实际是 `TraceableItem<std::string>`。JSON 解析异常被吞掉
并替换为 fallback string，随后 Adapter 再次解析 JSON，并计算 SAFE、风险分数、默认
策略等业务结果。

#### 修改方案

1. 将公共值类型移到轻量 `include/contracts/`，至少定义：
   - 结构化 document/value；
   - provenance；
   - parse status；
   - diagnostics；
2. 将节点命名收窄为真实能力。若只处理 JSON，契约应明确是
   `JsonDocumentBatch`，不要用一个实际无法承载任意结构化格式的宽泛名称；
3. `StructuredJsonParseNode` 在 Init 编译并验证受限 schema、field mapping 和
   failure policy；Process 不得 catch-all 后无诊断返回成功；
4. `failure_policy` 使用封闭枚举，例如 `fail|emit_diagnostic|configured_fallback`；
5. fallback 的值和字段必须来自 Pipeline 配置并通过 schema 校验；
6. Adapter 只把已计算好的内部字段拷贝到 C ABI 输出，不在 Adapter 内决定 SAFE、
   HIGH_RISK、默认置信度、策略文案或 chunk 数量。

不建议为了消除 Adapter 逻辑而引入任意字段表达式 DSL。若某个输出决策确属不可配置的
领域计算，可以保留一个窄而明确的 Domain Node；这是 RFC 允许的例外。

#### 必测用例

- 合法 JSON、代码块 JSON、截断 JSON、类型错误、缺字段、schema 不匹配；
- 每种 failure policy 的返回值和 diagnostics；
- Adapter golden test 证明它只做映射，不补业务默认值。

### NA-005（P1）：NodeDefinition 尚未成为单一事实源

#### 现状

构造函数和 `Definition()` 同时维护逻辑端口名、类型字符串、默认模型 ID 和配置字段。
`BoundInput<T>` 已知道 C++ 类型，却仍要求手写 `"TextBatch"`，契约仍可能漂移。

#### 修改方案

1. Node 构造函数只接收实例身份，不再接收 Blackboard key、类型字符串或默认业务配置；
2. `NodeInitContext` 提供类型化 API，例如：

   ```cpp
   primary_ = ctx.BindInput<TextBatch>("primary");
   output_ = ctx.BindOutput<TextBatch>("text");
   ```

   类型名由 `BlackboardTypeTraits<T>` 推导，不由节点手写；
3. `Definition()` 是端口和配置 schema 的唯一声明位置。节点中引用逻辑端口名属于消费
   Definition，不得再声明第二套 required/type/default schema；
4. 模型能力、模型配置字段和默认值只定义一次，并由公共解析器产生已校验配置；
5. Catalog 注册时继续做 Definition 自校验，拒绝重复端口、未知类型和无效组合约束。

#### 必测用例

- Definition 类型与 `BindInput<T>` 不一致时，Init 必须失败；
- 重复端口、未知端口和未消费的必需配置触发结构化错误；
- Catalog JSON 与运行时实际绑定完全一致。

### NA-006（P1）：静态语料 embedding 每个请求重复计算

#### 现状

DialogueAudit 当前每次 Process 都执行：

```text
TextCorpusSource -> TextEmbedding -> VectorTopK
```

固定 policy corpus 因而被重复 embedding。它应属于 Session 生命周期，而非 Request
生命周期。

#### 修改方案

优先实现显式、可验证的资源生命周期，不要依赖节点自行猜测输入是否静态：

1. 为端口或资源声明增加 `lifetime=request|session`；
2. `TextCorpusSourceNode` 的配置语料声明为 session immutable；
3. `TextEmbeddingNode` 对 session 输入在 Init/Warmup 阶段计算一次，将带模型版本和
   corpus digest 的结果存入 `SessionContext`；
4. Process 只读取只读 embedding batch；模型或 corpus 热更新时显式失效缓存；
5. Validator 拒绝把 request 输入错误绑定为 session cache 输入。

若现有 Pipeline 生命周期无法安全表达预计算，可以新增一个语义明确的
`StaticEmbeddingCorpusNode`。其独立理由是资源所有权和执行生命周期变化，而不是业务名。

#### 必测用例

- 多次 Process 只调用一次静态 corpus embedding；
- query embedding 仍按请求执行；
- corpus/model 更新后缓存失效；
- 并发请求读取同一缓存时线程安全且结果一致。

### NA-007（P1）：TextTemplate 契约不完整

#### 现状

模板在 Init 中没有编译或验证；未知 placeholder 被静默保留；同一 `req_id` 的多个输入
可能互相覆盖；超长输出被静默截断；Definition 缺少 RFC 约定的 attributes 输入。

#### 修改方案

1. Init 将模板编译为只读 token/placeholder 计划；
2. placeholder 只能引用 Definition 声明的动态端口或 `values` 中的静态字段；
3. 未知 placeholder、非法模板和缺少必需变量在 Init 阶段失败；
4. 明确 join/cardinality 规则，以 `(req_id, sub_id)` 对齐，禁止使用
   `req_id -> single string` 覆盖多项数据；
5. 输出继承可追溯 provenance；
6. `overflow_policy` 使用封闭枚举，默认 fail-closed；只有显式配置时才允许 truncate；
7. 增加受限 `TextAttributesBatch` 输入，禁止把任意 JSON 对象作为模板运行时环境。

#### 必测用例

- 静态变量、动态变量、候选集合、attributes；
- 未知变量、缺变量、重复 `req_id`、多个 `sub_id`；
- Unicode 长度边界和每种 overflow policy；
- 模型聊天模板示例的精确 golden output。

### NA-008（P1）：Node 保存了生命周期不安全的 Plan 裸指针

#### 现状

`Pipeline::Build` 中的 `ValidatedPipelinePlan` 是局部变量，而 `NodeBase` 保存了其中
`ValidatedNodePlan` 的裸指针。Build 返回后该指针悬空，当前只是因为多数节点只在 Init
读取它而暂未暴露。

#### 修改方案

推荐移除 Node 中长期保存的 `plan_` 和公开 `Plan()`：

- `NodeInitContext` 只在 Init 调用期间有效；
- `BindInput/BindOutput` 将解析结果复制为节点自己持有的 typed key；
- 节点只保存运行所需的不可变配置和 typed binding，不保存整个 plan 指针。

如果确实需要运行期查询 plan，则 Pipeline 必须把 plan 作为成员完整持有；二者只能选
一个清晰的所有权模型，不能依赖局部对象地址。

同时删除或严格弃用默认返回成功的旧 `INode::Init(json, SessionContext*)` 兼容入口，避免
未实现新初始化契约的节点静默成功。

#### 必测用例

- Build 返回后反复 Process/Control；
- Pipeline move、destroy 和并发运行；
- ASan/UBSan 下不存在 plan 生命周期访问错误；
- 未实现必需 Init 的 Node 注册或构建失败。

### NA-009（P0）：公共 Node 契约测试缺失

#### 现状

除 TextRerank 外，大部分新增 Common Node 没有直接单元测试。本次最重要的非法配置、
失败语义和业务兼容行为因此没有进入回归门禁。

#### 修改方案

为每个 Common Node 建立独立 `tests/test_<node>.cpp` 并注册到 CMake，至少覆盖：

- Definition 与 Init 合法/非法配置；
- 输入、输出类型和 cardinality；
- provenance；
- 空输入、边界输入和失败策略；
- Control（若支持）；
- 同一类型多实例、并发和无请求级状态残留。

另增加三组跨层测试：

1. `pipeline_validation` 共享非法 fixture：Core、CLI validate、CLI plan、Pipeline Build
   使用同一输入并比较诊断；
2. 7 个既有 Operator 行为 golden test：迁移前后的外部字段和值保持一致；
3. Adapter purity test：禁止 Adapter 引入业务默认结果或执行规则/检索/解析策略。

### NA-010（P1）：RFC 状态、文档和门禁脚本漂移

#### 现状

RFC 在特性分支尚未验收和合入 main 时被标记为 `Completed`；README 和架构文档仍有
`src/business/`、旧 Biz Node 等描述；Layer 检查脚本仍 grep 已删除的 `src/biz`，目录
不存在时因 `|| true` 继续通过。

#### 修改方案

1. RFC 和索引保持 `In Implementation`，合入 main 后才标记 `Completed`；
2. 更新 README、architecture、developer guide 中的目录和 Node Catalog；
3. 重写 Layer 检查为基于当前真实目录和 include dependency 的 fail-closed 检查；
4. 为检查脚本增加自测：目标目录不存在、违规 include 和旧注册方式都必须失败；
5. 最终验收报告绑定同一个完整 commit SHA，不引用不在当前历史中的提交。

## 5. 推荐实施顺序

为减少返工，按以下顺序修改：

1. **Core 契约**：NA-001、NA-003、NA-005、NA-008；
2. **公共数据契约**：NA-004，以及 cardinality、provenance、lifetime 定义；
3. **Common Node 行为**：NA-002、NA-006、NA-007；
4. **Layer 1 净化与兼容恢复**：移除 Adapter 业务计算，补齐 7 个 Operator golden test；
5. **测试门禁**：NA-009，先写失败用例，再完成实现；
6. **文档与脚本**：NA-010；
7. **完整复验**：所有 P0/P1 关闭后再申请独立验收。

建议每一阶段独立 commit，示例：

```text
fix(pipeline): enforce typed optional bindings and port constraints
fix(control): route declared node commands with explicit status
refactor(contracts): add structured values diagnostics and provenance
fix(nodes): restore rule captures and template cardinality
perf(nodes): cache session-scoped corpus embeddings
test(nodes): add common node and operator compatibility matrices
docs(rfc): align node architecture lifecycle and gates
```

## 6. 最终验收标准

只有同时满足以下条件，RFC-0012 才可进入 Completed：

- NA-001～NA-010 的 P0/P1 全部关闭；
- 7 个既有 Operator 的可观察行为有 golden test 且无回退；
- 每个新增 Common Node 有直接 GoogleTest；
- 非法 optional binding、无输入 Template、未知 Control 命令均 fail-closed；
- Adapter 只负责 C ABI 解包、类型映射和结果打包；
- 静态 corpus embedding 不随每个请求重复执行；
- ASan/UBSan 不报告 Plan 生命周期问题；
- Catalog、Validator、Planner、Runtime 对 Definition 契约一致；
- 文档、目录、检查脚本与代码一致；
- 以下本地门禁全部通过：

  ```bash
  ./scripts/format.sh
  cmake -B build -G Ninja -DLLM_EDGEFLOW_USE_CCACHE=ON
  cmake --build build -j$(nproc)
  ctest --test-dir build -j$(nproc) --output-on-failure
  ./scripts/run_all_tests.sh
  git diff --check
  ```

- 在同一最终候选 SHA 上完成独立复审；
- 用户明确授权后完成远端 PR、CI 和 main 合并，再把 RFC 与索引更新为 `Completed`。

## 7. 最终架构判断

本次实现没有证明“公共 Node 路线错误”，相反，它证明了业务可以被较少的原子能力组合。
真正需要守住的是以下边界：

```text
业务差异              -> Pipeline 配置
可复用单一操作        -> Common Node
不可配置的领域原子计算 -> Domain Node（例外且需评审）
请求编排和类型校验    -> Pipeline / Validator
资源与模型生命周期    -> SessionContext / Engine
C ABI 映射             -> Adapter
```

最终目标不是把 Node 数量压到最低，而是让每个 Node 都具有稳定、可理解、可测试、可组合的
契约。按本报告完成整改后，当前 11 类 Common Node 可以成为合理的首批框架基线。
