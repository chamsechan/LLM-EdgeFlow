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
