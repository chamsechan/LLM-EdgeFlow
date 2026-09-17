# RFC 0061: Pipeline JSON 集中管理部署配置

- **RFC 编号**：0061-pipeline-owned-deployment-configuration
- **创建日期**：2026-09-17
- **文档状态**：In Implementation
- **关联分支**：`docs/pipeline-deployment-rfc`（设计文档）
- **目标版本**：下一次配置格式切换版本，具体发布号待定
- **负责人 / 作者**：LLM-EdgeFlow 维护者
- **代码核查基线**：`c7358b7`（RFC-0060 合入后的 `main`）
- **关联决策**：取代 RFC-0059 中 Schema 1 `.conf` 承载接入配置的规定，以及 RFC-0025 中模型路径覆盖存放于 `.conf` 的规定；继承 RFC-0049 的输出分配机制、RFC-0050 的参数文本边界及 RFC-0060 的 C++ Operator 唯一入口。

> 本文是待实施规格。当前代码尚不接受本文的新文件格式，本次只提交 RFC 与索引。
> 最终方向：`.conf` 只有 `pipe_path`；Pipeline JSON 的 `deployment` 管理部署信息，
> 其中 `io_binding` 与 `output_allocations` 始终作为一组配置。

## 1. 问题与范围

### 1.1 现状与目标

当前同一方案分散在两个文件：`.conf` 的 `data` 中有 `pipe_path`、`io_binding`、
`model_paths`、`outputs`；Pipeline JSON 中有 `biz_name`、`models`、`pipeline` 等。
只复制或编辑 JSON 无法带走完整的接入绑定、模型覆盖与外部输出容量。

两个 `outputs` 的语义也不同：`.conf` 描述 Operator 外部输出对象的分配，
`pipeline[*].ports.outputs` 描述 Node 逻辑输出端口到 Blackboard Key 的映射。
迁移不能让它们在同一 JSON 中继续使用容易混淆的名称。

实施后的用户路径是：在 JSON 中维护算法和部署配置，`.conf` 仅作为宿主已有
`cfg_file_name` 入口的定位文件。切换 `.conf.pipe_path` 即切换整套部署配置。

### 1.2 已核对的实现边界

| 当前入口 | 当前职责 | 本次影响 |
| --- | --- | --- |
| `src/adapter/deployment_io_config.*` | 严格读取 Schema 1 `.conf`，解析 `data.pipe_path` | 收窄为仅定位 JSON |
| `src/adapter/io_binding_resolver.*` | 查 binding/converter，校验外部槽位，读取 JSON，应用模型覆盖，生成 `ValidatedIoPlan` | 配置信息改从同一 JSON 快照取得，收敛文件和内存入口 |
| `src/adapter/operator/operator_config_resolver.*` | 宿主根目录、安全路径、输出分配和有效深度预算 | 沿用这些规则，调整读取来源 |
| `src/core/pipeline_config.cpp` | Pipeline 根字段白名单与严格解析 | 继续只解析中性 Pipeline 文档 |
| `src/tools/alg_pipeline_tool.cpp` | 编排校验、部署预检与生效配置报告 | 增加部署文档拆分；更新来源报告 |
| Studio、recipe、效果评估 | 读取/继承旧 conf 字段并生成临时部署文件 | 全链路迁移，不能只修改生产 resolver |

本次影响接入适配层 / Integration、流程编排层 / Orchestration 的输入边界及工具。
能力节点层 / Capability Nodes 与模型执行层 / Model Execution 的算法、端口、模型语义不变。
不修改 Operator 函数表、`CreateParam` 布局、平台载体、Converter 注册契约或输出池生命周期。
不引入配置热更新、多 binding 同时运行、JSON include/继承、环境变量替换或新的公司 SDK 依赖。

## 2. 决策与权衡

### 2.1 `.conf` 字面上只保留一个字段

以当前对话审核方案为例，迁移后的完整 `.conf` 为：

```json
{
  "pipe_path": "pipeline_dialogue_audit_default.json"
}
```

这里的“只剩一个 `pipe_path`”指整个文件只有这个键，删除旧 `schema_version` 和 `data`
包装，不仅是缩减 `data` 中的业务字段。

- 根必须是对象，必须有且仅有非空字符串 `pipe_path`；数组、空值、未知键均拒绝。
- 旧 Schema 1 包装、顶层或 `data` 内的 `io_binding/model_paths/outputs` 都拒绝，
  报错指向迁移说明，不做自动合并或读取优先级回退。
- `pipe_path` 继续相对于 `.conf` 所在目录；目标必须存在且是普通 JSON 文件，
  规范化后必须位于该目录内。位于目录内的绝对路径可沿用现有支持，目录外路径及符号链接逃逸拒绝。
- `CreateParam.model_path` 仍为模型与配置根目录，`cfg_file_name` 仍为其中的相对配置路径。
  保留现有根目录边界和禁止搜索回退的规则。

持久化格式采用一次切换，不新增版本协商。Catalog、CLI 报告及业务 payload 中原有的
`schema_version` 属于各自契约，不能随 `.conf` 包装一起删除。

### 2.2 JSON 的确定结构与名称

现有 Pipeline 根字段保持原位置，增加一个 Integration 所有的 `deployment` 对象：

| 路径 | 类型与约束 | 含义 |
| --- | --- | --- |
| `deployment` | 对象；可部署文档必填；只允许 `model_paths`、`io` | 同文件内的部署信息 |
| `deployment.model_paths` | 可选对象，缺省等同 `{}`；键为已声明的 `model_id`，值为非空路径字符串 | 原 `.conf.data.model_paths`，保留覆盖语义 |
| `deployment.io` | 对象，部署时必填；有且仅有下面两个字段 | 一份完整的 Operator 接入配置 |
| `deployment.io.io_binding` | 必填非空字符串 | 已注册 binding 的 ID |
| `deployment.io.output_allocations` | 必填对象，键为外部输出槽位名 | 原 `.conf.data.outputs`，每槽位的分配配置 |
| `pipeline[*].ports.outputs` | 保持当前对象格式 | Node 逻辑端口到内部 Blackboard Key 的映射 |

`output_allocations` 强调其内容包括外部类型、allocator、参数、元数据与容量，
不是算法结果，也不是内部端口连线。文档和界面分别称为“外部输出分配”和“节点输出端口”。
不得将 `output_allocations` 再命名为 `outputs`，也不改名既有 `ports.outputs`。
内部代码持有外部分配配置时使用 `output_allocations` 等明确名称，避免新加含糊的 `outputs` 成员。

下面是迁移后**完整、无模型依赖的关键词方案**。名称和端口已通过当前
`alg_pipeline_tool catalog --biz keyword_match_v1` 核对；新部署格式须在实施后才能使用。

```json
{
  "biz_name": "keyword_match_v1",
  "deployment": {
    "io": {
      "io_binding": "keyword_match.operator.v1",
      "output_allocations": {
        "keyword_out": {
          "type": "keyword_out",
          "meta_num": 0,
          "metadata_type_id": 0,
          "capacities": {
            "match_result_json": 2047
          }
        }
      }
    }
  },
  "models": [],
  "pipeline": [
    {
      "id": "node_0_TextRuleMatchNode",
      "node_type": "TextRuleMatchNode",
      "depends_on": [],
      "ports": {
        "inputs": {
          "text": "input_sentences"
        },
        "outputs": {
          "matches": "rule_matches"
        }
      },
      "config": {
        "categories": {
          "SYSTEM_INIT": ["初始化", "自检"]
        }
      }
    }
  ]
}
```

关键词 `.conf` 则是 `{"pipe_path":"pipeline_keyword_match_rules.json"}`。
这里 `matches → rule_matches` 是内部数据连线；`keyword_out` 是宿主输出槽位。
它们通过注册的 OutputConverter 和 IoBinding 关联，不通过名称相等或新增 JSON 映射关联。

对用户当前打开的 `pipeline_dialogue_audit_default.json`，保持现有 `models` 与
九个节点内容，向根对象加入以下成员即可；**这是合并片段，不是完整 Pipeline 文件**：

```json
{
  "deployment": {
    "model_paths": {
      "embed_model_v2": "./models/bge_base_zh_v1.5.onnx",
      "rerank_model_v1": "./models/ms_marco_tinybert_l2_v2_quantized.onnx",
      "audit_llm_v1": "./models/qwen2.5-0.5b-instruct-q4_k_m.gguf"
    },
    "io": {
      "io_binding": "compliance_audit.operator.v1",
      "output_allocations": {
        "audit_out": {
          "type": "audit_out",
          "meta_num": 0,
          "metadata_type_id": 0,
          "capacities": {
            "risk_level": 31,
            "matched_policy_clause": 255,
            "audit_verdict_json": 1023
          }
        }
      }
    }
  }
}
```

### 2.3 `io_binding` 与外部输出分配的配对规则

“一对”是**一个 binding ID 对应一整份外部输出分配表**，不表示只能有一个输出槽位。
一个 JSON 只有一个 `deployment.io`；同一绑定可以要求多个外部槽位。

1. `io_binding` 和 `output_allocations` 必须同时存在于同一个 `io` 对象。
   不接受从 `.conf`、Profile、`biz_name` 默认映射或另一份 JSON 补齐半组配置。
2. 根据 binding 找到注册的 InputConverter 和 OutputConverter；确认 transport 为
   `operator`，且 binding 的 `biz_name` 与 JSON 根的 `biz_name` 一致。
3. 设输出转换器声明的全部输出槽位集合为 `A`，其中必需槽位为 `R`，配置键集合为 `C`。
   必须满足 `R ⊆ C ⊆ A`。因此缺少必需槽位、额外未知槽位都报错；可选槽位可不配置。
   `{}` 仅在没有必需输出槽位时合法，仍须显式写出此对象。
4. 每个槽位继续调用现有 `ResolveOutputAllocation`：校验 `type` 与声明一致、allocator
   已注册、参数可解析、容量和元数据合法，保留单槽位与整句柄预算检查。
   支持的字段仍是 `type/allocator/params/meta_num/metadata_type_id/capacities`，默认值沿用注册机制。
5. `type` 不能由 map 键猜测；容量不能由 `ports.outputs` 推导；`params` 文本解析、
   归一化参数共享及 `max_frame_depth` 的作用保持原契约。
6. Studio、recipe 和 Profile 继承以 `deployment.io` 为最小整体单位。只更换 binding 时，
   必须重新校验整张分配表；不能静默保留另一 binding 的分配配置。

这些检查在 Create / 部署预检时完成。失败不发布可用句柄，不初始化 Node、不加载模型、
不开始输出池分配；已创建句柄持有不可变计划，后续修改文件仅对新 Create 生效。

### 2.4 模型路径覆盖与解析基准

本次保留 `model_paths`，因为需求是迁移其位置，而非取消部署覆盖。它与
`models[*].model_path` 的职责和优先级明确如下：

```text
effective_path(model_id) =
  deployment.model_paths[model_id]  （显式配置时）
  models 中该 model_id 的 model_path（否则）
```

- `models` 是模型声明的唯一列表；`model_paths` 不能新增模型、改变 capability 或 Backend。
  未知 ID、空 ID、非字符串值和空路径均拒绝。无模型时只能省略它或使用 `{}`。
- 原 `models[*].model_path` 仍按现有契约必填非空；覆盖不能把一个缺字段或字段类型错误的
  模型声明变成合法声明。复用 Core 严格解析做结构检查，不能在工具端复制模型 schema。
- 相对有效路径继续相对宿主 `CreateParam.model_path` 解析，**不因字段搬到 JSON 就改为
  相对 JSON 目录、conf 目录或进程工作目录**。绝对路径沿用共享 resolver 的规则，
  给定根目录时必须位于根目录内；规范化、`..` 与符号链接边界继续由共享路径解析器负责。
- 例如宿主根为 `/bundle` 时，`models/a.onnx` 解析到 `/bundle/models/a.onnx`；
  JSON 即使位于 `/bundle/configs/`，也不会得到 `/bundle/configs/models/a.onnx`。
- `model_config.tokenizer_file` 等 sidecar 配置留在原位置，沿用各 Model 的资源目录契约。
  不以新 `deployment.model_paths` 扩展为任意 JSON 字段覆盖系统。
- 静态校验不加载模型，也不把“模型文件实际可加载”当作成功承诺；实际加载失败仍由
  现有 Model/Backend 路径报告。

### 2.5 文档集中，但职责仍然分层

同一 JSON 是文件组织方式，不是让 Core 认识 Operator 的理由。

```text
Operator Create / validate-io / resolve-conf
  → 读取仅有 pipe_path 的 conf
  → Integration 读取一次完整 JSON 并拆分 deployment 与中性 Pipeline
  → 校验 deployment.io，应用模型覆盖、解析路径
  → 由注册的 Converter/Binding 构造中性 PipelineIoBoundary
  → PipelineValidator::ValidateAndPlan
  → ValidatedIoPlan（含同一次校验的 ValidatedPipelinePlan）
  → Pipeline 消费已有计划，执行时不重读配置
```

文档拆分使用 Integration 私有的轻量结构与解析函数，按复用需要放在现有 resolver
旁或 `src/adapter/pipeline_document.h/.cpp`，具体类型和函数名由实现确定。
职责按下表分配：

| 组件 | 实施责任 |
| --- | --- |
| `DeploymentIoConfig` | 收窄为启动定位信息，删除承载 binding、模型覆盖、输出分配的成员；可按新职责局部改名，不新增旧结构兼容类 |
| Integration 文档解析函数 | 从同一 JSON 值取出并严格检查 `deployment`，产生供 Core 使用的中性 Pipeline 值；解析结果只保留后续校验和规划所需数据 |
| 编辑工具 | 自行持有完整源 JSON，编辑后保留 `deployment`；运行时解析结构不为编辑用途额外保存整份原文副本 |
| `IoBindingResolver` | 文件入口负责读取并调用 JSON 入口；二者共用一个严格的接入解析实现，校验 binding/converter/槽位，构造 I/O 边界并调用唯一 PipelineValidator |
| `OperatorConfigResolver` / 模型路径 resolver | 保留宿主路径和分配规则；有效队列深度预算在物化前完成 |
| `ParsePipelineConfig` / `PipelineValidator` | 只接收去除 `deployment` 的中性字段；继续拒绝未知字段并拥有全部 DAG、模型声明、端口与类型规则 |
| `ValidatedIoPlan` / `ResolvedOperatorConfig` | 保存同一快照的解析结果、计划及路径来源；检查工具不能为报告再次打开文件或重新规划 |

拆分时只能取走已识别、已校验的 `deployment`。其他未知根字段留给 Core 报错，
不能用“复制已知字段”偷偷过滤拼写错误，也不能简单 `erase("deployment")` 后声称完整校验成功。
Core 不引入 Adapter 头文件，不增加 allocator、外部槽位、binding ID 或文件读取逻辑。

现有 `ResolveFromPipelineJson` 不校验外部分配；实施时将其收敛为上述严格 JSON 入口，
部署入口始终要求完整 `deployment.io`。按实际调用需要保留薄封装，解析步骤作为私有函数，
不为每个中间结构增加入口，也不增加 `require_deployment` 等开关改变部署入口的必填约束。

中性算法测试直接使用 `PipelineValidator`；需要绑定边界的测试通过测试辅助函数构造输入，
复用现有校验逻辑。生产 resolver 不提供测试专用解析分支，也不在缺少 `deployment` 时
自动回退到中性测试路径。工具对纯算法文档的支持由工具入口显式分流，见下一节。

### 2.6 工具模式、诊断和保存契约

| 入口 | 迁移后的确定行为 |
| --- | --- |
| `validate` / `plan`，包括 `--stdin`、`--explain` | 支持完整 JSON；有 `deployment` 就校验其结构、模型覆盖和整组 I/O，按 binding 提供边界；不读模型文件，不要求部署根目录。模型路径仅做环境无关检查 |
| 仅有中性字段的 `validate` / `plan` 输入 | 保留算法编排/单元测试用途，使用原有中性校验；成功只表示算法配置通过，不能据此 Create。不得将有缺陷的 `deployment` 降级成此模式 |
| `validate-io CONFIG --model-root DIR` | 继续以 `.conf` 为入口，读取其指向的完整 JSON；要求 `deployment.io`，按部署根解析路径并检查注册/预算，不加载模型 |
| `resolve-conf FILE --root DIR --depth N` | 使用与 Create 相同的 resolver 和有效深度；报告来源于同一 `ValidatedIoPlan`，删除当前再次调用 Validator、再次读 conf 的做法 |
| `init --profile`、`edit`、`fix-deps`、Studio 导入/保存 | 保留完整 `deployment`；图编辑只改图字段，不能重新序列化时丢掉部署信息。空白草稿可暂不含 deployment，但不得标为可运行 |
| Demo | 原样调用 SDK；只调整 fixture 与配置定位，不代替 SDK 解析 binding、字段选择或响应组装 |

算法规划阶段与部署阶段共用纯结构/语义校验，后者额外有文件定位、路径 containment 和
宿主深度预算。不要通过虚构当前目录作为模型根来让 `validate` 成功。
编辑工具的草稿路径允许尚未满足完整运行条件，但保存和运行必须明确反馈缺失项。

错误需包含源文件与 JSON Pointer；stdin 输入标记为 stdin。沿用现有 `DiagnosticCode`
及 CLI 报告结构，Integration 解析失败使用相应的字段/组合诊断，注册与预算错误保持稳定含义。
公开 Operator 的返回码分类和 `GetOperatorLastError()` 入口不变，所有异常屏障保留。

| 错误示例 | 诊断定位 |
| --- | --- |
| conf 留有旧字段 | conf 的 `/data`、`/schema_version` 或对应非法键，提示迁移到新结构 |
| 缺少配对字段 | JSON 的 `/deployment/io/io_binding` 或 `/deployment/io/output_allocations` |
| 未知 binding / biz 不匹配 | `/deployment/io/io_binding`，不匹配时同时说明根 `/biz_name` |
| 未知/缺失外部槽位 | `/deployment/io/output_allocations/<slot>` |
| 类型、allocator、容量错误 | 上述槽位下对应字段 |
| 模型覆盖 ID/路径错误 | `/deployment/model_paths/<model_id>`；未覆盖时为 `/models/<index>/model_path` |
| Node 输出映射错误 | 保持 `/pipeline/<index>/ports/outputs/<port>` |

包含 `/` 或 `~` 的键按 JSON Pointer 规则转义。覆盖路径经过投影后出错时须映射回原始
`deployment.model_paths` 位置，不能只报合成文档中的 `/models/...`。

`resolve-conf.configuration.model_paths[*].source` 从 `conf.data.model_paths` 改为
`pipeline.deployment.model_paths`；默认路径来源仍为 `pipeline.models.model_path`。
`output_pools`、`output_pool` 是已有**诊断报告字段**，本次保留其名字和参数文本语义，
不要与持久化文件的新字段 `output_allocations` 混改。`effective_pipeline` 继续是供
Core 使用的中性生效文档，包含已解析路径，不作为可部署源文件保存。

### 2.7 选择原因与代价

采用 `deployment.io` 将 binding 和分配表放在一起，`deployment.model_paths` 则与
它们同属部署信息。Integration 可以一次拆出明确的子文档，Core 无需放宽根字段白名单。

未采用把三个字段平铺到 JSON 根或仅将 `outputs` 改为 `operator_outputs`：这些形式
无法在结构上表达 I/O 必须整体继承，后者也没说明是分配参数。未采用把部署配置全部塞进
`models` 或 Node：这会混入不属于模型或算法节点的外部载体责任。

代价是同一个算法若有不同模型覆盖或接入配置，需要不同完整 JSON；不再能只靠多个
`.conf` 定制。同一模型 ID 的默认路径与部署覆盖仍可能同时存在，因此必须提供清晰的
生效来源报告。是否进一步取消 `model_paths`、引入配置复用机制，不属于本次迁移。

## 3. 兼容与迁移

### 3.1 一次切换与回退边界

新代码只接收新 `.conf`；部署 JSON 必须含完整 `deployment.io`。旧配置不自动升级，
旧字段与新字段并存直接拒绝，不能建立“conf 优先/JSON 优先”的永久双来源规则。
现有中性 Core fixture 无需凭空增加部署配置，但凡经 Operator 部署的 fixture 必须迁移。

这是一项持久化配置破坏性变更，须与运行库、工具、配置包同时发布。
它本身不改变 C++ Operator ABI，不要求仅为移动配置字段再次调整共享库 ABI major。
回退时整体恢复旧运行库、工具与配置包，不能单独回退 `.conf` 或 JSON。

### 3.2 按关联关系迁移，不能按字段名全局替换

对每个旧 `.conf`，定位真实目标 JSON，并执行确定映射：

| 旧来源 | 新位置 / 动作 |
| --- | --- |
| `data.pipe_path` | `.conf.pipe_path`；若目标被拆分则改为新文件名 |
| `data.io_binding` | JSON 的 `deployment.io.io_binding` |
| `data.outputs` | JSON 的 `deployment.io.output_allocations`；槽位内部值原样迁移 |
| `data.model_paths` | JSON 的 `deployment.model_paths`；原来省略则继续省略 |
| `.conf.schema_version`、`.conf.data` | 删除包装 |
| `models[*].model_path`、`pipeline[*].ports.outputs` | 原样保留 |

先列出“JSON 被哪些 conf 引用”，再写文件：

1. 多份 conf 引用同一个 JSON 且部署配置完全相同，可以继续共用该 JSON。
2. 若 binding、分配表或模型覆盖不同，复制成不同部署 JSON，并分别修改 `pipe_path`。
   不能顺序覆盖同一个 JSON 的 `deployment`，也不能只保留最后一份配置。
3. `tests/fixtures/pipelines/cross_rerank/` 的正常/缺失模型场景就是需单独核查的共用目标。
   拆分后要保留原负例的失败原因，不能因新格式不合法提前失败而使测试假通过。
4. 若目标已有 `deployment`，迁移工具遇到内容冲突应停止并报告；禁止静默覆盖。
   一次性迁移脚本可以用于仓库改造，不能进入运行时作为兼容解析器。

### 3.3 实施检查清单

| 范围 | 必须完成的迁移 |
| --- | --- |
| `configs/`、`demo/fixtures/mock/`、`tests/fixtures/pipelines/` | 全量迁移部署文件及关联 JSON，包括无模型、多模型和负例 |
| `tests/support/control_test_utils.h` 及各测试内 JSON 字面量 | 临时配置构造器改为生成新文件对；保留有意构造的旧格式拒绝测试 |
| `tools/verify_selection.py` | 将 `build_run_conf` 收窄到定位文件或改造成明确生成文件对的 helper；`effect_inputs`/临时执行从 JSON 读取部署信息，更新指纹和来源 |
| `tools/pipeline_studio/server.py` | 迁移 `profile_inputs`、`deployment_candidate`、save/staging/run；完整继承 I/O，保留模型 `select_asset/preserve_override` 意图和失效 ID 清理 |
| `scripts/dev_recipe.py` | Profile 解析、临时配置和最终产物都迁移；保留目前明确的单外部输出槽位限制，复杂方案走已有相应路径 |
| `src/tools/pipeline_authoring.*` 与 Studio 前端 | 算法编辑往返保留部署子文档；外部分配编辑与节点端口显示分开 |
| 活动指南、README 中相关示例、开发 skills | 更新配置字段路径、生成步骤和“当前方案”运行方式；历史 RFC 保留原文，由本文声明取代范围 |

工具已有的 `BIZ_TO_OPERATOR_BINDING` 等选择代码不能用于为已选文档补齐缺失 binding。
生成新方案需从显式选择或 Catalog/Profile 取得完整 `io`，再交原生校验器验证，不能
用业务名猜 binding 后拼接另一份输出表。Profile 的数据集、Demo 运行选项仍属于 Profile。

Studio 仍需保护 JSON/conf 的关联和并发 revision：conf 虽变小，仍可被另一进程切换
`pipe_path`。保存时保留既有冲突检查和成对 staging/失败恢复，校验成功才提交候选文件；
临时执行只改候选 conf 的定位字段，不能回读旧 conf 的部署参数覆盖当前编辑内容。
效果评估指纹必须包含当前 JSON 中生效的模型覆盖、binding 与分配表，避免复用旧结果。

实施完成后更新当前配置指南和 `doc/CHANGELOG.md`。本次仅设计，不提前把拟议格式
写成已支持功能，也不为尚未实现的能力增加发布记录。

## 4. 验证与完成条件

扩展现有责任套件，不为此另建测试可执行程序。以下是实施验收项，不是本次文档提交
已经通过的新行为测试：

| 验证项 | 最小证据与既有落点 |
| --- | --- |
| conf 严格结构 | 单字段通过；旧包装、旧键、空/错误类型、未知键拒绝；`tests/unit/adapter/test_io_binding_registry.cpp` |
| 配对与槽位 | 缺 binding/分配表、未知 binding、biz/transport 不匹配、缺必需/多未知槽位失败；可选槽位、多槽位、同键不同 allocator 行为保持；`test_io_binding_registry.cpp`、`tests/unit/adapter/test_adapter_purity.cpp`、`tests/integration/operator/test_operator_api.cpp` |
| 输出分配契约 | 类型、参数、容量、元数据、有效深度预算、分配失败回滚和只解析一次的原测试继续通过；诊断指向新源位置；沿用 Operator suite |
| 模型覆盖 | 覆盖/默认优先级、未知 ID、空值、非法原模型声明、无模型、多个模型；绝对/相对/逃逸/符号链接路径；同一根下改 JSON/conf 目录及 CWD 后结果不变；沿用 resolver/Operator suite |
| Core 边界 | 对中性投影规划与迁移前一致；完整文档误送 Core 仍拒绝 `deployment`；未知根键不被过滤；`tests/unit/core/test_pipeline_config.cpp` 与现有分层检查 |
| 公共入口 | `Create`、`ValidateOperatorConfigBinding`、`validate-io`、`resolve-conf` 对同一输入得到一致配置结果；失败在加载/分配前；`tests/contract/abi/test_cpp_operator_sdk.cpp`、`test_operator_safety.cpp`、`test_adapter_contract_security.cpp` |
| CLI 与诊断 | `validate/plan/--stdin/--explain` 支持完整文档且拒绝半组 I/O；纯算法文档仍可校验但不能部署；source 和 JSON Pointer 正确；扩展现有 CLI/tooling 测试 |
| 生成、编辑与保存 | init/profile、edit、fix-deps、Studio 往返不丢 deployment；换 binding 不混配、模型编辑意图和双文件冲突保护有效；`tests/tooling/test_pipeline_studio.py`、`studio_browser_test.mjs`、`test_dev_recipe.py` |
| 效果评估与共享 fixture | 参数变更令指纹失效，临时运行使用当前 JSON；共用 JSON 的不同部署拆分后仍各自成立，负例仍在预期阶段失败 |
| 完整 SDK 执行 | 用迁移后的无模型关键词方案直接 `Create/Process`，校验 `keyword_out`、请求 ID 与结果；再运行同配置 Demo；沿用 Operator 与 `tests/integration/demo/test_demo_runner.cpp` |

所有注册能力与字段以实施时构建的 Catalog 为准。完成迁移后，对官方配置和有效 fixture
执行已有工具链的 `validate`、`plan`、`validate-io`、`resolve-conf` 检查；对负例验证其
预期错误。以下是单个方案的操作形式，不替代测试矩阵：

```bash
./build/alg_pipeline_tool catalog --biz dialogue_compliance_audit_v1
./build/alg_pipeline_tool validate configs/pipeline_dialogue_audit_default.json
./build/alg_pipeline_tool plan configs/pipeline_dialogue_audit_default.json
./build/alg_pipeline_tool validate-io configs/pipeline_dialogue_audit_default.conf --model-root .
./build/alg_pipeline_tool resolve-conf configs/pipeline_dialogue_audit_default.conf --root . --depth 1
```

最后运行一次 [CONTRIBUTING.md](../../CONTRIBUTING.md#6-run-one-canonical-delivery-gate)
规定的 `./scripts/run_all_tests.sh`。本次迁移不改变模型推理，不要求下载新模型来证明字段
搬迁；没有真实模型/硬件时，静态解析与 Mock/无模型执行不得表述为真实业务效果验收。

## 5. 实施与最终结果

按下面顺序实施，每阶段直接更新本文状态和检查项，不另写重复计划：

- [ ] **阶段 1：解析与配对。** 收窄 conf，以轻量私有函数拆分 JSON；文件与 JSON 入口
  共用严格 I/O 校验，编辑原文由工具持有，覆盖新格式、严格拒绝与中性 Core 边界测试。
- [ ] **阶段 2：运行时闭环。** 应用 JSON 模型覆盖，保留路径/输出预算和异常屏障；
  Create、绑定预检共用不可变计划，完成公共 SDK 正负例。
- [ ] **阶段 3：工具迁移。** 完成 CLI/Studio/recipe/效果评估的读写与诊断；
  去除再次读文件、再次规划和按 biz 猜配对的路径，完成编辑/保存/指纹回归。
- [ ] **阶段 4：文件与指南迁移。** 按 conf→JSON 关联表迁移所有部署场景，拆分冲突共享文件，
  修正动态 fixture、活动示例和 skills，完成直接 SDK 与 Demo smoke。
- [ ] **阶段 5：验收。** 独立复核分层、路径、配对和负例含义；完成上述检查与 canonical gate，
  更新 CHANGELOG、本文及索引状态。

开始实施时将状态改为 `In Implementation`；仅在配置、工具、测试、文档全部迁移并验证
后改为 `Completed`。本次设计核查已查询对话审核和关键词的 Catalog，并验证现有对话
审核 JSON 与关键词计划；这些证据用于确认示例和实施落点，不代表新格式已被实现。
