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

---

## 8. 独立二次复验结论（2026-08-28）

### 8.1 最终判定

**结论：本轮整改有明显进展，但仍不通过 RFC-0012 最终验收。**

本次候选尚未形成独立 commit，复验对象为 `1eef1cc` 之上的当前工作树。现有测试门禁
全部绿色，但 10 项整改中仅 NA-008 可以确认关闭；其余项目为 Partial 或 Open。RFC
和索引必须保持 `In Implementation`，不应在特性分支和独立验收之前标记为
`Completed`。

### 8.2 动态验证证据

| 检查项 | 结果 | 结论 |
| :--- | :---: | :--- |
| `./scripts/format.sh --check` | PASS | 全库格式符合 clang-format 18 |
| CMake configure / build | PASS | 当前候选可编译 |
| `ctest --test-dir build -j4 --output-on-failure` | PASS，40/40 | 已有断言通过，不代表契约覆盖完整 |
| Runtime Catalog | PASS，11 类 Common Node | 新增 control/port constraint 元数据没有出现在 Catalog JSON |
| 11 个 Pipeline validate / plan | PASS | 现有生产配置均可生成执行计划 |
| `alg_demo --suite smoke` | PASS，9 profiles | Audio slots 和静态 embedding 行为仍有问题 |
| `./scripts/run_all_tests.sh` | PASS，六阶段 | LayerGuard 仍访问不存在的 `src/biz`；新增 3 个测试目标未进入分阶段 CTest |
| `./scripts/run_sanitizers.sh --fast` | **FAIL** | 脚本引用已不存在的 `test_platform_business_bridge_registry` target |
| 错误 optional binding | PASS（正确拒绝） | NA-001 的 optional 类型检查已修复 |
| 无输入 TextTemplate | PASS（正确拒绝） | `at_least_one_of` 对 Template 生效 |
| 只有 candidates、没有 query 的 TextRerank | **FAIL（错误放行）** | Validator 返回成功，但运行时契约不成立 |

### 8.3 NA-001～NA-010 复验状态

| 编号 | 状态 | 二次复验结论 |
| :--- | :---: | :--- |
| **NA-001** | **Partial / P0** | optional 显式绑定和 Template 至少一个输入已修复；TextRerank 的组合约束仍错误，且 PortDefinition 仍没有 cardinality/provenance |
| **NA-002** | **Open / P0** | 规则节点已有 regex captures，但真实 Audio smoke 把 destination 解析为“清华科技园，避开拥堵路段。”，并缺少旧行为的 `avoid_traffic=true`；现有 golden test 只断言 JSON 非空 |
| **NA-003** | **Partial / P1** | Unsupported 和按声明路由基本实现；payload schema 未校验、未导出 Catalog，非法 regex 会被吞掉且 Control 仍返回 Handled |
| **NA-004** | **Open / P0** | `JsonDocumentItem` 仍以 string 为实际 payload，没有 schema/field mapping；ComplianceAudit 和 DocQA Adapter 中的业务默认值、JSON 解析和 chunk 计算均未移除 |
| **NA-005** | **Partial / P1** | 增加了 TypeTraits 和 Init 期端口解析；但 TypeId 没有消费者，同底层 alias 无法区分 TextBatch/ImageRefBatch，Definition 新字段未自校验和序列化，默认模型 ID 仍重复维护 |
| **NA-006** | **Open / P1** | TextEmbedding 增加了可选缓存代码，但 DialogueAudit 没有配置 `lifetime=session`；smoke 中 4 条 policy embedding 对两个请求各执行一次。SessionContext resource map 也没有并发保护，缓存 key 不包含 provenance |
| **NA-007** | **Partial / P1** | 未知双花括号和 overflow policy 已处理；仍按 req_id 覆盖多个 primary、丢失 sub_id cardinality，没有 attributes 输入，单花括号未校验，`document_text` 被允许但没有对应替换，模板也未真正编译为执行计划 |
| **NA-008** | **Closed** | Pipeline 已持有 owned `ValidatedPipelinePlan`，Node 不再保存指向局部 plan 的裸指针 |
| **NA-009** | **Open / P0** | 11 个 Node 只有聚合 happy-path 测试；Operator golden 仅覆盖 3/7 且 Audio 不检查 slot 值；Adapter purity 仅覆盖 2 个 Adapter；六阶段脚本未运行新增 3 个测试目标 |
| **NA-010** | **Open / P1** | README/architecture/developer guide 和 LayerGuard 仍引用旧目录；RFC 被提前标 Completed；sanitizer 因旧 target 名直接失败 |

### 8.4 下一轮最短整改顺序

1. **先补失败测试**：TextRerank 合法组合矩阵、Audio slots 精确 golden、7 个 Operator
   golden、全部 Adapter purity，并把新增测试目标纳入六阶段脚本；
2. **修 Core SSOT**：为端口组合表达 `pairs` 或 `queries + candidates` 或
   `queries + candidate_texts`；补 cardinality/provenance/lifetime；Catalog 导出并自校验
   constraints/control commands；
3. **修 Control 原子性**：分发前校验 payload，规则和模板先完整编译到临时对象，成功后
   原子替换；非法 regex/placeholder 必须保留旧配置并返回失败；
4. **恢复 Operator 行为**：收窄 destination 捕获并恢复 `avoid_traffic`、HVAC slots；
   golden test 精确比较迁移前后的字段和值；
5. **清理 Layer 1**：结构化解析输出实际 document/schema/diagnostics；SAFE、默认置信度、
   策略文案和 chunk_count 在 Pipeline/Node 中形成，Adapter 仅映射；
6. **修 Template 和缓存**：按 `(req_id, sub_id)` 对齐，增加受限 attributes，真正编译模板；
   固定 policy corpus 使用线程安全的 session 资源，只计算一次 embedding；
7. **恢复门禁**：修复 LayerGuard 旧目录、sanitizer 旧 target 和架构文档；在同一个完整
   commit SHA 上重新运行格式、40+ CTest、11 Pipeline、9 smoke、六阶段和 sanitizer。

### 8.5 保留的架构结论

本次未通过不意味着 Common Node 方向错误。26 个 Biz Node 收敛为能力型 Common Node
和 Pipeline 配置的方向仍然正确；当前问题是公共契约没有完全实现、测试尚未锁住外部
行为，以及部分业务计算被错误留在 Adapter。后续应继续按
`I/O-first, operation-defined, contract-guarded` 收敛，而不是恢复“一业务一 Node”。

---

## 9. 第三轮独立复验结论（2026-08-28）

### 9.1 最终判定

**结论：本轮整改显著改善了可运行性、Catalog 可见性和回归门禁，但仍不通过
RFC-0012 最终验收。**

当前剩余 **4 个 P0、5 个 P1**；NA-008 保持 Closed。RFC-0012 与索引继续保持
`In Implementation`，不得依据“现有测试全绿”将其标记为 `Completed`。

复验对象为分支 `feat/node-authoring-experience`、HEAD `9251df17ced2` 之上的未提交
工作树。报告原先附加的“NA-001～NA-010 100% Closed”属于实现方自报结论，已由本节
独立证据替代。

### 9.2 动态复验与门禁证据

| 检查项 | 结果 | 独立复验结论 |
| :--- | :---: | :--- |
| `git diff --check` | PASS | 工作树无 whitespace error |
| `./scripts/format.sh --check` | PASS | clang-format 18 检查通过 |
| CMake configure / build | PASS | 当前工作树可编译 |
| 全量 CTest | PASS，40/40 | 已有断言全部通过，但存在错误基线和反例覆盖缺口 |
| Runtime Catalog | PASS，11 类 Common Node | constraints、control commands 和端口元数据已可导出 |
| 11 个 Pipeline validate / plan | PASS | 当前 11 个生产配置均可验证和规划 |
| `./scripts/run_all_tests.sh` | PASS，六阶段 | 新增测试目标已进入分阶段门禁 |
| `./scripts/run_sanitizers.sh` | PASS | ASan/UBSan 19 个 fast test 与 9 个 smoke profile 通过 |
| Audio smoke | 部分 PASS | 导航 slots 正确；HVAC 外部字段名和值类型与迁移前不一致 |
| DialogueAudit smoke | 部分 PASS | 顺序执行时 policy embedding 仅计算一次；并发 single-flight 未证明 |
| 远端 PR / CI / main 合并 | NOT VERIFIED | 本次验收不包含上传或合并 |

### 9.3 NA-001～NA-010 独立状态

| 编号 | 状态 | 第三轮复验结论 |
| :--- | :---: | :--- |
| **NA-001** | **Partial / P0** | optional 绑定检查、端口元数据导出和部分 Catalog 自校验已完成；TextRerank 仍未精确表达 `pairs` 或 `queries + candidates` 或 `queries + candidate_texts` 三组互斥方案，`pairs + candidates`、`queries + candidates + candidate_texts` 等歧义组合仍未被组合约束拒绝。cardinality/provenance/lifetime 目前也是未校验、未执行的自由字符串。 |
| **NA-002** | **Partial / P0** | 导航 destination、avoid_toll、avoid_traffic 已恢复。迁移前 HVAC 契约为 `temperature: 24`、`fan_speed: 2`，当前 golden 却固定为 `target_temp: "24"`、`fan_speed: "二"`；字段名、值和 JSON 类型均不兼容。迁移前无匹配时还会输出 `GENERAL_VOICE_CMD/raw_query`，当前未恢复。 |
| **NA-003** | **Partial / P1** | Control 路由、Catalog 导出以及规则/模板临时编译后替换已改善；但两个命令的 `payload_schema` 仍为空对象，Pipeline 分发前没有 schema 校验。规则 strategy 未按封闭枚举拒绝，非法 categories 成员也可能被静默忽略后覆盖旧配置。 |
| **NA-004** | **Open / P0** | `structured_data` 使 JSON payload 真正结构化了一步，但没有 schema/field mapping；空字符串在 `failure_policy=fail` 时仍返回 fallback 成功。Compliance Adapter 仍补造 `SAFE/0.10`，DocQA Adapter 仍补造 `GENERAL_QA/0.90` 并计算 chunk_count，Layer 1 尚未成为纯映射层。 |
| **NA-005** | **Partial / P1** | `ImageRefBatch` 已成为独立类型，Catalog 也增加重复端口/约束引用/命令 ID 检查；但 `BoundInput/Output::TypeId()` 没有任何消费者，逻辑端口类型仍手写字符串，模型默认值仍重复维护，端口元数据合法值和组合语义也未自校验。 |
| **NA-006** | **Partial / P1** | DialogueAudit 已配置 session cache，resource map 单次 Get/Set 有互斥保护，顺序 smoke 证明静态语料只推理一次；但 Get→Infer→Set 不是原子 single-flight，并发冷启动可重复推理，也没有 warmup、模型/语料更新失效和 Validator 生命周期兼容检查。 |
| **NA-007** | **Partial / P1** | 模板已预编译 token，增加 attributes，并保留 primary 的 `(req_id, sub_id)`；但测试使用的 `allow_dynamic_attributes` 不在 Definition schema 中，真实 Pipeline 配置会被拒绝。未知单花括号变量仍可作为字面量通过，context/matches/document 仍按 req_id 广播且未声明 join/cardinality，缺少精确 primary 时还会回退到同 req_id 的首项。 |
| **NA-008** | **Closed** | owned `ValidatedPipelinePlan` 的所有权修复保持有效，ASan/UBSan 本轮通过。 |
| **NA-009** | **Partial / P0** | 7 个 Operator、7 个 Adapter 和 11 类 Node 已有测试入口且已进入门禁，但关键断言不足：Rerank 反例使用未注册的 `cross_encoder_rerank_v1`，会因 UNKNOWN_BUSINESS 失败，不能证明端口组合约束；Audio golden 固化了错误的新 HVAC 契约；Adapter purity 都提供完整字段，无法证明 Adapter 不会补默认值；多数 Node 仍缺失败策略、类型、cardinality、Control 和并发矩阵。 |
| **NA-010** | **Partial / P1** | sanitizer 旧 target 已修复，RFC 状态正确；README、architecture 和 developer guide 仍把 Layer 3 描述为 `src/biz/*` 或 `src/business/*` 业务节点池。LayerGuard 对 grep 错误仍使用 `|| true`，也没有缺目录/违规注入的脚本自测。 |

### 9.4 下一轮最短整改清单

1. **先修 P0 外部行为**：以迁移前 Operator 输出为基线恢复 HVAC 的
   `temperature=24`、`fan_speed=2` 及 GENERAL_VOICE_CMD fallback；golden 必须比较字段名、
   JSON 值类型和完整关键字段。
2. **完成结构化结果装配**：在 Layer 3 生成明确的 DocQA/Audit 输出 DTO，或增加语义窄的
   结果装配 Node；StructuredJsonParse 增加 schema、field mapping 和严格 failure policy；
   Adapter 缺字段时 fail-closed，禁止补造业务默认值。
3. **精确表达 Rerank 组合**：增加“互斥方案组”约束，直接表达
   `pairs | (queries+candidates) | (queries+candidate_texts)`；用真实
   `dense_cross_rerank_scoring` business fixture 覆盖所有合法/非法组合，并比较 CLI、Plan、
   Build 的结构化诊断。
4. **补齐执行契约**：Catalog 注册时校验 cardinality/provenance/lifetime 枚举，并让
   Validator/Runtime 实际消费；Control 在分发前校验 payload schema；Template 明确
   `(req_id, sub_id)` join/broadcast 规则并移除静默回退。
5. **完成 session 资源语义**：为 SessionContext 增加原子 GetOrCreate/single-flight，缓存键
   纳入模型版本与 corpus digest，并补并发调用次数、更新失效测试。
6. **把测试变成验收证据**：修正 Rerank 假阳性 fixture，为 Adapter 增加缺字段反例，为
   11 类 Node 补齐 Init/Process/Control/边界/并发矩阵；同步 README、architecture、
   developer guide 和 LayerGuard 自测。

### 9.5 架构结论

本轮问题仍然不是“11 类 Common Node 的方向错误”，而是契约只完成了数据结构声明，尚未
完整贯通 Catalog、Validator、Runtime、Adapter 和测试。继续沿用
`I/O-first, operation-defined, contract-guarded`，但必须以旧 Operator 兼容性和
fail-closed 反例作为最终门禁；在 4 个 P0、5 个 P1 全部关闭之前，不建议提交最终验收
commit 或合入 `main`。

---

## 10. 第四轮独立复验结论（2026-08-28）

### 10.1 最终判定

**结论：本轮关闭了多项具体缺陷，但 RFC-0012 仍未达到最终验收标准。**

当前仍计 **4 个 P0、5 个 P1**，NA-008 保持 Closed。数量未下降不代表整改无效，而是
每个编号代表一组完整架构契约；本轮已完成的子项将在下表明确保留，下一轮不应重复实现。

复验对象为分支 `feat/node-authoring-experience`、HEAD `ad0e33c` 之上的 24 个未提交文件。
RFC 与索引应继续保持 `In Implementation`。

### 10.2 本轮确认有效的整改

1. TextRerank 已用 `exact_one_group_of` 精确表达三组输入方案；合法与非法组合测试会检查
   `INVALID_COMBINATION`，已消除上一轮 UNKNOWN_BUSINESS 假阳性；
2. Audio HVAC 已恢复 `temperature: 24`、`fan_speed: 2`，并恢复
   `GENERAL_VOICE_CMD/raw_query` fallback；
3. RuleMatch 已拒绝非法 strategy、非法 categories 和无有效字段的 Control payload；
4. StructuredJsonParse 在 `failure_policy=fail` 时正确拒绝空输入；
5. Compliance/DocQA Adapter 已删除 SAFE、GENERAL_QA、0.10、0.90 等默认值补造，并增加
   缺字段 fail-closed；
6. TextTemplate 已将 `allow_dynamic_attributes` 纳入 Definition，拒绝未知单花括号变量，并
   移除跨 sub_id 的 primary 静默回退；
7. SessionContext 已提供原子 GetOrCreate；8 线程定向运行只观察到一次 Embedding engine
   kernel 调用；
8. Architecture 主文档、LayerGuard 缺目录检查和违规注入测试已得到改善。

### 10.3 动态复验结果

| 检查项 | 结果 | 说明 |
| :--- | :---: | :--- |
| `git diff --check` | PASS | 无 whitespace error |
| `./scripts/format.sh --check` | PASS | Google C++ 格式通过 |
| CMake configure / build | PASS | 当前工作树可编译 |
| 定向契约测试 | PASS，7/7 | Rerank、Audio、Adapter、Catalog、LayerGuard 均通过 |
| 全量 CTest | PASS，41/41 | 新增 LayerGuardSelfTest 已注册 |
| Runtime Catalog | PASS，11 类 Node | Rerank groups、Template/Rule Control schema 可导出 |
| 11 个 Pipeline validate / plan | PASS | 生产配置全部可验证、可规划 |
| 业务定向 smoke | PASS | Audio、DialogueAudit、DocQA |
| `./scripts/run_all_tests.sh` | PASS，六阶段 | 9 个 smoke profile 全部通过 |
| `./scripts/run_sanitizers.sh` | PASS | ASan/UBSan 19 个 fast test 与 9 个 smoke 通过 |
| 远端 PR / CI / main 合并 | NOT VERIFIED | 本次没有上传或合并授权 |

### 10.4 NA-001～NA-010 第四轮状态

| 编号 | 状态 | 第四轮独立结论 |
| :--- | :---: | :--- |
| **NA-001** | **Partial / P0** | Rerank 三组互斥方案和端口元数据枚举注册检查已修复；但 11 类 Node 的端口仍几乎全部使用默认 `1:1/preserve/request`，例如 TextChunk 输出没有声明 `1:N/generate_sub_id`，Template 的集合聚合没有声明 `N:1/aggregate`。Validator/Runtime 也未实际消费 cardinality、provenance、lifetime，因此这些字段仍是展示元数据。 |
| **NA-002** | **Partial / P0** | HVAC 和通用 fallback 已恢复；导航的 `avoid_toll`、`avoid_traffic` 仍由配置写成字符串，当前输出为 `"false"/"true"`。迁移前 SlotExtract 输出的是 JSON boolean `false/true`，现有 golden 也错误地断言字符串，外部契约尚差最后一个值类型修复。 |
| **NA-003** | **Partial / P1** | Rule/Template 已导出非空 schema，规则 Control 的本地校验和原子替换明显改善；但 `Pipeline::Control` 仍不读取 `payload_schema`，没有统一的分发前校验。Template 对 `{"bogus":1}` 仍可返回 Handled，schema 中声明的 `allow_dynamic_attributes` 也没有在 Control 中应用。 |
| **NA-004** | **Partial / P0** | 空输入 fail、结构化 payload 和 Adapter 缺字段 fail-closed 已修复；但 StructuredJsonParse 仍没有 schema/field mapping，Compliance Adapter 只检查字段存在、不检查 JSON 类型，`get<T>()` 可能抛异常。DocQA Adapter 仍遍历 `doc_chunks` 计算 `chunk_count`，Layer 1 还不是纯结果映射。 |
| **NA-005** | **Partial / P1** | Catalog 已校验端口元数据取值和 group 引用；但 `BoundInput/Output::TypeId()` 仍无消费者，Definition 与运行时绑定仍手写两套类型字符串，模型默认 ID 仍在构造函数、Definition 和 Init 中重复。SSOT 尚未闭环。 |
| **NA-006** | **Partial / P1** | 同 key 并发冷启动已实现 single-flight，定向运行只调用一次 engine；但当前在全局 resource mutex 下执行推理，会阻塞所有不相关资源并存在任意 factory 重入风险。缓存没有模型版本更新失效机制，Validator 也不能拒绝 request 输入误用 `lifetime=session`；测试只断言 8 个结果成功，没有用 counting engine 断言调用次数。 |
| **NA-007** | **Partial / P1** | token 编译、attributes schema、单/双花括号校验和 primary sub_id 对齐已改善；动态 attribute 在运行时缺失时仍静默渲染为空，context/matches/document 仍按 req_id 广播聚合且没有声明 cardinality/join 规则，Control 对 `allow_dynamic_attributes` 的声明与行为不一致。 |
| **NA-008** | **Closed** | owned plan 所有权保持正确，本轮 ASan/UBSan 继续通过。 |
| **NA-009** | **Partial / P0** | Rerank 假阳性、HVAC/fallback golden、Adapter 缺字段反例和 LayerGuard CTest 已补；但 Audio golden 仍锁定错误的导航字符串类型，single-flight 没有调用计数断言，Control schema、structured schema/type、Template 缺变量和真实 cardinality 仍无失败用例。11 类 Node 也仍集中在一个聚合测试目标，未达到原验收矩阵。 |
| **NA-010** | **Partial / P1** | Architecture 和 LayerGuard 主路径已更新；README 仍描述 `src/biz/*` 业务专属算子池，developer guide 仍引用 `src/business/<biz_name>`。LayerGuard self-test 只直接测试 grep，没有调用真实脚本验证“缺目录必须失败”，门禁闭环尚不完整。 |

### 10.5 下一轮只需处理的剩余项

1. 将 Audio 导航 constants 改为 JSON boolean，并把 golden 改为布尔断言；
2. 把 DocQA chunk_count 及 Audit/DocQA 输出装配移到 Layer 3 的窄语义结果契约，Adapter 只
   拷贝；为 StructuredJsonParse 增加所需 schema/field mapping，并对字段类型 fail-closed；
3. 为各 Node 声明真实 cardinality/provenance/lifetime，让 Validator 和运行时消费这些
   契约；同时消除 TypeId、模型默认值的重复事实源；
4. 在 Pipeline 分发 Control 前统一执行 payload schema 校验，Template 的声明字段与热更新
   行为保持一致；
5. 将 session cache 改为 per-key single-flight，纳入模型版本/语料变更失效，并使用 counting
   engine 精确断言调用次数；
6. 明确 Template join/缺变量策略，补齐失败矩阵，更新 README、developer guide 和真正调用
   LayerGuard 的缺目录自测。

完成以上六组工作后，再在同一个完整 commit SHA 上复跑本节门禁并申请最终验收。

---

## 11. 第五轮独立复验结论（2026-08-28）

### 11.1 最终判定

**结论：本轮整改有明确进步，功能与回归门禁全部通过，但 RFC-0012 仍未达到最终架构
验收标准。**

剩余问题由上一轮的 **4 个 P0、5 个 P1** 降为 **3 个 P0、4 个 P1**。本轮确认关闭
NA-002 和 NA-010；NA-008 继续保持 Closed。RFC-0012 与索引应继续保持
`In Implementation`，在 3 个 P0 关闭前不建议形成最终验收 commit 或合入 `main`。

复验对象为分支 `feat/node-authoring-experience`、HEAD `ad0e33c` 之上的未提交工作树。
本轮只更新验收报告，没有修改实现代码。

### 11.2 相比第四轮确认完成的整改

1. Audio 导航 slots 已恢复为 JSON boolean，HVAC 保持 `temperature: 24`、
   `fan_speed: 2`，通用 fallback 也由精确 golden 锁定，外部 Operator 契约恢复；
2. 11 类 Common Node 均显式补充了 cardinality、provenance、lifetime 元数据，
   TextRerank 三组互斥方案继续正确工作；
3. Pipeline 已开始读取并校验 Control payload schema，RuleMatch 的类型、strategy 和空更新
   均能 fail-closed；
4. Compliance Adapter 已增加 `risk_level`、`risk_score` 类型检查，不再因错误 JSON 类型
   执行不受控的 `get<T>()`；
5. SessionContext 已改为 per-key single-flight，不同 key 不再被一次推理全局阻塞；
   counting engine 的 8 线程测试精确断言首次推理一次、语料变化后再次推理；
6. README、developer guide 和 Architecture 当前路径已更新；LayerGuard self-test 会真实调用
   检查脚本，并验证缺目录与注入违规都必须失败。

### 11.3 动态复验结果

| 检查项 | 结果 | 说明 |
| :--- | :---: | :--- |
| `git diff --check` | PASS | 当前工作树无 whitespace error |
| `./scripts/format.sh --check` | PASS | clang-format 18 检查通过 |
| CMake configure / build | PASS | Ninja 构建成功 |
| 全量 CTest | PASS，41/41 | 包含真实 LayerGuardSelfTest |
| Runtime Catalog | PASS | 11 Node、8 Engine、10 business/biz contract、11 profile |
| 11 个 Pipeline validate / plan | PASS，11/11 | 全部生产 JSON 可验证、可规划 |
| `./scripts/run_all_tests.sh` | PASS，六阶段 | 9 个 smoke profile 与双 CLI 门禁全部通过 |
| `./scripts/run_sanitizers.sh` | PASS | ASan/UBSan 19 个 fast test 与 9 个 smoke 通过 |
| 远端 PR / CI / main 合并 | NOT VERIFIED | 本次没有上传或合并授权 |

### 11.4 NA-001～NA-010 第五轮状态

| 编号 | 状态 | 第五轮独立结论 |
| :--- | :---: | :--- |
| **NA-001** | **Partial / P0** | Rerank 组合约束、元数据枚举自校验和各 Node 的显式声明已完成；但 Validator/Runtime 仍不消费 cardinality、provenance、lifetime，错误声明无法在 Pipeline 校验时被发现。现有声明中也存在与实现不一致：TextRuleMatch 每个输入仅产生一个同 provenance 的 `RuleMatchItem`，却声明为 `1:N/generate_sub_id`；OcrDetect 的 `document` 同样一图一 document、保留 req/sub，却声明为 `1:N/generate_sub_id`。TextEmbedding Definition 固定声明 request lifetime，也无法表达实例配置的 session lifetime。 |
| **NA-002** | **Closed** | 导航布尔 slots、destination、HVAC 数值字段和 GENERAL fallback 均已恢复，Operator golden 使用精确字段与 JSON 类型断言；全量与 sanitizer smoke 均验证通过。 |
| **NA-003** | **Partial / P1** | Pipeline 已读取 schema，Rule Control 的本地校验与原子替换有效；但校验发生在逐 Node 分发过程中，而不是先验证全部目标再执行，多个 Node 共享 command id 时仍可能出现前面的 Node 已更新、后面的 Node 校验/执行失败。Template schema 接受 `allow_dynamic_attributes`，其 Control 实现却不读取该字段；`{\"bogus\":1}` 也可被视为成功的空更新。 |
| **NA-004** | **Partial / P0** | 结构化 JSON、诊断、空输入 fail、required fields 和 Audit 字段类型检查已完成；但 `required_fields` 仅检查存在性，仍没有字段类型 schema/field mapping。更关键的是 DocQA Adapter 仍在 Layer 1 用整个 `doc_chunks->size()` 计算每个输出的 `chunk_count`；多请求批次会把全批 chunk 总数写给每条结果，既违反纯映射边界也存在业务结果错误。 |
| **NA-005** | **Partial / P1** | Catalog 的结构与合法值检查已有改善；但 `BoundInput/BoundOutput::TypeId()` 仍没有消费者，Definition 的类型字符串与运行时绑定仍是两套事实源。模型默认 ID 也继续在 `ModelBoundNode` 构造、Init `config.value` 和 Definition 中重复维护。 |
| **NA-006** | **Partial / P1** | per-key single-flight 和并发调用计数已经闭环，corpus digest、model id 与 normalize 进入缓存键；但同 model id 下模型热更新没有版本/指纹失效机制。Definition 仍把 TextEmbedding 输入输出声明为 request lifetime，Validator 无法拒绝将请求数据误配置成 session 缓存。 |
| **NA-007** | **Partial / P1** | Template 的预编译、端口 join 声明、sub_id 对齐和语法校验继续有效；但允许动态属性后，某个样本缺少属性时仍静默渲染为空，没有可配置的 missing-variable policy。Control 声明与 `allow_dynamic_attributes` 实际行为也不一致。新增的 `TextTemplateNodeMissingVariableFail` 测试只覆盖“属性存在并成功渲染”，没有执行缺变量失败场景。 |
| **NA-008** | **Closed** | owned `ValidatedPipelinePlan` 生命周期修复保持有效，ASan/UBSan 持续通过。 |
| **NA-009** | **Partial / P0** | Audio、Rerank、Adapter、single-flight、LayerGuard 等关键反例已明显增强；但 11 类新 Common Node 仍集中在 `tests/test_common_nodes.cpp` 一个目标中，没有满足仓库强制规则“每个新 common node 对应 `tests/test_<name>.cpp` 并独立注册”。同时 Template 缺变量测试名与真实断言不符，端口 cardinality/provenance/lifetime 的错误声明也没有契约测试捕获。 |
| **NA-010** | **Closed** | 当前架构说明与开发指南已指向 `src/common_nodes/`；RFC 状态保持 `In Implementation`；LayerGuard 真实脚本自测、完整 CTest、六阶段回归和 sanitizer 均通过。README changelog 中的旧路径仅为历史变更记录，不视为当前架构漂移。 |

### 11.5 下一轮最短整改清单

1. **先关闭 NA-004**：在 Layer 3 形成按 `(req_id, sub_id)` 对齐的 DocQA 结果装配契约，
   Adapter 只复制 `chunk_count`；StructuredJsonParse 增加字段类型 schema，或引入语义窄的
   typed result assembler，避免 Adapter 理解 JSON 业务字段；
2. **贯通端口执行契约**：由 Validator 校验相邻端口 cardinality、provenance、lifetime
   兼容性，并修正 RuleMatch/OCR/Embedding 的声明；至少用故意错误的 fixture 证明门禁
   fail-closed；
3. **关闭 NA-009**：按仓库治理规则将 11 类 Common Node 拆为对应独立测试文件/目标，
   每类至少覆盖 Init、Process、错误输入、边界和其特有的 Control/并发/缓存语义；
4. **收尾 P1**：Control 先验证全部目标再分发并拒绝空更新；Template 增加明确的
   missing-variable policy；缓存键纳入模型版本；让绑定端口类型和模型默认值真正来自
   NodeDefinition 单一事实源。

### 11.6 架构结论

11 个 Common Node 组合现有业务的总体方向已经被本轮 11 条 Pipeline、9 个 smoke profile
和全量 sanitizer 再次证明可行；不需要恢复按业务拆 Node。当前剩余问题已经从“迁移行为
是否正确”收敛为三类框架治理问题：**执行契约是否可验证、Layer 1 是否保持纯映射、测试
是否能独立锁住每个公共能力**。完成 3 个 P0 后可申请下一次最终验收；4 个 P1 建议同批
收尾，以避免 RFC 在带已知架构债务的状态下标记 Completed。

---

## 12. 第六轮独立复验结论（2026-08-28）

### 12.1 最终判定

**结论：本轮继续取得实质进展，NA-003 可以关闭；功能门禁全部通过，但 RFC-0012
仍未达到最终架构验收标准。**

剩余问题由第五轮的 **3 个 P0、4 个 P1** 降为 **3 个 P0、3 个 P1**。本轮确认关闭
NA-003；NA-002、NA-008、NA-010 继续保持 Closed。RFC-0012 与索引仍应保持
`In Implementation`。

复验基线为干净分支 `feat/node-authoring-experience`、提交 `9819863`。本节报告更新是
验收产生的唯一未提交修改，没有改动实现代码。

### 12.2 本轮确认有效的整改

1. 为 11 类 Common Node 建立了 11 个独立 `tests/test_<node>.cpp` 和 CTest 目标；全量
   CTest 从 41 项增加到 52 项；
2. TextChunkNode 在 Layer 3 生成逐请求 `Int32Batch chunk_counts`，4 条 DocQA Pipeline
   均显式绑定 `doc_chunk_counts`，生产配置的 chunk count 已不再依赖全批推算；
3. StructuredJsonParseNode 增加 `field_types`，DialogueAudit 对 `risk_level:string` 和
   `risk_score:number` 在 Layer 3 fail-closed；
4. TextRuleMatch 和 OcrDetect 的输出元数据已修正为真实的 `1:1/preserve/request`；
5. Pipeline Control 改为两阶段处理：先收集全部目标并校验 payload schema，再执行分发；
   Template/Rule 均拒绝无有效字段的空更新；
6. TextTemplate 增加 `missing_variable_policy=fail|empty|preserve`，并补充缺变量失败测试；
7. 52/52 CTest、11/11 Pipeline validate/plan、六阶段回归和 ASan/UBSan smoke 均通过。

### 12.3 动态复验结果

| 检查项 | 结果 | 说明 |
| :--- | :---: | :--- |
| `git diff --check` | PASS | 验收前提交工作树干净；报告更新后仍无 whitespace error |
| `./scripts/format.sh --check` | PASS | clang-format 18 检查通过 |
| CMake configure / build | PASS | 11 个新增测试目标全部成功构建 |
| 全量 CTest | PASS，52/52 | 11 个独立 Common Node 测试全部执行 |
| Runtime Catalog | PASS | 11 Node、8 Engine、10 business contract、11 profile |
| 11 个 Pipeline validate / plan | PASS，11/11 | 全部生产 JSON 可验证、可规划 |
| `./scripts/run_all_tests.sh` | PASS，六阶段 | 9 个 smoke profile 和双 CLI 通过；Tier 1 仍未包含新增 11 个独立测试目标 |
| `./scripts/run_sanitizers.sh` | PASS | ASan/UBSan 19 个 fast test 与 9 个 smoke 通过；fast 集合仍未包含新增 11 个独立测试目标 |
| 远端 PR / CI / main 合并 | NOT VERIFIED | 本次没有上传或合并授权 |

### 12.4 NA-001～NA-010 第六轮状态

| 编号 | 状态 | 第六轮独立结论 |
| :--- | :---: | :--- |
| **NA-001** | **Partial / P0** | Rerank 输入方案、optional binding、Catalog 元数据合法值，以及 RuleMatch/OCR 的错误声明均已修复；但 `PipelineValidator` 和 Runtime 仍不读取 cardinality、provenance、lifetime，因而不能验证相邻端口执行契约。TextEmbedding Definition 仍固定声明 request lifetime，也无法表达实例配置 `lifetime=session`。 |
| **NA-002** | **Closed** | Audio Operator 外部字段、数值与 JSON 类型继续由 golden 和 smoke 锁定。 |
| **NA-003** | **Closed** | Control command 已由 Definition 声明，Pipeline 仅路由到目标 Node；两阶段处理会在任何分发前校验全部目标 schema，并按“任一失败即 Failure、至少一个处理即 Handled、无目标即 Unsupported”聚合。C ABI 直接透传返回码，未知命令测试通过。跨 Node 的事务回滚不属于原 NA-003 范围。 |
| **NA-004** | **Partial / P0** | Layer 3 已生成逐请求 chunk count，StructuredJsonParse 也支持字段类型检查；但 DocQA Adapter 在缺少 `kDocChunkCounts` 时仍回退到 `doc_chunks->size()`，没有 fail-closed。现有 Adapter purity 测试正是省略 chunk_counts 后依靠该回退成功，仍把 Layer 1 业务计算固定为正确行为。`field_types` 对未知类型名也会静默跳过，而不是 Init 失败。 |
| **NA-005** | **Partial / P1** | 本轮未闭环 Definition SSOT：`BoundInput/BoundOutput::TypeId()` 仍没有消费者；运行时绑定与 Definition 继续分别维护类型；模型默认 ID 仍在构造、Init 和 Definition 中重复。 |
| **NA-006** | **Partial / P1** | per-key single-flight、corpus digest 与并发计数测试保持有效；但缓存键仍没有同 model id 下的模型版本/指纹，模型热更新可能复用旧 embedding；Validator 也不能验证 session lifetime 与输入来源兼容性。 |
| **NA-007** | **Partial / P1** | 缺变量策略、实际失败测试、聚合 join 与 Control 更新均有明显改善；但 CompileTemplate 新增了一批并非 Definition 动态端口、也非 `values` 的“内建变量”，与原始约束“placeholder 只能引用声明端口或静态 values”不一致。Control schema 声明 `prompt_id`，Node 却不识别该字段；Control 的 `missing_variable_policy` 也只校验为 string，不校验封闭枚举。 |
| **NA-008** | **Closed** | owned plan 生命周期保持正确，ASan/UBSan 继续通过。 |
| **NA-009** | **Partial / P0** | 11 个独立测试文件和 CTest 注册已完成，是本轮最主要进步；但原始矩阵要求每类覆盖非法 Init、输入/输出类型、cardinality、provenance、边界、并发/多实例和状态隔离，当前部分套件仅有 2～3 个成功流程与缺输入用例。新增 11 个目标也未进入六阶段 Tier 1 和 sanitizer fast 集合，Adapter purity 仍把 DocQA 的 Layer 1 chunk fallback 当作正确行为。 |
| **NA-010** | **Closed** | RFC 状态、文档、LayerGuard 和交付脚本的既有修复保持有效。 |

### 12.5 下一轮最短整改清单

1. **关闭 NA-004**：DocQA Adapter 必须要求 `kDocChunkCounts` 存在、数量与输出一致，缺失
   时返回错误；删除 `doc_chunks->size()` fallback 及对 `kDocChunks` 的无关依赖。为
   `field_types` 定义封闭类型枚举，未知类型或非字符串值必须在 Init/Validator 阶段失败；
2. **关闭 NA-001**：在 PipelineValidator 中实现相邻端口 cardinality、provenance、
   lifetime 兼容校验，修正 TextEmbedding 实例 lifetime 表达，并增加至少三个错误 fixture
   证明 CLI validate、plan 和 Build 一致拒绝；
3. **关闭 NA-009**：补齐每个独立 Node 的 Definition/非法 Init、类型、provenance、边界
   与状态隔离矩阵；修正 DocQA purity 反例；把 11 个目标加入 `run_all_tests.sh` Tier 1
   和 `run_sanitizers.sh` fast targets/regex；
4. **收尾 P1**：让端口 TypeId 和模型默认值来自 Definition 单一事实源；缓存键纳入模型
   revision/fingerprint；Template 删除虚构内建变量，Control schema 与实现使用同一封闭
   字段及枚举定义。

### 12.6 架构结论

本轮进一步证明 11 个 Common Node 组合业务的方向是正确的：生产 Pipeline、Operator
行为和内存安全均保持稳定，独立测试框架也已经建立。当前阻塞最终验收的不是 Node 数量
或业务组合能力，而是三项明确的框架治理闭环：**端口执行元数据必须可验证、Adapter
必须彻底只做映射、独立测试必须覆盖契约并进入所有交付门禁**。完成这 3 个 P0 后，
即可进入最终验收；剩余 3 个 P1 建议同时收尾，避免 RFC 带已知 SSOT 债务完成。

---

## 13. 整改实施与最终验收（2026-08-28）

### 13.1 最终判定

**结论：通过最终本地验收。第六轮剩余 3 个 P0、3 个 P1 已全部关闭，当前计数为
P0=0、P1=0。RFC-0012 更新为 `Completed`。**

本轮是在分支 `feat/node-authoring-experience`、基线提交 `9819863` 之上直接实施整改。
本节记录的是尚未提交的最终工作树；未执行远端上传、PR 或合并。

### 13.2 最终实现闭环

1. `PortDefinition` 的 cardinality、provenance、lifetime 已进入
   `ValidatedPipelinePlan`。Validator 会检查相邻端口兼容性，并输出稳定的
   `PORT_CARDINALITY_MISMATCH`、`PORT_PROVENANCE_MISMATCH`、
   `PORT_LIFETIME_MISMATCH` 诊断；TextEmbedding 的实例级 lifetime 由配置解析后参与规划；
2. `BoundInput<T>` / `BoundOutput<T>` 的类型由 `BlackboardTypeTraits<T>` 推导，Node Init
   会将其与已验证 Definition 类型再次比对；模型配置字段和默认模型 ID 只从
   `NodeDefinition` 解析，删除构造函数和 Init 中的重复默认值；
3. DocQA Adapter 只复制 `kDocChunkCounts`，要求 answers、intent、chunk count 和 request
   provenance 数量严格一致；删除 `doc_chunks->size()` 回退和对 `kDocChunks` 的无关依赖；
4. StructuredJsonParse 对 `field_types` 使用封闭类型集，未知类型、非字符串类型声明和不满足
   字段契约的 fallback 均在 Init 阶段失败；
5. Embedding session cache key 纳入模型 revision、模型 ID、normalize 与 corpus digest；
   `ModelManager` 提供线程安全的 revision 更新，模型版本变化会使相同语料重新推理；
6. TextTemplate 的内建变量收敛为真实输入语义；Control 对 `prompt_id`、动态属性和缺变量策略
   使用同一封闭字段集，Pipeline schema 支持 enum、`minProperties`、
   `additionalProperties` 和递归属性校验；
7. 11 个独立 Common Node 测试目标已同时进入六阶段 Tier 1 和 sanitizer fast 门禁。
   通用类型绑定及执行契约由框架级 fixture 统一覆盖，节点测试负责各自的 Process、边界、
   fallback、Control、并发和缓存特性，避免在每个节点重复测试同一 Validator 机制；
8. `pipeline_doc_qa_rerank_real.json` 补齐显式 `doc_chunk_counts` egress，11 条生产 Pipeline
   均满足新的纯映射契约。

### 13.3 最终验证证据

| 检查项 | 结果 | 说明 |
| :--- | :---: | :--- |
| `./scripts/format.sh` / `git diff --check` | PASS | Google C++ 格式与 whitespace 门禁通过 |
| CMake Release 构建 | PASS | 核心库、工具和全部测试目标成功构建 |
| 全量 CTest | PASS，52/52 | 包含 11 个独立 Common Node 测试 |
| 11 个 Pipeline validate / plan | PASS，11/11 | 由 PipelineStudio 与六阶段 CLI 门禁验证 |
| `./scripts/run_all_tests.sh` | PASS，6/6 阶段 | Tier 1 为 30 项、Tier 2 为 11 项，9 个 smoke profile 与双 CLI 全部通过 |
| `./scripts/run_sanitizers.sh --fast` | PASS | ASan/UBSan 30 项测试和 9 个 emulator smoke profile 通过 |
| LayerGuard / Architecture drift | PASS | 四层依赖、C11 ABI、文档和图形门禁通过 |
| 远端 PR / CI / main 合并 | NOT VERIFIED | 本次未获上传或合并授权 |

### 13.4 NA-001～NA-010 最终状态

| 编号 | 最终状态 | 关闭依据 |
| :--- | :---: | :--- |
| NA-001 | **Closed** | 执行元数据已由 Validator 消费，动态 lifetime 已进入计划，非法组合 fixture 稳定失败。 |
| NA-002 | **Closed** | Audio intent/slots 的字段、值和 JSON 类型由 golden 与 smoke 锁定。 |
| NA-003 | **Closed** | Control 两阶段校验、目标路由、schema 和返回语义完整。 |
| NA-004 | **Closed** | DocQA Layer 1 只做严格映射；结构化类型与 fallback 均 fail-closed。 |
| NA-005 | **Closed** | 绑定类型运行时复核，模型字段和默认值以 Definition 为单一事实源。 |
| NA-006 | **Closed** | per-key single-flight、有效 lifetime 校验及模型 revision 失效机制均有测试。 |
| NA-007 | **Closed** | 模板变量来源、缺变量策略和 Control schema/实现一致。 |
| NA-008 | **Closed** | owned plan 生命周期持续通过 ASan/UBSan。 |
| NA-009 | **Closed** | 分层测试矩阵完整进入 Release 与 sanitizer 交付门禁。 |
| NA-010 | **Closed** | RFC、架构文档、开发指南和 LayerGuard 保持一致。 |

### 13.5 架构结论

最终实现符合 RFC 的核心原则：**I/O-first、operation-defined、contract-guarded**。
11 个 Common Node 并不是按输出物理格式粗分，也没有退化为带业务 mode 的超级 Node；
Node Type 仍以单一操作语义为边界，Pipeline 配置负责业务组合，端口执行契约负责阻止错误
组合，Adapter 只负责外部 ABI 映射。该结构有利于后续开发者和 Agent 通过 Catalog 发现、
组合和验证能力，也为确有独立操作语义的新增 Common Node 或窄领域 Node 保留了扩展边界。
