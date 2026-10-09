# LLM-EdgeFlow 框架简化与跨请求状态实施计划

> 基线：`main@5140195`（2026-09-29）。本文件是工作计划，不是现行规则文档。
> 按 `CONTRIBUTING.md` §3，不把它作为提案归档提交到 `doc/`：每个阶段的设计写进对应 PR 描述，
> 落地后的规则写进各自的现行指南。

| 阶段 | 状态 |
| --- | --- |
| 1：业务源码自动收录 | 由 [业务开发者体验 RFC](DEVELOPER_EXPERIENCE_RFC.md) 的 WI-1 取代 |
| 2：Spec 签名提示 | 由 [业务开发者体验 RFC](DEVELOPER_EXPERIENCE_RFC.md) 的 WI-4 取代 |
| 3：Adapter 重复声明收敛 | 已完成，合入于 PR #150；现行规则见开发指南 |
| 4：跨请求状态 | 暂缓：出现需要跨请求状态的真实业务前不实施；保留本文设计与验收，届时按当时代码复核后再评审 |
| 5：平台与调度参数归位 | 已完成，合入于 PR #150–#154 |

## 1. 目标与约束

**目标**

- 做成通用框架：业务开发者只需要补业务逻辑（Node 函数、转换器的 Decode/Encode 函数），再写 Pipeline JSON 编排。
- 节点可以跨业务复用，多个节点组合成一个业务。
- 降低上手门槛，去掉框架中不必要的复杂度。

**硬约束**

| 约束 | 来源 |
| --- | --- |
| 四层架构及依赖方向不变 | 用户确认 |
| 平台契约不变：Operator API、宿主结构体、槽位名、输出池语义、Demo 与 SDK 的交互、`.conf` 格式 | 用户确认 |
| 节点写法保留"函数 + Spec"，不引入派生基类 | 讨论结论：两者心智基本一致，再加一层只会多一套写法 |
| InputConverter、OutputConverter 保留；删除 Core 业务与间接绑定，按 `(type, name)` 选择单槽转换器 | 后续 I/O 设计已实施 |
| 每项都要有明确收益，不做高投入低收益的事 | 用户要求 |

**已确认的决策**

| 问题 | 决定 |
| --- | --- |
| 接入选择是否属于平台成员布局 | `(type, name)` 仅在 Integration 选择转换器；平台成员与枚举待内网核对 |
| Exposure 白名单是否必须由代码控制 | 不需要，删除 |
| `.conf` | 平台定义，不动 |
| 跨请求状态的范围 | 每个 Operator 句柄一份 |
| 同一批次内的请求能否看到彼此的写入 | 不能，批次内各请求是平行的 |
| 进程重启后是否保留 | 不保留 |
| 状态接口的形式 | 通用接口：既支持累积（如 embedding 历史），也支持切换（如某个模式一直保持到下一个特殊请求） |
| Core 业务边界如何声明 | 删除独立业务登记；接入准备组合所选转换器端口并向 Core 传明确边界（I/O 设计替代原决定） |
| 阶段 4 的推进方式 | 暂缓；有真实业务需求时，先按当时代码复核设计和验收，评审确认后再实施 |
| 状态的提交边界 | 状态候选和输出发布对象全部准备成功后才最终提交；多个状态一起生效，或一起保持旧值 |
| 状态的数据所有权 | 从产生起就是共享不可变对象；提交只转移或共享所有权；已有读取指针始终有效；所有读取路径对初始空状态的处理一致 |
| 状态与请求数据如何配合 | 规定哪些端口可以直接读状态、哪些必须先广播到每条请求；新状态由写入节点生成，批内冲突指令由业务逻辑明确裁决 |
| 状态容量与成本 | 每个状态必须声明容量上限，历史类状态有明确的淘汰规则；内存峰值、每批耗时、"读取不复制"和"更新成本"都进入验收 |
| 真实业务闭环 | 先做"模式保持"，再做"向量历史" |

## 2. 现状核对

| 发现 | 证据 | 阶段 |
| --- | --- | --- |
| 没有跨请求状态。会话缓存 `GetOrCreateResult` 只能创建一次，没有更新和淘汰；端口 `lifetime=session` 只是元数据 | `include/nodes/session_resources.h`；`pipeline_validator.cpp:68-84` | 4 |
| 同一句柄上的 `Process` 已经被句柄互斥锁串行化 | `src/adapter/operator/operator_adapter.cpp:321` | 4 |
| `AlgContext` 按值存储（`std::any`），直接发布历史状态会每个批次复制一次 | `include/core/alg_context.h:43-47` | 4 |
| 读取入口不止一个：`BoundInput::Get`、`BoundInput::Require`、`NodeBase::Require` 都直接调用 `AlgContext::Read` | `include/nodes/node_base.h:65`、`:73`、`:301` | 4 |
| `TextTemplateNode`、`PromptGuidedLlmNode` 的上下文输入按请求聚合；共享条目（`req_id=0`）直接接进去只会挂到批内第一条请求上 | `src/common_nodes/text_template_node.cpp:525-534`；`src/custom_nodes/prompt_guided_llm_node.cpp:198` | 4 |
| 现有的共享数据约定只有 `req_id=0`，由消费方用配置开启（`VectorTopKNode` 的 `candidate_scope=shared`）；端口定义里没有"共享输入"的声明 | `src/common_nodes/vector_top_k_node.cpp:77-83`、`:164-173`；来源规则的允许值见 `src/core/node_definition_validation.cpp:17-21` | 4 |
| 输出发布在一个函数里边构造共享指针边写入输出，没有"准备"与"提交"之分。内存池会拒绝重复归还，所以中途失败不会损坏账本 | `src/adapter/operator/operator_process_binding.cpp:232-252`；`src/adapter/operator/operator_output_pool.cpp:134-151` | 4 |
| 测试夹具里有维度可配置的测试向量模型；Demo 在同一个句柄上按 `batch_size` 分批处理数据集 | `dev_support/inference/test_biz_models.cpp:83-92`；`demo/common/operator_runner.h:215-271` | 4 |
| Studio 会就地修改 Pipeline 对象，但按数据名自动推导连线 | `tools/pipeline_studio/web/workbench.js:65-120` | 4 |


## 3. 总览与顺序

| 阶段 | 内容 | 新概念 | 规模 | 设计审查 | 建议分支 |
| --- | --- | --- | --- | --- | --- |
| 4 | 跨请求状态 | `state`；端口来源规则新增 `shared` 取值 | 中偏大 | 需要（Pipeline 结构、所有权、提交边界），先评审第 8 节 | `feat/pipeline-state` |


**每个阶段的通用流程**

1. 从最新 `main` 建分支。
2. 需要设计审查的阶段，先在 PR 描述中写清：问题、方案、受影响的契约、取舍、验收标准、回退方式（`CONTRIBUTING.md` §3）。
3. 实现代码，并补充聚焦测试。
4. 更新受影响的现行文档；用户能感知到的变化写进 `doc/CHANGELOG.md`。
5. 运行 `./scripts/run_all_tests.sh`，必须通过。
6. 按本阶段的"验收"清单逐项检查，把结果记录在 PR 中。验收通过后，再进入下一阶段。


阶段 1、2 的设计与验收改由[业务开发者体验 RFC](DEVELOPER_EXPERIENCE_RFC.md)及其[详细设计](DEVELOPER_EXPERIENCE_DESIGN.md)维护。

## 8. 阶段 4：跨请求状态（暂缓；设计与验收留待真实需求出现时评审）

### 8.1 范围

- 用同一套机制支持两类场景：
  - 切换类：某个模式一直保持到下一个特殊请求；
  - 累积类：向量历史。
- 状态的作用范围是每个 Operator 句柄一份；同一批次内的请求读到同一份状态，互不可见对方的写入；进程重启后不保留。
- 不使用状态的 Pipeline 完全不受影响。
- 不做：按用户或会话 key 分区、持久化、通过 Control 清空状态、跨句柄共享（见 8.12）。

### 8.2 接口（Pipeline JSON）

```json
"state": {
  "mode":        { "type": "TextBatch",      "max_items": 1 },
  "vec_history": { "type": "EmbeddingBatch", "max_items": 10000 }
}
```

**声明**
- 名字：`[a-z][a-z0-9_]*`，最多 64 个状态。
- `type`：Catalog 中端口使用的 `type_id`，必须是可追溯批次类型（元素为 `TraceableItem`）。
- `max_items`：必填，范围 1–1,000,000，是框架强制的容量上限。

**读写**
- 读：节点**输入**接 `state.<名字>`，读到本批次开始时的版本。
- 写：节点**输出**接 `state.<名字>`，写入的是新状态的候选，按 8.5 的规则提交，下一个批次才能读到。
- `state.` 是保留前缀，不能用作普通数据名。

### 8.3 数据模型与所有权

**状态版本**
- 所有状态组成一个不可变的"状态集版本"：名字 → `std::shared_ptr<const std::any>`。
- 句柄的 `SessionContext` 持有当前版本（`std::shared_ptr<const StateSet>`）。

**初始值**
- 在 Pipeline 构建（节点初始化）时创建。
- 读或写某个状态的端口都知道它的 C++ 类型；第一个绑定该状态的端口创建该类型的空值，作为共享不可变对象放进初始版本。Validator 保证同一个状态只有一种类型。
- 因此运行时不存在"状态缺失"：`BoundInput::Get`、`BoundInput::Require`、`NodeBase::Require`、`AlgContext::Read/Has` 看到的都是真实的值，不需要各自特殊处理初始空状态。

**批次快照**
- 批次开始时，本批上下文持有当前版本的一个引用，并把每个状态作为只读的共享条目挂进上下文。
- 读取不复制；读到的指针在整个批次内有效，即使期间有新版本提交也不受影响。

**写入候选**
- 写入节点发布状态输出时，框架先按 8.4 校验，然后直接把值构造成共享不可变对象（`std::make_shared<const std::any>`），放在上下文里一个保留的暂存 key（`state.<名字>.next`）下。
- 此后这个对象不再被复制或移动。

**提交**
- 以当前版本为基础，只复制指针，构造出新版本，然后替换当前版本。
- 所有权在上下文和新版本之间共享；旧版本在最后一个持有者释放后回收。

**并发保护**
- 准备提交时，校验本批的基础版本仍是当前版本（乐观校验）；版本已变化则准备失败。
- 在 Operator 内同一句柄是串行的，这种情况不会发生；这条校验防止有人在 Operator 之外并发执行同一个 Pipeline 时丢失更新。

**需要的框架改动**
- `AlgContext` 增加只读共享条目。`Read`、`Has` 同时查找普通值和共享条目；共享条目不能被覆盖，也不能与普通值同名。

### 8.4 状态与请求数据的配合规则

1. **状态值是句柄级共享批次**：所有条目 `req_id = 0`，`sub_id` 从 0 连续编号，条目数不超过 `max_items`。写入时由框架校验，违反时写入节点失败，本批不提交。
2. **直接读取**：只有"共享输入"端口可以直接接 `state.*`。
   - 端口来源规则（provenance）新增取值 `shared`，由节点作者在 Spec 中声明，例如 `PortFlow{"N:1", "shared"}`。
   - 现有的 `VectorTopKNode` 的 `candidates`、`candidate_texts` 通过配置切换：端口新增 `provenance_config_field` 字段（与现有 `lifetime_config_field` 同一机制），`candidate_scope` 为 `shared` 时，该端口按共享输入处理。
   - 接到非共享端口时，Validator 报 `PORT_PROVENANCE_MISMATCH`，提示"该端口按请求处理数据，需要先把状态广播到每条请求"。
   - 共享输入端口仍然接受现有的普通数据（例如语料向量），现有配置不受影响。
3. **需要广播**：
   - 按条对齐的输入（Map、LLM 文本）和按请求聚合的输入（如 `TextTemplateNode`、`PromptGuidedLlmNode` 的 `context`）不能直接接状态，否则状态只会挂到批内第一条请求上。
   - 需要按请求使用状态时，由节点调用框架提供的 `BroadcastToRequests(shared, anchor)`（放在 `nodes/traceable_batch_operations.h`），把共享条目复制到 anchor 中的每个请求上。
   - 纯 JSON 编排用的通用广播节点暂缓，见 8.12。
4. **写入**：
   - 写入节点根据"当前状态 + 本批数据"产出**完整的新状态**，不是增量。
   - 写入端口声明来源规则 `shared`。
   - 结果只能依赖本批数据及其顺序（批内顺序就是平台输入顺序）。
   - 同一批出现相互冲突的指令时，由写入节点的业务规则明确裁决，并写进节点说明和测试。模式保持业务采用"批内最后一条指令生效"。
5. **给发出指令的请求回执**：写入节点另开一个普通的按请求输出端口，不从状态读取。

### 8.5 成功与提交边界

`Operator_Process` 的顺序：

| 步骤 | 内容 | 失败时 |
| --- | --- | --- |
| 1 | 输入校验、输出槽解析 | 返回错误；没有租约，状态不变 |
| 2 | 解码 | 同上 |
| 3 | 租用输出块，由租约守卫接管 | 守卫归还已租用的块；状态不变 |
| 4 | 执行 Pipeline：开始时绑定当前状态版本；写入节点产出已校验的候选 | 同上 |
| 5 | 输出编码，写入已租用的块 | 同上 |
| 6 | **准备状态提交**：获取句柄状态锁；校验基础版本仍是当前版本、每个声明的状态都有且只有一个候选；构造新版本（只复制指针） | 同上，并释放锁 |
| 7 | **准备输出发布**：为每个输出块建好发布用的共享指针，但不写入 `outputs` | 同上 |
| 8 | **最终提交**（不会失败）：替换状态版本；把准备好的指针写入 `outputs` 中已存在的 key；提交租约 | — |

**规则**
- 第 8 步只包含不会抛异常的操作：共享指针交换、对已存在 key 赋值、租约守卫提交。
- 多个状态在同一个新版本里一起替换，要么全部生效，要么全部保持旧值。
- 不存在"状态已提交但输出没发布"或反过来的中间状态。

**接口**
- `Pipeline::Execute(AlgContext*)`：执行，并绑定快照。
- `Pipeline::PrepareStateCommit(const AlgContext&, std::string* error)`：返回持有锁和新版本的对象；该对象的 `Commit()` 声明为 `noexcept`；销毁时如果还没提交，则放弃。
- 不声明状态的 Pipeline 返回一个空操作对象。
- Operator 之外直接运行 Pipeline 的调用方（测试、工具）不调用提交，状态就不会前进，这是安全的默认行为。

**输出发布拆分**
- 把 `PublishOperatorOutputs` 拆成"准备"和"提交"两步。准备阶段失败时，只由租约守卫统一归还内存块。

**故障注入**
- 在第 6、7 步各加一个测试专用钩子，沿用现有 `Pipeline` 的 `test_internal_hook_` 做法。

### 8.6 校验规则（沿用现有诊断码，不新增）

| 规则 | 诊断码 |
| --- | --- |
| `state` 声明缺字段、字段类型错误、超出范围、名字不合法、类型不是可追溯批次 | `MISSING_FIELD` / `FIELD_TYPE` / `FIELD_RANGE` |
| 读写未声明的状态名；`state.` 前缀用于其他用途；接入边界（Binding 映射）使用 `state.*` | `UNKNOWN_FIELD` |
| 同一个状态有多个写入节点 | `DUPLICATE_PORT_PRODUCER` |
| 声明的状态没有写入节点；读取端口的类型与声明不符 | `MISSING_INPUT_PRODUCER`（与普通端口的类型不符报法一致） |
| 写入端口的类型与声明不符 | `INVALID_COMBINATION` |
| 读取端口不是共享输入；写入端口没有声明 `shared` | `PORT_PROVENANCE_MISMATCH` |

读取状态不产生依赖边；写入节点可以读取自己写的状态，不会被判为成环。`alg_pipeline_tool plan` 的输出增加 `states` 列表（名字、类型、容量、写入节点、读取节点）。Studio 不画状态连线，这是查看状态读写关系的入口。

### 8.7 各层改动

| 层 | 改动 |
| --- | --- |
| 编排层 | 配置结构增加 `state` 并解析（外部文档结构自动继承）；Validator 按 8.6 校验；`ValidatedPipelinePlan` 增加 `states`；`ResolvedPortBinding` 增加状态名和容量；来源规则允许 `shared`，并支持 `provenance_config_field`；`AlgContext` 增加共享条目；`SessionContext` 保存状态集版本；`Pipeline` 绑定快照，提供准备与提交接口 |
| 能力节点层 | 绑定到状态的 `BoundInput`/`BoundOutput` 在初始化时创建类型化的空值；写入时按 8.4 校验并构造共享对象；`VectorTopKNode` 的两个候选端口声明 `provenance_config_field = "candidate_scope"`；新增 `BroadcastToRequests` 辅助函数 |
| 接入适配层 | `Operator_Process` 按 8.5 调整；拆分输出发布；增加测试钩子 |
| 工具 | Studio 推导连线时跳过 `state.*`，保存时保留 `state`；`plan` 输出 `states`；导出的 JSON Schema 自动包含 `state` |
| 文档 | `configs/README.md` 的状态规则；`doc/architecture.md` 的状态版本与提交边界；`doc/dev_guide/custom_node_concepts.md` 的共享输入、广播与写入规则；`doc/developer_guide.md`；Studio README；CHANGELOG |

### 8.8 闭环一：模式保持（真实业务）

**业务**：关键词匹配（`keyword_match`）加上模式保持。复用所选转换器和 Demo，不新增业务契约。

**规则**
- `#严格模式` 切到 strict，`#常规模式` 切回 normal。
- 同一批出现多条指令时，批内最后一条生效。
- normal 模式用常规规则，strict 模式用更严格的规则。
- 两套规则都把指令本身识别为 `MODE_COMMAND`，便于回执。

**配置**（`configs/pipeline_keyword_match_mode.json` 及对应 `.conf`）：

```json
{
  "state": { "mode": { "type": "TextBatch", "max_items": 1 } },
  "pipeline": [
    { "id": "mode_switch", "node_type": "ModeSwitchNode",
      "config": { "commands": { "#严格模式": "strict", "#常规模式": "normal" },
                  "default_mode": "normal" },
      "inputs":  { "texts": "input_sentences", "current": "state.mode" },
      "outputs": { "next": "state.mode" } },
    { "id": "rules_normal", "node_type": "TextRuleMatchNode",
      "config": { "categories": { "URGENT": ["加急"],
                                  "MODE_COMMAND": ["#严格模式", "#常规模式"] } },
      "inputs": { "text": "input_sentences" }, "outputs": { "matches": "normal_matches" } },
    { "id": "rules_strict", "node_type": "TextRuleMatchNode",
      "config": { "categories": { "URGENT": ["加急", "尽快"], "COMPLAINT": ["投诉"],
                                  "MODE_COMMAND": ["#严格模式", "#常规模式"] } },
      "inputs": { "text": "input_sentences" }, "outputs": { "matches": "strict_matches" } },
    { "id": "select", "node_type": "ModeSelectNode",
      "inputs":  { "mode": "state.mode", "normal": "normal_matches", "strict": "strict_matches" },
      "outputs": { "matches": "rule_matches" } }
  ],
  "io": {"input":[{"type":"keyword_in","name":"keyword_match"}],
         "output":[{"type":"keyword_out","name":"keyword_match"}]}
}
```

**新增两个自定义节点**（`src/custom_nodes/`，属于常规自定义 Node）
- `ModeSwitchNode`（写入节点）：
  - 输入 `texts`，以及共享输入 `current`；
  - 输出共享的 `next`，只有 1 条；
  - 规则：从当前模式出发（为空时用 `default_mode`），按批内顺序应用指令，最后一条生效。
- `ModeSelectNode`（直接读取状态）：
  - 输入共享的 `mode`，以及按请求的 `normal`、`strict` 两路结果；
  - 按本批快照的模式选择其中一路，输出与 `normal` 保持同序、同来源。

**Demo 与数据**
- 新增 Profile `keyword_match_mode`（`batch_size: 2`）和数据集 `tests/fixtures/effects/keyword_mode_inputs.txt`，并加入 Smoke 套件。
- 数据集的 7 行与期望结果（每 2 行一个批次）：

| 行 | 输入 | 所在批次读到的模式 | 期望结果 |
| --- | --- | --- | --- |
| 1 | 请尽快回复 | normal | 未命中 |
| 2 | #严格模式 | normal | `MODE_COMMAND` |
| 3 | 请尽快回复 | strict | `URGENT` |
| 4 | 我要投诉 | strict | `COMPLAINT` |
| 5 | #常规模式 | strict | `MODE_COMMAND` |
| 6 | 我要投诉 | strict | `COMPLAINT`（与第 5 行同批，仍读到 strict） |
| 7 | 我要投诉 | normal | 未命中 |

**复用情况**：`TextRuleMatchNode` 用了两次，转换器与 Demo 运行代码都不改。新增的两个节点是这个业务真正需要的逻辑，可以借此检验接口是否好用。评审节点在 4c 结束时进行（见 8.11）。

### 8.9 闭环二：向量历史（验证容量、共享条目和成本）

**测试用 Pipeline**
- 使用测试向量模型：`TextEmbeddingNode` → `VectorTopKNode`（`candidate_scope: shared`，候选接 `state.vec_history`）→ 命中判定 → `rule_matches`。
- 另有写入节点，输入为历史状态和本批向量，输出新的历史。
- 写入节点和命中判定节点放在 `tests/` 中，作为测试夹具。

**容量与淘汰**
- 状态声明 `max_items`，由框架强制。
- 写入节点按先进先出淘汰最旧的条目，并在输出前把条目数收敛到上限以内。
- 输出超过上限时写入失败，本批不提交。

**成本测量**
- 新增 `dev_support/benchmarks/state_history_bench.cpp`，不进门禁，结果记录在 PR 中。
- 测量组合：`max_items ∈ {1,000; 10,000; 100,000}` × 向量维度 `{384; 768}`。
- 分别记录：
  - 快照绑定耗时；
  - 写入节点更新耗时（不可变状态下，追加需要构造新历史，预期与条目数成线性关系）；
  - 提交耗时；
  - 每批总耗时（p50/p95）；
  - 进程内存峰值（`getrusage`）。
- 理论内存上界：同一时刻最多存在当前版本和候选两份历史，约为 `2 × max_items × 每条字节数`。

### 8.10 验收

**Core 单元测试**
- [ ] 8.6 中每条规则都有对应的报错用例。另外验证：读取不产生依赖边；写入节点读取自己的状态不成环；`plan` 输出 `states`。
- [ ] 初始空状态：通过 `BoundInput::Get`、`BoundInput::Require`、`NodeBase::Require`、`AlgContext::Has` 读取，结果一致，都是真实存在的空值。
- [ ] 所有权：
  - 批内读到的指针在后续提交之后仍然有效，内容不变；
  - 用一个会计数复制次数的测试载荷类型，验证读取和提交的复制次数为 0。
- [ ] 写入校验：`req_id ≠ 0`、`sub_id` 不连续、超过 `max_items` 时，写入节点失败，状态不变。
- [ ] 共享输入规则：
  - 状态接到按请求聚合的端口、按条对齐的端口，都报 `PORT_PROVENANCE_MISMATCH`；
  - 接 `VectorTopKNode` 时，`candidate_scope=shared` 可以通过，`request` 报错；
  - 现有全部配置仍然通过校验。
- [ ] `BroadcastToRequests`：共享条目被复制到每个请求，`sub_id` 保持原顺序。

**提交边界集成测试**（通过 Operator 运行）
- [ ] Pipeline 中某个节点失败时，状态不变。
- [ ] 输出编码失败时（用极小的 `out_mem` 容量触发），状态不变。
- [ ] **输出发布准备失败**时（第 7 步钩子）：没有任何输出被发布，所有租用的块都回到空闲状态（按内存池账本核对），状态不变。
- [ ] **多状态提交准备失败**时（第 6 步钩子，Pipeline 声明两个状态）：两个状态都保持旧值。
- [ ] 两个状态都成功写入时，下一批同时看到两个新值，且来自同一个版本。
- [ ] 句柄隔离：同一配置的两个句柄，状态互不影响。

**闭环一：模式保持**
- [ ] 8.8 表中 7 行的结果全部符合期望，通过 Operator 集成测试和 Demo Profile 各验证一次；Profile 已加入 Smoke 套件。
- [ ] 冲突裁决：同批出现 `["#严格模式", "#常规模式"]` 时下一批为 normal；顺序反过来时为 strict。
- [ ] 在失败回滚和句柄隔离场景下，也使用这个业务的配置再验证一次。

**闭环二：向量历史**
- [ ] 第一批读到空候选，不报错；批 1 的 `"A"` 未命中，批 2 的 `"A"` 命中，`"B"` 未命中。
- [ ] 历史中的条目都是 `req_id=0`，`sub_id` 连续；达到容量后淘汰最旧的条目，条目数始终不超过 `max_items`。
- [ ] 成本测量结果写进 PR，并满足：
  - 快照绑定和提交耗时与条目数无关（1,000 与 100,000 条之间相差不超过 2 倍）；
  - 更新耗时单独列出；
  - 内存峰值增量不超过理论上界的 1.25 倍。

**其他**
- [ ] Studio：含 `state` 的方案可以打开、保存、校验，保存后 `state` 字段仍在；图中不出现状态连线；导出的 JSON Schema 包含 `state`。
- [ ] 现有全部配置和 Smoke 套件的行为不变；门禁通过；8.7 列出的文档全部更新。

**回退**：`state` 是可选的新增字段；按 8.11 的子步骤分别提交，任一子步骤都可以单独回退。

### 8.11 实施顺序（评审确认后）

| 子步骤 | 内容 | 结束时的检查点 |
| --- | --- | --- |
| 4a | 编排层与节点层的运行时及校验（不含 Operator） | Core 单元测试全部通过 |
| 4b | Operator 提交边界与输出发布拆分 | 提交边界集成测试全部通过 |
| 4c | 闭环一：模式保持 | 业务测试与 Demo 通过。**在这里暂停评审接口是否好用**，必要时先调整接口，再进入 4d |
| 4d | 闭环二：向量历史与成本测量 | 容量与成本验收通过，门禁通过 |

### 8.12 本阶段不做

- **纯 JSON 编排用的通用广播节点、通用"追加并限制容量"节点**：两个闭环都不需要。等第一个真实业务需要时，再按新增通用 Node 的流程（设计审查）加入。
- 按用户或会话 key 分区、持久化、通过 Control 清空状态、跨句柄共享、在状态上配置 merge 或 evict 规则。

## 9. 不做的事项及理由

| 提议 | 结论 | 理由 |
| --- | --- | --- |
| Core 业务登记与边界 | 已删除独立登记 | 所选转换器形成接入边界；全部方案校验与 converter 端口契约测试承担一致性检查，无额外挂接/派生时序。 |
| 用派生基类写节点 | 不做 | 与 Spec 的心智基本一致（`MakeLlmTextSpec` 本身就是"填钩子"），加一层只会多一套写法 |
| 具名链式钩子、公开通用节点的 Spec、拆分/过滤/排序预设工厂、节点分类树、`PortFlow` 改枚举 | 不做 | 都是新写法或新概念。报错可读性的问题由阶段 2 解决；`PortFlow` 的非法值在注册时已经会被校验 |
| 把接线从 C++ 挪到 JSON `deployment.io` | 不做 | 需要同时改 Validator、Catalog、Studio、SDK 初始化和全部配置。阶段 3 在 C++ 内合并已经拿到主要收益 |
| `REGISTER_DECODER/ENCODER`、`RowInputConverter` 等新名字 | 不做 | 与现有 InputConverter、OutputConverter 同义 |
| 由函数签名推导转换器的宿主类型和槽位 | 不做 | `ExternalInputSlot<T>`、`ExternalOutputSlot<T>` 已经由类型推出类型名；槽位名是平台契约字符串，只需写一行，收益小 |
| Demo 按载体重组 | 已实施 | 两个登记表分别选择输入构造组合与输出展示，同载体共享实现 |
| 模型配置拆分、节点预设、子图 | 不做 | 都是新的 JSON 概念，成本高 |
| 配置变体一致性测试 | 不做，交给方案负责人 | 现有差异里，按后端调整的生成参数和模板可能是有意的，规则内容的差异可能是遗漏，这需要业务判断，不是框架机制的问题。已发现的差异见附录 |
| 状态的 merge、evict 等配置项 | 不做 | 合并与淘汰规则由写入节点实现；框架只强制容量上限 `max_items` |
| 纯 JSON 用的通用广播节点、通用追加节点 | 暂缓 | 两个闭环都不需要，等第一个真实业务需要时再加（见 8.12） |
| 统一错误类型、大规模改写文档 | 不做 | 各阶段只更新受影响的文档 |
| 部署配置与业务编排分离（按硬件拆出 `models`、`backend_config`，业务 Pipeline 只留一份） | 不做 | 改变 Pipeline JSON 格式，属于外部契约；与"模型配置拆分"同理。5.1 和 5.3 的默认值清单已能去掉大部分重复 |
| 合并 `model_config.max_batch_size` 与 `backend_config.max_batch_size` | 不做 | 现有配置写了这两个字段，而未知字段会被拒绝；删掉任一字段都会让现有配置失效 |
| 新增 SDK 接口查询有效批次上限 | 不做 | 改变公共 Operator 接口；有效限制改由 `resolve-conf` 查询（5.3） |
| Demo 运行前读取内部有效限制并自动分批 | 不做 | Demo 应和真实宿主一样只通过 SDK 运行；超限时 Create/Process 已明确报错 |
| 批量删除配置中等于默认值的显式字段 | 不做，交给方案负责人 | 显式值可能是有意固定，现在无法区分（Studio 曾自动写入默认值，已由 5.1 修复，PR #150）。现有配置保持原样，清单待方案负责人确认（见 [业务开发者体验 RFC 第 10 节](DEVELOPER_EXPERIENCE_RFC.md#10-待决事项)） |
| `validate` 对"显式值等于默认值"给出警告 | 不做 | 与"保留有意固定的显式值"矛盾；根源由 5.1 修复 |

## 11. 完成后的效果

| 场景 | 现在 | 之后 |
| --- | --- | --- |
| 前面请求的结果影响后续请求 | 没有正式支持 | 声明 `state`（含容量上限）；写入节点产出新状态，共享输入端口直接读取，提交边界由框架保证 |

## 附录：配置变体差异（供方案负责人确认）

各方案不同变体之间的节点、类型和连线完全一致，差异只在节点配置上：

| 方案 | 对比 | 不同的配置 |
| --- | --- | --- |
| doc_qa | cpu 与 default | `TextRuleMatchNode.categories`：default 多出"发票"和 `LOGISTICS_STATUS` 类别；某个类别的关键词在 cpu 中是 "ONNX、llama.cpp"，在 default 中是 "NPU" |
| doc_qa | cpu 与 kite | `TextRuleMatchNode.categories`、`LlmGenerateNode.temperature` |
| doc_qa_rerank | cpu 与 default | `TextRuleMatchNode.default_score/default_category`、`TextTemplateNode.template` |
| doc_qa_rerank | cpu 与 kite | `LlmGenerateNode.temperature` |
| entity_extract | cpu 与 default | `LlmGenerateNode.temperature/max_tokens`、`StructuredJsonParseNode.failure_policy/fallback_json` |
| dialogue_audit | default 与 kite | `LlmGenerateNode.temperature/max_tokens`、`StructuredJsonParseNode.fallback_json`、`TextTemplateNode.template` |
