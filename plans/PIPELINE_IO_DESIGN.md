# Pipeline I/O 改造设计

> **状态**：设计已确认，待实施。
> **基线**：`main@cda1f5c`（PR #183 已合入 Engine 源码自动收录和 `Init` 列出注册冲突原因）。
> **实施**：按[实施总说明](PIPELINE_REFACTOR_GUIDE.md)的步骤进行；本文中关于 PR 划分和门禁的说法与总说明不一致时，以总说明为准。
> **性质**：跨多个 PR 的工作计划，规则见 [plans/README](README.md) 与 [CONTRIBUTING](../CONTRIBUTING.md) §3。
> 各阶段 PR 的描述引用本文对应章节；最后一个阶段合入时删除本文，形成的规则写进现行指南。
> **目标**：简化系统、降低开发心智、提升易用性。每项改动都应能落到下面某一条：概念更少、写法只有一种、名字一看就懂、出错更早。
> **不做兼容**：项目尚未发布，没有兼容使用方。只实现新方案：不保留旧字段别名、不识别旧格式、不提交迁移代码、不写针对旧名字的测试（见 6.4）。
> **不在范围**：
> - `.conf` 与 `pipe_path`：外部契约，不改；
> - Operator 函数表、宿主结构体的布局、宿主 map key 规则。`service_type` 的真实成员与取值进内网后核对（5.1）；
> - 模型、后端参数：见 [Pipeline 模型配置改造设计](PIPELINE_MODEL_DESIGN.md)；
> - 数据连接方式与 converter 端口名：[节点设计](PIPELINE_NODE_DESIGN.md)把连线统一改为"节点名.端口名"引用，本文不改（4.6）。

## 1. 目标

改造完成后：

1. Pipeline JSON 在根层用 `io` 列出输入项和输出项。每项写宿主结构体 `type` 和业务 `name`，需要时用 `params` 覆盖参数。
   `deployment`、`io_binding`、`out_mem` 全部删除。
2. converter 按（结构体，业务）登记。`type` 是宿主 map key 的后缀；`name` 对应结构体成员 `service_type` 的一个取值，每个请求都核对。
3. converter 的参数与节点同一种写法：默认值写在代码里，配置可以覆盖，Create 阶段可以做准备，Process 阶段只读使用。
   输出内存尺寸也是 converter 参数。
4. 删除 Core 中的"业务"层：`IoBinding`、`BizDefinition`、Core 的 `biz_name`。
   Core 只按 IO 边界做校验，即输入 converter 发布的端口和输出 converter 需要的端口。业务名只在接入层使用：选择 converter、核对 `service_type`。
5. Demo 按宿主结构体构造请求、显示结果，不再按业务分派。

改造后的配置：

```json
{
  "io": {
    "input":  [ { "type": "doc_in",  "name": "doc_qa" } ],
    "output": [ { "type": "doc_out", "name": "doc_qa" } ]
  },
  "models":   [],
  "pipeline": []
}
```

对开发者的变化：

| | 现在 | 改造后 |
| --- | --- | --- |
| 方案里的 I/O | `deployment.io.io_binding`（业务名）加可选的 `out_mem`（结构体字段路径） | `io.input`、`io.output` 列出所用的（结构体，业务），需要时覆盖参数 |
| 要理解的概念 | 业务（`BizDefinition`）、binding、converter、`out_mem`、结构体默认尺寸 | 按（结构体，业务）登记的 converter 及其参数、平台结构登记 |
| 新方案复用已有格式 | 找到对应业务名 | 直接写（结构体，业务） |
| 新的载荷格式 | 写 converter，再写 `BizDefinition`、`IoBinding`、Demo runner | 新增一个（结构体，业务）登记；结构体已有时 Demo 不用改 |
| 请求属于哪个业务 | 不核对 | 每个请求核对 `service_type` |
| 输出尺寸 | 平台结构给默认值，方案用 `out_mem` 按字段路径覆盖 | converter 给默认值，方案按参数名覆盖 |
| 参数写法 | 节点一种，`out_mem` 另一种 | 节点、converter 同一种 |
| 配错的发现时机 | 嵌套布局配错要到 Process 才发现；未被绑定的 converter 不做结构审计 | 布局随 converter 固定，不会配错；所有 converter 都在 `Init` 时审计 |

## 2. 现状与问题

| 编号 | 问题 | 依据 |
| --- | --- | --- |
| P1 | 为表达一个名字套了三层：`deployment` 只允许含 `io`，`io` 里只有 `io_binding` 和可选的 `out_mem` | `src/adapter/pipeline_document.cpp`。`deployment` 原来还有 `model_paths`，后来移到 `models[].model_path` |
| P2 | `io_binding` 就是业务名：每个业务恰好一个 binding，按业务名查找 | `IoBindingRegistry::FindBinding(biz_name)` |
| P3 | 端口契约写了两遍：8 个 `BizDefinition` 的入口/出口与所选 converter 的端口逐字段相同，IoBinding 审计专门比较这两份 | `src/adapter/biz/*_bindings.cpp`；`IoBindingRegistry::Audit`；`alg_pipeline_tool catalog` 实测 |
| P4 | 业务名被来回搬运：先从 Core 结构里删掉 `biz_name` 并加上 `deployment`；准备阶段再把 `biz_name` 写回 Core 文档；最后把诊断路径 `/biz_name` 改回 `/deployment/io/io_binding` | `src/adapter/deployment_structure.cpp`；`src/adapter/deployment_preparation.cpp` 的 `PrepareDeploymentDocument` 与 `ProjectDeploymentDiagnostics` |
| P5 | 同一边界检查两遍：Validator 先按业务的入口/出口查一遍，再按 converter 端口（`PipelineIoBoundary`）查一遍 | `src/core/pipeline_validator.cpp` |
| P6 | Core 依赖业务概念：`biz_name` 必填、Core 持有业务注册表；Core 测试要往全局注册表登记假业务 | `src/core/pipeline_config_structure.cpp`、`src/core/pipeline_catalog.cpp`、`tests/support/pipeline_test_utils.h` |
| P7 | binding 上的批次上限：8 个 binding 全部使用默认值 64 | `include/adapter/io_binding.h` |
| P8 | 输出默认尺寸挂在平台结构登记上，但该写多少取决于 converter 的载荷。同一个 `CompanyOperatorEntityOutput`，实体抽取 2047 字节够用，翻译需要 8191，只能在方案里写 `out_mem` 覆盖 | `src/adapter/operator/operator_builtin_value_types.cpp`；`configs/pipeline_translate_cpu.json` |
| P9 | 嵌套布局由部署配置选择、由 converter 按布局写入。Create 时不核对二者是否匹配，converter 只能在运行时自查 `spec.allocator` | [输出分配指南](../doc/dev_guide/operator_output_allocation.md)："Create 时框架不核对转换器是否支持所选方案" |
| P10 | 参数有两种写法：节点参数由定义声明（类型、默认值、范围、说明，Catalog 可见）；`out_mem` 则是结构体字段路径，加一段交给布局自行解析的 JSON 文本 | `OperatorConfigResolver::ResolveOutputAllocation` |
| P11 | converter 没有参数机制，也无法在 Create 阶段做准备 | `include/adapter/io_converter.h` |
| P12 | converter 名看不出方向，后缀含义混杂（`.plain`、`.result`、`.structured`、`.pcm`、`.json`）；`translate.json` 同时是输入和输出 converter 的名字 | `alg_pipeline_tool catalog` |
| P13 | Demo 按业务分派：SDK 导出函数 `ResolveOperatorConfigBiz` 只为 Demo 取业务名；7 个 runner 各写一遍创建、Control、分批、写结果的流程；默认 Control 命令写死在 runner 里 | `demo/main.cpp`、`demo/biz/`、`demo/common/operator_runner.h` |
| P14 | 真实平台的每个宿主结构体都有成员 `service_type`（外部契约），标明请求属于哪个业务。现在的 converter 既不读取也不核对它，平台模拟中也没有这个成员 | 用户说明；`include/platform_mock/operator_data_types.h` |

## 3. 职责划分

每一类事实只有一个归属：

| 事实 | 归属 | 代码位置 |
| --- | --- | --- |
| 宿主结构体的布局、`request_id` 与 `service_type` 成员的位置、每个字符串字段的平台硬上限、分配/重置/释放 | 平台结构登记（ValueType），每种结构集中登记一次 | `src/adapter/operator/operator_builtin_value_types.cpp` |
| 特殊嵌套布局 | 命名布局（代码中称 allocator），在自己的 `.cpp` 中登记 | `src/adapter/output/` |
| 一种（结构体，业务）的载荷格式：怎样解析请求、怎样组装响应；业务名对应的 `service_type` 取值；输出槽用哪种布局 | converter 登记 | `src/adapter/input/`、`src/adapter/output/` |
| converter 参数（含输出尺寸）的默认值、Create 阶段的准备 | converter | 同上 |
| 用哪些（结构体，业务）、覆盖哪些参数 | 方案配置的 `io` | Pipeline JSON |
| 数据流是否成立 | Core，只看 IO 边界 | `src/core/pipeline_validator.cpp` |
| 输出池预分配份数 | 宿主的 Create 参数 `max_frame_depth` | 不变 |
| 单次 Process 的条数上限 | 框架常量 `kMaxProcessBatchSize = 64`，再按池深收紧 | 新常量，替代 binding 上的值 |

`io` 由接入层独占解析。接入层把根对象中的 `io` 去掉后交给 Core，同时传入 IO 边界。Core 中不再有"业务"概念：业务名只在接入层用于选择 converter、核对 `service_type`。四层依赖方向不变。

## 4. 配置格式

### 4.1 根层字段

| 字段 | 必填 | 归属 | 说明 |
| --- | --- | --- | --- |
| `io` | 是 | 接入层 | 见 4.2 |
| `models` | 否 | Core | 不变 |
| `pipeline` | 是 | Core | 不变 |
| `max_parallel_workers` | 否 | Core | 不变 |
| `comment` | 否 | Core | 不变 |

根层只允许以上字段，其他字段按未知字段报错。

### 4.2 `io` 的规则

| 位置 | 规则 |
| --- | --- |
| `io` | 必填对象，只允许 `input` 和 `output` 两个键 |
| `io.input`、`io.output` | 必填数组，至少一项；多项的规则见 5.5 |
| 每一项 | 对象，只允许 `type`、`name`、`params` 三个键 |
| `type` | 必填。宿主结构体，即宿主 map key `xxx.yyy` 中的 `yyy`，例如 `doc_in` |
| `name` | 必填。业务，对应结构体成员 `service_type` 的一个取值，例如 `doc_qa`。`common` 是保留值，表示该结构体的默认处理（5.1） |
| `params` | 可省略，省略表示全部使用默认值。缺省的项补默认值；未知参数、类型错误、超出范围、不在枚举内都报错；之后执行 converter 的 `Prepare` 和 `Validate` |
| 唯一性 | 同一侧的（`type`, `name`）不能重复。外部保证同一业务中不会出现两个相同的（结构体，`service_type`），校验器再检查一次 |

两侧用数组，而不用以名字作键的对象，原因有三点：
- 能唯一确定一项的只有（`type`, `name`）这一对，单用其中一个作键都可能重复：图片问答的两项业务名相同；同一种结构体可以承载不同业务。
- JSON 对象的重复键会被静默覆盖，写错了查不出来。
- 与 `models`、`pipeline` 的写法一致。

### 4.3 诊断

| 情况 | 诊断码 | 路径 |
| --- | --- | --- |
| 结构错误：缺少 `io`/`input`/`output`、不是数组、数组为空、某项不是对象、有其他键、缺少 `type` 或 `name` | `DEPLOYMENT_ERROR`（与现在的结构错误相同） | 指向出错位置，如 `/io/input/0/name` |
| （`type`, `name`）未登记（包括方向用错） | `UNKNOWN_CONVERTER`（新增） | `/io/input/<i>` 或 `/io/output/<i>`。`type` 已登记时列出它的业务，否则建议相近的 `type` |
| 同一侧（`type`, `name`）重复 | `DUPLICATE_IO_ENTRY`（新增） | 后出现的一项，如 `/io/input/1` |
| 多个输入项发布了同名端口 | `DUPLICATE_PORT_PRODUCER`（已有） | 后出现的一项 |
| 所有输入项的结构体都不带 `request_id`（5.5） | `INVALID_COMBINATION` | `/io/input` |
| 参数未知、类型错误、超出范围、不在枚举内 | 与节点相同：`UNKNOWN_CONFIG_FIELD`、`CONFIG_FIELD_TYPE`、`CONFIG_FIELD_RANGE`、`CONFIG_FIELD_ENUM` | `/io/input/<i>/params/<参数>` 或 `/io/output/<i>/params/<参数>` |
| `Prepare`/`Validate` 失败 | `INVALID_COMBINATION`（与节点、模型的语义校验相同） | `/io/input/<i>/params` 或 `/io/output/<i>/params` |
| 尺寸参数超过平台上限 | `CONFIG_FIELD_RANGE` | `/io/output/<i>/params/<参数>` |
| 句柄的输出池总预算超限 | `INVALID_OUTPUT_ALLOCATION`（保留） | `/io/output` |

运行时（Process）有两种情况整批按输入非法处理：
- 宿主结构体的 `service_type` 与该项业务的取值不一致；
- 多项输入的 `request_id` 不一致。

两种情况的错误码与结构校验失败相同，错误信息写明结构体、序号和期望值。

删除的诊断码：`UNKNOWN_IO_BINDING`、`UNREGISTERED_CONVERTER`、`UNKNOWN_OUTPUT_SLOT`，以及 Core 的 `UNKNOWN_BIZ`。

改名的诊断码：`MISSING_BIZ_OUTPUT` 改为 `MISSING_OUTPUT_PRODUCER`，与已有的 `MISSING_INPUT_PRODUCER` 对称；修复原因 `missing_biz_output` 改为 `missing_output_producer`。

### 4.4 示例

```json
// 大多数方案：参数全部使用 converter 默认值
"io": {
  "input":  [ { "type": "doc_in",  "name": "doc_qa" } ],
  "output": [ { "type": "doc_out", "name": "doc_qa" } ]
}

// 翻译：与实体抽取用同一种宿主结构体，业务不同。8191 是翻译输出自带的默认值，不用写
"io": {
  "input":  [ { "type": "entity_in",  "name": "translate" } ],
  "output": [ { "type": "entity_out", "name": "translate" } ]
}

// 需要更长的回答：只写要覆盖的项
"io": {
  "input":  [ { "type": "doc_in",  "name": "doc_qa" } ],
  "output": [ { "type": "doc_out", "name": "doc_qa", "params": { "answer_text_max_bytes": 4095 } } ]
}

// 图片问答：按平台模拟，图片和问题是两个宿主结构体，属于同一个业务
"io": {
  "input":  [ { "type": "frame",  "name": "ocr_invoice_qa" },
              { "type": "string", "name": "ocr_invoice_qa" } ],
  "output": [ { "type": "od_out", "name": "ocr_invoice_qa" } ]
}
```

如果内网中图片和问题放在同一个结构体里，`io.input` 只写一项，该（结构体，业务）的 converter 从两个成员分别读出图片和问题。两种情况都不需要改框架。

### 4.5 现有配置的迁移规则

在本地一次性迁移。迁移脚本不提交，也不保留旧格式兼容（CONTRIBUTING §3）。

1. 按下表把 `deployment.io.io_binding` 换成 `io.input` 和 `io.output`。表中是各项的 `type`；各项的 `name` 都是原业务名。
2. `deployment.io.out_mem.<槽>.capacities.<字段> = n`：如果 n 不等于该项 converter 的默认值，写成该输出项的 `params.<字段>_max_bytes = n`；否则删除。
   仓库里只有翻译方案写了 8191，它等于新默认值，所以直接删除。
3. `out_mem` 的其他字段（`allocator`、`params`、`meta_num`、`metadata_type_id`）只出现在测试中，改为在测试 converter 的槽声明里写固定值，见 5.4。
4. `io` 放在根对象的第一个位置。

| 原 `io_binding` | `io.input` | `io.output` |
| --- | --- | --- |
| `audio_asr_intent` | `audio_in` | `audio_out` |
| `cross_rerank` | `rerank_in` | `rerank_out` |
| `dialogue_audit` | `audit_in` | `audit_out` |
| `doc_qa` | `doc_in` | `doc_out` |
| `entity_extract` | `entity_in` | `entity_out` |
| `keyword_match` | `keyword_in` | `keyword_out` |
| `ocr_invoice_qa` | `frame`、`string` 两项 | `od_out` |
| `translate` | `entity_in` | `entity_out` |

需要迁移的文件：
- `configs/*.json`（17 个，包括 kite 方案）；
- `demo/fixtures/mock/*.json`（9 个）；
- `tests/fixtures/pipelines/` 下的 JSON（含 `validation/invalid_pipeline_cases.json`）；
- `tests/fixtures/effects/*.json`（见 8.5）；
- C++、Python、JS 测试中内嵌的配置文本；
- 文档里的配置示例。

### 4.6 与节点连线的关系

本设计不改变数据的连接方式：
- 输入 converter 仍按固定的数据名发布数据；
- 输出 converter 仍按固定的数据名读取；
- 节点按这些数据名接线。

[节点设计](PIPELINE_NODE_DESIGN.md)的阶段 2 会把连线统一改为"节点名.端口名"引用：
- 节点用 `input.<端口>` 引用输入项发布的数据；
- 输出项增加 `inputs`，写明每个端口的数据来自哪个节点的哪个端口；
- converter 的端口改为逻辑名，不再依赖 `adapter/biz_blackboard_keys.h` 中的固定数据名。

这部分改动由节点设计负责（其第 4 节、附录 B）。

## 5. Converter

### 5.1 按（结构体，业务）登记

每个 converter 登记对应一种（结构体，业务）：

| 登记项 | 说明 |
| --- | --- |
| `type` | 宿主结构体，即宿主 map key 的后缀（`rag_channel.doc_in` 中的 `doc_in`），与 ValueType 后缀、槽名相同 |
| `name` | 业务名，例如 `doc_qa`。方案中 `io` 各项的 `name` 写的就是它 |
| `service_type` | 业务名对应的 `service_type` 取值。`common` 不填；结构体没有 `service_type` 成员时（如 `CompanyString`）也不填 |
| 端口、参数、转换函数 | 与现在相同 |

各项规则：
- **分发**：Create 时按方案中的（`type`, `name`）选中登记；每个请求在调用转换函数之前，由框架核对宿主结构体的 `service_type` 是否等于登记的取值（登记没有取值时不核对）。效果相当于按 `service_type` 写 switch 分发，但新增一个业务只需新增一个登记，不用修改已有的分发代码。
- **同一结构体的多个业务**：各自登记，解析结构体的公共代码抽成函数共用。例如 `entity_in` 有 `entity_extract`、`translate` 两个业务。
- **`common`**：保留的业务名，表示该结构体的默认处理，相当于 switch 的 `default` 分支。框架只约定两点：
  - 方案写 `"name": "common"` 时，选中（`type`, `common`）这个登记，不核对 `service_type`；
  - 写了未登记的业务名时报 `UNKNOWN_CONVERTER`，不会退回 `common`，以免拼写错误被悄悄吞掉。

  各结构体的 `common` 登记由用户后续补充，本设计不新增。
- **输出项**：converter 写出时，把输出结构体的 `service_type` 设为登记的取值。
- **`service_type` 与 `request_id` 的位置**：由平台结构登记声明。
  - 平台模拟中，给各业务结构体和 `CompanyFrame`、`CompanyOdOutput` 加上 `int32_t service_type` 成员，取值用占位枚举。
  - `CompanyString` 同时被其他结构体用作字段，保持不变；它作为宿主值时不核对 `service_type`，所以 `string/ocr_invoice_qa` 登记不填 `service_type`。
  - 真实的成员和取值进内网后按真实头文件核对，见[平台定义模拟说明](../include/platform_mock/README.md)。

现有 converter 改为如下登记：

| 方向 | 源文件 | 现名 | `type` | `name` | 说明 |
| --- | --- | --- | --- | --- | --- |
| 输入 | `src/adapter/input/audio_input.cpp` | `audio.pcm` | `audio_in` | `audio_asr_intent` | |
| 输入 | `src/adapter/input/audit_input.cpp` | `audit.plain` | `audit_in` | `dialogue_audit` | |
| 输入 | `src/adapter/input/doc_query_input.cpp` | `doc_query.plain` | `doc_in` | `doc_qa` | |
| 输入 | `src/adapter/input/text_input.cpp` | `keyword.plain` | `keyword_in` | `keyword_match` | |
| 输入 | `src/adapter/input/rerank_input.cpp` | `rerank.plain` | `rerank_in` | `cross_rerank` | |
| 输入 | `src/adapter/input/text_input.cpp` | `text.plain` | `entity_in` | `entity_extract` | `sentence_text` 是纯文本 |
| 输入 | `src/adapter/input/image_query_input.cpp` | `image_query.plain` | `frame`，`string` | `ocr_invoice_qa` | 拆成两个登记，分别读图片和问题，按 5.5 配对 |
| 输入 | `src/adapter/input/translate_json_input.cpp` | `translate.json` | `entity_in` | `translate` | `sentence_text` 中是 `{"query": ...}` |
| 输出 | `src/adapter/output/audio_result_output.cpp` | `audio_result.plain` | `audio_out` | `audio_asr_intent` | |
| 输出 | `src/adapter/output/audit_result_output.cpp` | `audit_result.plain` | `audit_out` | `dialogue_audit` | |
| 输出 | `src/adapter/output/doc_answer_output.cpp` | `doc_answer.plain` | `doc_out` | `doc_qa` | |
| 输出 | `src/adapter/output/keyword_result_output.cpp` | `keyword.result` | `keyword_out` | `keyword_match` | |
| 输出 | `src/adapter/output/rerank_result_output.cpp` | `rerank_result.plain` | `rerank_out` | `cross_rerank` | |
| 输出 | `src/adapter/output/invoice_result_output.cpp` | `invoice_result.plain` | `od_out` | `ocr_invoice_qa` | |
| 输出 | `src/adapter/output/structured_document_output.cpp` | `document.structured` | `entity_out` | `entity_extract` | 把结构化文档写成 JSON |
| 输出 | `src/adapter/output/translation_json_output.cpp` | `translate.json` | `entity_out` | `translate` | 写入 `{"translated": ...}` |

`type` 跟随宿主载体名。[平台定义模拟说明](../include/platform_mock/README.md) 写明，`CompanyOperator*` 系列结构体和 key 后缀约定都是外网替身，进内网后按真实头文件核对，`type` 随之调整。

### 5.2 定义结构

```cpp
// include/adapter/io_converter.h（改造后的相关部分）
inline constexpr size_t kMaxProcessBatchSize = 64;  // 替代 kDefaultIoBindingMaxBatchSize
inline constexpr char kCommonIoName[] = "common";   // 保留的业务名（5.1）

struct ExternalSlotDefinition {
  std::string type_id;      // 宿主结构名，例如 "CompanyOperatorDocInput"
  std::string type_suffix;  // 宿主 map key 后缀，即登记的 type
  bool required = true;
  // 以下仅用于输出槽，由 converter 固定，不进入配置：
  std::string allocator;         // 空：结构体的标准布局；非空：命名布局
  std::string allocator_params;  // 命名布局的参数（JSON 文本），注册审计时解析
  uint32_t metadata_count = 0;   // 结构体带 metadata 成员时分配的元素数
  int32_t metadata_type_id = 0;
};

struct InputConverterDefinition {
  std::string type;                     // 宿主结构体（map key 后缀）
  std::string name;                     // 业务名；kCommonIoName 表示默认处理
  std::optional<int32_t> service_type;  // name 对应的取值；common 或结构体没有该成员时为空
  ExternalSlotDefinition slot;          // 取代 external_slots：一个登记只对应一个结构体
  std::vector<NodePortDefinition> logical_ports;
  ParameterSet params;  // 新增；默认没有参数
  DecodeInputFn decode_fn = nullptr;
};

struct OutputConverterDefinition {
  std::string type;
  std::string name;
  std::optional<int32_t> service_type;
  ExternalSlotDefinition slot;
  std::vector<NodePortDefinition> logical_ports;
  ParameterSet params;  // 新增
  EncodeOutputFn encode_fn = nullptr;
};

struct InputDecodeOptions {
  std::string type;
  std::string name;
  // 结构体带 request_id 时，converter 按行写入请求 ID；不带时保持为空（5.5）
  std::vector<uint64_t>* request_ids = nullptr;
  const ParameterValues* params = nullptr;  // 新增：本句柄解析好的参数
  template <typename P>
  const P& Params() const;  // 等价于 params->Get<P>()
};
// OutputEncodeOptions 同样新增 params 和 Params<P>()。
```

`converter_id` 由（`type`, `name`）取代；日志和报告中写作 `type/name`，例如 `doc_in/doc_qa`。

```cpp
// include/contracts/parameter_set.h（新增；converter、模型、后端共用，见模型配置设计 5.2）
// Create 阶段生成，同一句柄的所有调用只读共享。
class ParameterValues {
 public:
  template <typename P>
  const P& Get() const;  // 类型与声明不符时抛出 std::logic_error，由 Operator 的异常屏障转成错误码
  std::optional<int64_t> Integer(const std::string& name) const;  // 框架按参数名读取尺寸
  const nlohmann::json& Effective() const;  // 生效值，供 resolve-conf、validate-io 报告
};

class ParameterSet {
 public:
  ParameterSet() = default;  // 没有参数：配置中不能写任何参数
  template <typename P>
  ParameterSet(Parameters<P> spec);  // NOLINT：允许 def.params = ParamSpec();
  const std::vector<ConfigFieldDefinition>& Fields() const;
  // 依次执行：按 Fields() 校验并补齐默认值、字段赋值、Prepare、Validate、生成生效值。
  // config 可以是原始配置，也可以是已归一化的配置。
  bool Parse(const nlohmann::json& config,
             std::shared_ptr<const ParameterValues>* values,
             std::string* error) const noexcept;
};
```

参数容器放在 `contracts/` 并使用中性名字，是因为模型、后端也用它（见[模型配置设计](PIPELINE_MODEL_DESIGN.md) 5.2）。模型配置设计的阶段 1 先合入，新增该文件并实现 `Fields()`、`Parse`、`Get<P>()`；本设计的 C2 在其上加入 `Integer()`、`Effective()` 和 `ParameterFieldBinding::Read`。

`Effective()` 的实现方式：在 `Prepare` 之后，对每个已声明字段调用新增的 `ParameterFieldBinding::Read(const ParamsT&)`，读出成员当前值。所以 `Prepare` 修改过的值（例如推导出的尺寸）也会反映在生效值里。

`include/adapter/converter_authoring.h` 新增辅助函数 `MaxBytes`。一个登记只有一个输出槽，所以参数名不需要带槽名：

```cpp
// 尺寸参数：生成名为 <field>_max_bytes 的整数参数（最小值 1）；作者补 .Default(n)
template <typename P>
FieldBuilder<P, int64_t> MaxBytes(std::string field, int64_t P::*member);
```

参数只有一种取法：`options.Params<Params>()`。逐行回调需要参数时，在转换函数里取出后用 lambda 捕获（见 5.3），不另设辅助函数。

### 5.3 参数

**尺寸参数**：以翻译输出（`entity_out/translate`）为例，这也是本次的实际实现：

```cpp
namespace {
struct Params {
  int64_t entities_json_max_bytes{};
};

auto ParamSpec() {
  return Parameters<Params>({
      MaxBytes("entities_json", &Params::entities_json_max_bytes).Default(8191)});
}

AdapterStatus EncodeTranslation(const std::string& text, CompanyOperatorEntityOutput* out,
                                const OutputStringWriter& writer) {
  out->status_code = 0;
  return writer.Write(out->entities_json, "entities_json",
                      nlohmann::json{{"translated", text}}.dump());
}
}  // namespace

OutputConverterDefinition MakeTranslateOutput() {
  OutputConverterDefinition def;
  def.type = "entity_out";
  def.name = "translate";
  def.service_type = kMockServiceTranslate;  // 平台模拟的占位取值
  def.slot = ExternalOutputSlot<CompanyOperatorEntityOutput>("entity_out");
  def.logical_ports = {RequiredInputPort(kLlmAnswers)};
  def.params = ParamSpec();
  def.encode_fn = &EncodeTranslationJson;  // 内部调用 EncodeResultRows(..., &EncodeTranslation)
  return def;
}
REGISTER_OUTPUT_CONVERTER(MakeTranslateOutput());
```

**行为参数**：目前生产 converter 都没有行为参数。以后需要时这样写，下面只是演示：

```cpp
struct Params {
  std::string empty_policy;  // 示例：结果为空时怎么处理
};
auto ParamSpec() {
  auto params = Parameters<Params>({
      Field("empty_policy", &Params::empty_policy).Default("fail").Enum({"fail", "empty"})});
  params.Prepare([](Params* p, std::string* error) { return true; });  // Create 阶段执行一次
  return params;
}
// Process 阶段：逐行回调多带一个 const Params&
AdapterStatus EncodeRow(const std::string& text, SomeOutput* out,
                        const OutputStringWriter& writer, const Params& p);
// 转换函数里：
//   const auto& p = options.Params<Params>();
//   EncodeResultRows<SomeOutput>(..., [&p](const std::string& text, SomeOutput* out,
//                                         const OutputStringWriter& w) { return EncodeRow(text, out, w, p); });
```

规则：

| 规则 | 说明 |
| --- | --- |
| 声明 | 普通结构体 `Params`，加 `Parameters<Params>({Field(...), ...})`，与节点完全相同。每个字段必须有 `Default` 或 `Required` 之一；`std::optional` 成员两者都不写，表示可以不填（见[模型配置设计](PIPELINE_MODEL_DESIGN.md) 5.3） |
| 默认值 | 只写在代码里的 `Default(...)` |
| 覆盖 | 只认该项 `params` 中的值；没写的项用默认值 |
| Create 阶段 | 依次做字段校验和补默认值、字段赋值、`Prepare`（可修改参数，推导值和预处理结果放进不对应配置项的成员）、`Validate`（跨字段检查）。只做纯计算，不读文件、不加载外部资源 |
| Process 阶段 | 通过 `options.Params<Params>()` 拿到 `const Params&`。同一句柄的所有调用共享同一份，只读，多线程安全 |
| 边界 | 只能读本项的参数，不能读节点或模型的配置 |
| 运行中修改 | 不支持，没有对应的 Control 命令 |
| 没有参数 | 代码里不写 `params`；配置中该项也不写 `params` |

### 5.4 输出内存

**平台结构登记只保留上限**：

```cpp
// 改造前：默认值和上限
{"entities_json", &CompanyOperatorEntityOutput::entities_json, {2047, 65536}}
// 改造后：只有平台上限
{"entities_json", &CompanyOperatorEntityOutput::entities_json, 65536}
```

- `OutputCapacityFieldConfig` 只保留 `max_capacity`。
- `ResolveOutputPoolSpec` 不再填默认值；缺任何字段的尺寸都报错。注册审计保证不会出现缺失。

**尺寸参数**：

- 输出结构体的每个字符串字段，都必须有一个 `MaxBytes` 参数，参数名为 `<字段>_max_bytes`。
- 默认值必须在 1 到平台上限之间，由注册审计检查；配置值同样在 Create 时检查。
- 框架在 `Prepare` 之后按参数名读取尺寸，生成 `ResolvedOutputPoolSpec.capacities`。
- Catalog 和 schema 里，尺寸参数的 `maximum` 由框架按平台上限补齐。

现有输出登记的尺寸参数，默认值从平台结构登记原样搬过来，所以行为不变；只有翻译的默认值是从配置搬过来的：

| 登记（`type/name`） | 参数（默认值 / 平台上限） |
| --- | --- |
| `audio_out/audio_asr_intent` | `transcribed_text_max_bytes`（511 / 16384）、`intent_slot_json_max_bytes`（1023 / 65536） |
| `audit_out/dialogue_audit` | `risk_level_max_bytes`（31 / 255）、`matched_policy_clause_max_bytes`（255 / 4096）、`audit_verdict_json_max_bytes`（1023 / 65536） |
| `doc_out/doc_qa` | `intent_name_max_bytes`（63 / 255）、`answer_text_max_bytes`（1023 / 65536） |
| `entity_out/entity_extract` | `entities_json_max_bytes`（2047 / 65536） |
| `entity_out/translate` | `entities_json_max_bytes`（**8191** / 65536） |
| `keyword_out/keyword_match` | `match_result_json_max_bytes`（2047 / 65536） |
| `od_out/ocr_invoice_qa` | `result_json_max_bytes`（2047 / 65536） |
| `rerank_out/cross_rerank` | 无（结构体没有字符串字段） |

**由 converter 固定、不进入配置的输出槽属性**：

| 属性 | 用途 | 现状 |
| --- | --- | --- |
| `allocator`、`allocator_params` | 嵌套 `void*` 的命名布局及其参数。要用另一种布局或另一组参数，就换一个登记 | 只有测试在用 |
| `metadata_count`、`metadata_type_id` | 结构体带 `CompanyAny*` metadata 成员时（目前只有 `CompanyOdOutput`）分配的元素数和类型 | 生产 converter 都写 0，与现在的默认值相同；仍属待内网核对的替身字段 |

这两类属性固定在槽声明中，带来三点变化：
- converter 与它写入的布局一定匹配，因此删除"converter 运行时核对 `spec.allocator`"的要求，以及输出分配指南中"接入第一个生产命名方案时补配对校验"的待办；
- `allocator_params` 在注册审计时就用布局自带的解析函数解析，写错会在 `Init` 报错，比现在的 Create 更早；
- 命名布局的登记接口（`RegisterOperatorOutputAllocator`、`normalize_parameters`）不变。

**可选输出槽**（`required = false`，只有测试在用）：总是分配输出池；宿主每次调用都可以省略这个 key。

**批次上限**：有效上限为 `min(max_frame_depth, kMaxProcessBatchSize)`。

**Create 时生成输出池规格**：

```text
对每个输出项（登记 def，参数 values）：
  slot    = def.slot
  binding = 平台结构登记.GetOutputBinding(slot.type_suffix, slot.allocator)
  spec.type = slot.type_suffix；spec.allocator = slot.allocator
  对 binding 的每个字符串字段 f：
    spec.capacities[f] = values.Integer(f + "_max_bytes")   // 必有，注册审计保证
  spec.meta_num = slot.metadata_count；spec.metadata_type_id = slot.metadata_type_id
  spec.params = 注册审计时由 slot.allocator_params 解析得到的结果（标准布局为空）
  ResolveOutputPoolSpec(binding, spec, &resolved)  // 超过平台上限：CONFIG_FIELD_RANGE
所有输出项合计做输出池总预算检查（规则不变）。
```

### 5.5 每侧多项

`io.input`、`io.output` 都可以有多项，每项对应一个宿主结构体。

**输入侧**：
- 每项各自按 `type` 从宿主 map 中取出结构体，核对 `service_type`，再交给各自的 converter 解析。
- 多项按批内序号配对：第 i 张图片和第 i 个问题属于同一个请求。各项的条数必须相同。
- 请求 ID 取自带 `request_id` 成员的结构体（由平台结构登记声明）。多项都带时，逐条核对是否一致；都不带时 Create 报 `INVALID_COMBINATION`。平台模拟中 `CompanyFrame` 带 `request_id`，`CompanyString` 不带。
- 各项发布的端口不能重名，否则报 `DUPLICATE_PORT_PRODUCER`。

**输出侧**：每项写自己的结构体；全部成功后一起发布，任何一项失败，所有租约自动归还（与现在相同）。

### 5.6 注册审计

`IoConverterRegistry` 新增 `Audit()`，取代 `IoBindingRegistry::Audit`。`GlobalInit` 中的错误来源前缀由 `IoBinding audit:` 改为 `Converter audit:`。检查项：

1. 登记冲突（沿用现有逻辑并扩展）：
   - 同一方向下（`type`, `name`）重复；
   - 同一方向、同一 `type` 下，`service_type` 取值重复；
   - `type` 或 `name` 为空；
   - 缺少回调。
2. 槽的 `type_suffix` 等于登记的 `type`。
3. 槽与平台结构的一致性：
   - `type_suffix` 已登记，且登记的结构名等于 `type_id`；
   - 输入槽要有 `validate_external`；
   - 输出槽的 `(type_suffix, allocator)` 能找到登记；
   - `allocator_params` 能被该布局解析；
   - `metadata_count` 不超过结构体登记的上限；结构体没有 metadata 成员时，必须为 0；
   - 结构体声明了 `service_type` 成员时，非 `common` 的登记必须填 `service_type`；结构体没有这个成员、或登记为 `common` 时，不能填。

   前两项从 IoBinding 审计移过来，范围从"被绑定的 converter"扩大到"所有登记"。
4. 参数声明合法：`ValidateConfigFieldDefinitions(params.Fields())`，包括默认值满足范围和枚举。
5. 尺寸参数完整：输出结构体的每个字符串字段都有对应的 `_max_bytes` 整数参数，默认值在 1 到平台上限之间。
6. 逻辑端口非空；同一登记内端口名唯一。

审计范围扩大到所有登记后，测试在全局注册表中登记的 converter 也要满足这些规则，否则同一进程中之后的 `Init` 都会失败（`Init` 每次都做审计）：
- 测试 converter 使用测试专用的 `name`，`service_type` 取不与生产登记重复的占位值；
- 故意违反审计的登记只用于验证审计本身，用例结束前用 `ClearForTesting()` 清理并恢复，与现在对 binding 审计的处理相同。

## 6. 删除"业务"这一层

删除的是 Core 中的业务概念和 binding 这层间接：`BizDefinition`、`IoBinding`、`biz_name`。`io` 各项的 `name` 是外部契约 `service_type` 在接入层的对应，只用于选择 converter 和核对请求，不进入 Core。

### 6.1 删除清单

| 位置 | 删除的内容 |
| --- | --- |
| `include/adapter/io_binding.h`、`include/adapter/io_binding_registry.h`、`src/adapter/io_binding_registry.cpp` | 整个文件 |
| `src/adapter/biz/` | 8 个 `*_bindings.cpp` 和 `README.md` |
| `include/adapter/converter_authoring.h` | `REGISTER_IO_BINDING` 宏及对 binding 头文件的包含 |
| `include/core/biz_definition.h` | 整个文件 |
| `include/core/port_definition.h` | `RequiredBizInput`、`OptionalBizInput`、`BizOutput` |
| `include/core/pipeline_catalog.h`、`src/core/pipeline_catalog.cpp` | 业务注册表、`FindBiz`、`RegisterBizDefinition(s)`、`Bizs()`、快照中的 `bizs`、Catalog JSON 的 `bizs` 与业务过滤参数 |
| `include/core/pipeline_config.h`、`src/core/pipeline_config.cpp`、`src/core/pipeline_config_structure.cpp` | `biz_name` 字段及其解析和必填要求 |
| `include/core/pipeline.h` | `GetBizName()` |
| `src/core/pipeline_validator.cpp` | 按业务进行的入口/出口检查、`UNKNOWN_BIZ`。边界伪节点的名字本设计不改，由节点设计统一为 `input`/`output`（其 4.5） |
| `src/adapter/pipeline_document.*`、`src/adapter/deployment_structure.*` | `deployment` 结构、`OutputAllocationStructure`、删字段再注入的逻辑 |
| `src/adapter/deployment_preparation.*` | 写回 `biz_name`、`ProjectDeploymentDiagnostics`、按 `out_mem` 处理输出槽 |
| `src/adapter/operator/operator_config_resolver.*` | `ResolveOutputAllocation` |
| `include/edgeflow/operator/interface.h` | `ResolveOperatorConfigBiz`（阶段 1） |
| `demo/biz/`、`demo/common/demo_registry.*`、`REGISTER_DEMO_BIZ`、`--example-control` | 阶段 1 |

### 6.2 原有职责的去向

| 原来由 binding / 业务承担 | 改由 |
| --- | --- |
| 选择 converter | `io.input` / `io.output` 各项的（`type`, `name`） |
| 批次上限 | 常量 `kMaxProcessBatchSize`，再按池深收紧 |
| 结构体与槽声明一致的审计（只覆盖被绑定的 converter） | `IoConverterRegistry::Audit`，覆盖所有 converter |
| "业务端口等于 converter 端口"的交叉审计 | 由两条保证代替：① CI 校验仓库中的全部方案（`NativeCli_*`、`PythonCli_*`、全业务集成测试），converter 端口一变，用到它的方案就会失败；② 新增 converter 契约测试，固定每个 converter 的槽和端口，改端口必须同步改测试 |
| Core 的业务边界 | `PipelineValidator::ValidateAndPlan(root, boundary)` 和 `Validate(root, boundary)` 的边界参数改为必传引用，不再有"无边界"模式 |
| `BizPortDefinition` | 改名 `IoPortDefinition` |
| Demo 按业务分派 | 按载体分派（第 9 节） |
| `ResolveOperatorConfigBiz` | `ResolveOperatorConfigIo`（8.1） |
| Core 测试往全局登记假业务 | 测试直接构造边界：`tests/support/pipeline_test_utils.h` 提供 `MakeTestBoundary(inputs, outputs)`；`BuildTestPipeline` 增加边界参数 |

### 6.3 与旧计划的关系

[FRAMEWORK_SIMPLIFICATION_PLAN.md](FRAMEWORK_SIMPLIFICATION_PLAN.md) 记录的三条决定被本设计取代。阶段 2 中要同步改写该计划：

1. **"沿用 IoBinding、`biz_name`"**：用户已改变方向。
2. **"BizDefinition 不改为由 converter 派生"**：当时有两条理由。
   - 派生必须等所有 converter 注册完，但 Catalog 会更早读取。本设计不是派生，而是直接删除；IO 边界在部署准备时（注册早已完成）由所选 converter 组成；Catalog 直接列出 converter。所以这个时序问题不存在。
   - 会失去"改 converter 端口时审计报错"这道检查。这由 6.2 的两条保证代替。
3. **"Demo 不按载体重组"**：用户已决定拆分。翻译现在就复用了实体抽取的 runner，说明 Demo 本来就是按载体工作的。

### 6.4 一并清理的遗留与冗余

以下各项不在 6.1 的删除清单中，但都只为旧形态服务，或者在新设计下重复。阶段 2 中一并删除：

| 位置 | 现状 | 处理 | 提交 |
| --- | --- | --- | --- |
| `tests/tooling/test_pipeline_studio.py` 的 `test_external_selector_is_required_and_root_biz_name_is_rejected`，以及 `root_biz_name` 用例 | 专门验证旧的根层 `biz_name` 会被拒绝 | 删除，不改写。`io` 的结构用例按第 11 节新写 | C6 |
| `tests/tooling/studio_browser_test.mjs` 中断言保存结果没有 `biz_name`、`output_allocations` | 检查旧字段名不存在 | 删除这两条断言，改为断言保存结果含 `io` | C6 |
| `tests/tooling/test_dev_recipe.py` 中 `.conf` 非法用例 `{"pipe_path": ..., "outputs": {}}` | 用旧 `.conf` 字段作为反例 | 删除该项；同一测试已有通用的未知字段用例 `unexpected` | C6 |
| `include/adapter/converter_authoring.h` 的 `ValidateDecodeRequest`，以及 `InputDecodeOptions::max_batch_size` | Operator 在解码前已检查批次上限，converter 辅助函数"防御性地再检查一次" | 删除 `max_batch_size` 和这次重复检查；批次上限只由 Operator 检查 | C2 |
| `include/adapter/io_converter.h` 的 `ExternalOutputBatchView::GetSlotCapacity(..., default_cap)` | 取不到容量时回退到调用方给的默认值，违背"容量只来自租用块的规格"；生产代码不使用 | 删除该方法；需要时用 `GetPoolSpec(slot)->GetCapacity(field)` | C2 |
| `src/adapter/deployment_preparation.h` 的 `DeploymentPathMode` | 枚举完全由"部署根是否为空"决定，另需两条一致性检查 | 删除枚举，`DeploymentPrepareOptions` 只保留 `model_root_dir`：为空时只做路径格式检查，非空时检查文件留在根内 | C3 |
| `IoBindingSelection::effective_max_batch_size` | 恒等于框架常量 | 删除；工具只报告 `effective_process_batch_limit` | C3 |
| `IoBindingSelection::output_parameter_texts` | 只用于报告的参数文本 | 删除；报告时直接取槽声明中的 `allocator_params` | C3 |

改造后接入层的准备结果只保留以下字段：

```cpp
struct SelectedInput {
  const InputConverterDefinition* converter = nullptr;
  std::shared_ptr<const ParameterValues> params;
};

struct SelectedOutput {
  const OutputConverterDefinition* converter = nullptr;
  std::shared_ptr<const ParameterValues> params;
  ResolvedOutputPoolSpec pool_spec;
};

struct IoSelection {
  std::vector<SelectedInput> inputs;    // 与 io.input 顺序相同
  std::vector<SelectedOutput> outputs;  // 与 io.output 顺序相同
};
```

## 7. 运行链路

```text
Create（每个句柄一次）
  1. 解析 .conf → pipe_path → Pipeline JSON（不变）
  2. 拆出 io，检查结构；其余部分作为 Core 文档                              【改】
  3. 按（type, name）查找每一项的登记；检查唯一性和请求 ID 来源              【改】
  4. 按登记的声明校验参数并补默认值 → 字段赋值 → Prepare → Validate          【新】
  5. 按每个输出项的布局与尺寸参数生成输出池规格，检查总预算                  【改】
  6. 解析模型路径（不变）
  7. Core 带 IO 边界校验并生成计划                                           【改：只按边界】
  8. 创建运行时；按输出池规格预分配 max_frame_depth 份（不变）
Process（每批一次）
  1. 按每个输入项的 type 从宿主 key 中取出结构体并校验结构（不变）
  2. 核对每个结构体的 service_type；多项输入按序号配对并核对 request_id       【新】
  3. 各输入项的 converter 解析请求；options.params 指向本句柄的参数           【新】
  4. 每个请求从输出池租用一块，在执行 DAG 之前（不变）
  5. 执行 DAG（不变）
  6. 各输出项的 converter 写入租用的块，并写入 service_type                   【新】
  7. 全部成功后一起发布；任何一步失败，所有租约自动归还（不变）
```

## 8. 对外接口与工具

### 8.1 SDK：`ResolveOperatorConfigIo`（阶段 1）

替代 `ResolveOperatorConfigBiz`。它是只读预检，做与 Create 相同的解析和校验，但不加载模型；返回值约定也与原函数相同（0 成功，-2 参数或配置错误，其余负值为校验失败或内部异常）。

```cpp
// include/edgeflow/operator/interface.h
struct OperatorIoEntry {
  std::string type;                     // 宿主 map key 的后缀，例如 "doc_in"
  std::string name;                     // 业务，例如 "doc_qa"
  std::string type_name;                // 宿主结构名，例如 "CompanyOperatorDocInput"
  std::optional<int32_t> service_type;  // 业务对应的取值；common 或结构体没有该成员时为空
  bool required = true;
};
struct OperatorIoContract {
  std::vector<OperatorIoEntry> inputs;   // 与 io.input 顺序相同
  std::vector<OperatorIoEntry> outputs;  // 与 io.output 顺序相同
};
COMPANY_ALG_API int ResolveOperatorConfigIo(
    const char* model_path, const char* cfg_file_name, OperatorIoContract* out,
    char* out_error_msg = nullptr, size_t error_buf_size = 0) noexcept;
```

- 阶段 1（格式切换之前）：旧 converter 的每个槽对应一项。`type` 为槽的 key 后缀，`name` 为原业务名，`service_type` 为空。
- 保持 `noexcept`，以及 `catch (const std::exception&)` 和 `catch (...)` 两层异常屏障。
- `cmake_ext/edgeflow_sdk.map.in` 和 `scripts/check_sdk_exports.sh` 中的导出符号换成新函数的修饰名（构建后用 `nm -D --defined-only` 取得），白名单仍为 6 个符号。
- `ResolveOperatorConfigBiz` 原有的路径越界测试，迁移到新函数。

### 8.2 Catalog JSON

| 键 | 变化 |
| --- | --- |
| `bizs`、`io_bindings` | 删除 |
| `input_converters[]` / `output_converters[]` | 每项是一个登记：`converter_id` 改为 `type`、`name`，新增 `service_type`；新增 `config_fields`，格式与节点相同。`config_fields` 的序列化函数从 `src/core/pipeline_catalog.cpp` 的 `FieldJson` 移到 `contracts/config_schema.h`（`ConfigFieldToJson`），以便接入层复用。槽信息新增 `allocator`；删除 `capacity_fields`（尺寸已体现在参数里） |
| `profiles[]` | 用 `io`（方案中各项的 `type`、`name`）代替 `biz_name`、`io_binding` |

### 8.3 `alg_pipeline_tool`

| 命令 | 变化 |
| --- | --- |
| `catalog` | 删除 `--io-binding` 过滤，输出按 8.2 |
| `init` | `--io-binding BIZ` 改为 `--input TYPE/NAME --output TYPE/NAME`（可重复，每次一项），生成只含 `io` 的空草稿，因此删除 `--empty`；`--profile NAME` 克隆该 Profile 方案（含 `io`），不再要求同时指定业务。登记不存在时报 `UNKNOWN_CONVERTER` |
| `validate` / `plan` | 接口不变；诊断路径按 4.3 |
| `validate-io` | 输出中的 `binding` 改为 `io`：`{"input": [{"type", "name", "external_type", "params"}], "output": [...]}`，其中 `params` 为生效值；`output_pools` 不变 |
| `resolve-conf` | `configuration` 删除 `biz_name` 和 `io_binding`，新增 `io`（同上）；`effective_pipeline` 中的 `io` 写入生效参数；`output_pools` 不变 |
| `export-schema` | `io.input` / `io.output` 为数组，至少一项；每项用 `oneOf` 按登记分支，分支中 `type`、`name` 为常量，`params` 的 schema 由该登记的 `config_fields` 生成 |
| `edit` | 边界端点的端口改为取自所有输入项、输出项（`src/cli/pipeline_authoring.cpp`）；端点名按节点设计 4.5 改为 `input`、`output` |
| `alg_show` | 显示 `io: doc_in/doc_qa -> doc_out/doc_qa`，多项用逗号分隔 |

`validate-io` 与 `resolve-conf` 功能重叠的问题，按用户之前的决定继续延后，不在本次处理。

### 8.4 Studio

- `tools/pipeline_studio/server.py`：
  - `catalog()` 不再按业务过滤；
  - `init()` 接收输入项、输出项列表或 `profile`；
  - 方案列表显示各项的 `type/name`；
  - "另存为"保留 `io`；
  - 原来往 `out_mem` 写容量的地方，改为写对应输出项的 `params`；
  - 不再读取 `configuration["biz_name"]`。
- `web/index.html`、`web/app.js`、`web/workbench.js`：
  - 原来的"I/O 契约"下拉框改为输入、输出两个列表，每项选择结构体和业务，可以增删项；
  - 各项参数复用节点参数编辑组件；
  - 画布的边界节点改用所有输入项、输出项的端口。
- 测试：`tests/tooling/test_pipeline_studio.py` 及 `studio_*.mjs`。

### 8.5 Python 工具与效果规格

- `tools/verify_selection.py`：
  - `pipeline_binding()` 改为 `pipeline_io()`，返回 `(输入项列表, 输出项列表)`；
  - `build_run_conf()` 写入输出项的 `params`，不再写 `out_mem`；
  - 读取解析结果时用 `io`，不再用 `biz_name`。
- `tools/dev_recipe.py`：
  - 克隆方案时用 `init --profile`；
  - 效果规格的匹配检查改为比较输出项；
  - 原来写 `out_mem` 的地方改为写输出项的 `params`。
- 效果规格 `tests/fixtures/effects/*.json`：`"io_binding": "keyword_match"` 改为 `"output": {"type": "keyword_out", "name": "keyword_match"}`，因为期望字段由输出格式决定。

## 9. Demo（阶段 1）

**按载体分派**：
- 请求构造：按输入项的结构体组合注册，键是各输入项的结构名按 `io.input` 顺序用逗号拼接，例如 `CompanyFrame,CompanyString`。
- 结果显示：按输出项的结构名注册。
- 构造请求时，`ResolveOperatorConfigIo` 返回了 `service_type` 的项，按它填写结构体成员（阶段 2 起平台模拟才有这个成员）。
- Demo 不理解载荷语义（AGENTS.md：Demo 只构造载体、显示结果），所以同一结构体上的多个业务共用 Demo 代码，例如 `entity_in` 上的 `entity_extract` 与 `translate`。在已有结构体上新增业务，Demo 不用改。

```cpp
// demo/common/demo_io_registry.h（新增）
struct DemoRequestBatch {
  std::vector<llm_edgeflow::operator_api::NamedIo> requests;  // key = "demo.<type>"
  std::vector<nlohmann::json> request_info;  // 每条请求供结果显示读取的信息（如 rerank 的候选文本）
  std::shared_ptr<void> storage;             // 持有载体内存，覆盖整个运行过程
};
using BuildRequestsFn = int (*)(const DemoOptions& options,
                                const std::vector<llm_edgeflow::operator_api::OperatorIoEntry>& inputs,
                                DemoRequestBatch* out);
using ShowResultFn = void (*)(const void* output, const nlohmann::json& request_info,
                              uint64_t* request_id, int32_t* status, nlohmann::json* sample_output);
#define REGISTER_DEMO_INPUT(external_type, build_fn)  // 例：("CompanyOperatorDocInput", BuildDocRequests)
#define REGISTER_DEMO_OUTPUT(type_name, show_fn)      // 例：("CompanyOperatorDocOutput", ShowDocResult)
```

**公共流程**（`demo/common/operator_runner.h`）只保留一份：
1. 用 `ResolveOperatorConfigIo` 取得 I/O 契约，找到对应的请求构造和结果显示；找不到时报错并列出已支持的载体。
2. 创建句柄，执行 Control。
3. 按 `batch_size` 分批调用 Process；输入、输出 key 为各项的 `demo.<type>`。
4. 对每条结果、每个输出项调用结果显示，合并写入 `results.jsonl` / `summary.json`。多个输出项的字段同名时，以 `type` 作前缀（`<type>.<字段>`）。

**`results.jsonl` 的 `output` 字段保持现状**，`tests/integration/demo`、效果规格和 `demo/json_prompt_demo.py` 都依赖这些字段：

| 输入载体组合 | 数据集格式 | 来自现有 runner |
| --- | --- | --- |
| `CompanyOperatorKeywordInput` | 每行一条文本 | `keyword_match_demo.cpp` |
| `CompanyOperatorEntityInput` | 每行一条文本（纯文本或 JSON 请求） | `entity_extract_demo.cpp`（实体抽取、翻译） |
| `CompanyOperatorDocInput` | `[DOC]` / `[QUERY]` 标签段 | `doc_qa_demo.cpp` |
| `CompanyOperatorAuditInput` | `[CHANNEL]` / `[DIALOGUE]` 标签段 | `dialogue_audit_demo.cpp` |
| `CompanyOperatorRerankInput` | `[QUERY]` / `[PASSAGE]` 标签段 | `cross_rerank_demo.cpp` |
| `CompanyOperatorAudioInput` | `.jsonl`；测试可用内置回退样本 | `audio_asr_intent_demo.cpp` |
| `CompanyFrame,CompanyString` | `[IMAGE]` / `[PROMPT]` 标签段 | `ocr_invoice_qa_demo.cpp` |

| 输出结构 | `output` 字段 |
| --- | --- |
| `CompanyOperatorKeywordOutput` | `is_hit`、`match_result`（或 `match_result_raw`） |
| `CompanyOperatorEntityOutput` | `entities`（或 `entities_raw`） |
| `CompanyOperatorDocOutput` | `chunk_count`、`intent_name`、`confidence`、`answer_text` |
| `CompanyOperatorAuditOutput` | `risk_level`、`risk_score`、`matched_policy`、`audit_verdict`（或 `_raw`）；`channel` 取自请求信息 |
| `CompanyOperatorRerankOutput` | `ranked_results`，其中 `passage_text` 取自请求信息；`query` 取自请求信息 |
| `CompanyOperatorAudioOutput` | `transcribed_text`、`intent_slot`（或 `intent_slot_raw`） |
| `CompanyOdOutput` | `detected_box_count`、`extracted_invoice`（或 `_raw`） |

结果显示读取的请求信息都是可选的。缺失时降级显示，例如 rerank 只显示候选序号。

**Control 一律显式**：
- 删除 runner 内置的默认命令，以及 `--example-control` 选项。
- 给了 `control_file` 就必须同时给 `control_cmd`，否则返回 3。
- 关键词的示例 Control 改为数据文件 `data/keyword_match_control.json`，并新增 Profile `keyword_match_control`（`control_cmd: 1`，即 `kUpdateRules`）。

**其他**：
- 音频的"是否真实模型"只看 `suite == "real"`，删除按名字包含 `whisper` 猜测的逻辑。
- 源码：`demo/input/*.cpp`（7 个请求构造）、`demo/output/*.cpp`（7 个结果显示）；`demo/CMakeLists.txt` 的自动收录目录改为这两个；删除 `demo/biz/`。
- `alg_demo --list` 列出 Profile 和已支持的载体。

## 10. 实施阶段

| 阶段 | 内容 | 依赖 |
| --- | --- | --- |
| 0 | 参数声明下沉到 `contracts/` | 无 |
| 1 | Demo 按载体运行；`ResolveOperatorConfigIo` | 无（可与阶段 0 并行） |
| 2 | Pipeline 直接选择 converter（格式切换） | 阶段 0、1 |

每个阶段一个 PR，各自通过 `./scripts/run_all_tests.sh`；配置格式只在阶段 2 切换一次。

### 阶段 0：参数声明下沉到 `contracts/`

只移动代码、改类名，不改变行为。

| 文件 | 改动 |
| --- | --- |
| `include/contracts/parameters.h`（新增） | 移入 `FieldTypeTraits`、`ParameterFieldBinding`、`ConcreteFieldBinding`、`FieldBuilder`、`Field`、`ParameterFieldBindingHolder`、`NoParameters`、`Parameters<T>`、`BindingFacts`（只是普通结构体）；`NodeConfigParser<T>` 改名 `ConfigParser<T>` 并移入 |
| `include/nodes/parameter_binding.h`、`include/nodes/node_config_parser.h` | 删除 |
| `include/nodes/configuration_snapshot.h` | 删除 `BindingFacts` 的定义，改为包含 `contracts/parameters.h`；`MakeBindingFacts` 留在这里 |
| `include/nodes/authoring.h`、`control_authoring.h`、`function_node.h`、`generate_options_config.h` | 包含路径改为 `contracts/parameters.h` |
| `src/common_nodes/structured_json_parse_node.cpp`、`text_corpus_source_node.cpp`、`text_rule_match_node.cpp`、`text_template_node.cpp` | `NodeConfigParser` 改为 `ConfigParser` |
| `include/contracts/config_schema.h`、`src/core/pipeline_catalog.cpp` | `FieldJson` 移为 `ConfigFieldToJson` |
| 测试：`tests/unit/nodes/test_parameter_binding.cpp`、`test_function_node.cpp`、`tests/unit/core/test_node_base_contracts.cpp`、`tests/contract/catalog/test_registry_conflict.cpp` | 包含路径与类名 |
| `tests/contract/architecture/test_layer_header_views.cmake` | 增加 `check_header(capability_nodes contracts/parameters.h TRUE)` 与 `check_header(integration contracts/parameters.h TRUE)` |
| `dev_support/node_authoring/benchmark/run.py` | 头文件列表 |
| `doc/dev_guide/custom_node_concepts.md`、`doc/dev_guide/source_layout.md`、`src/custom_nodes/README.md`、`.agents/skills/edgeflow-node-developer/SKILL.md`、`.agents/skills/llm-edgeflow-developer-guide/references/capability-nodes.md` | `NodeConfigParser` 改为 `ConfigParser`，链接和文件列表指向新头文件 |

**验收**：
- 门禁通过；
- `src/common_nodes`、`src/custom_nodes` 除类名外没有改动；
- 节点作者包含 `nodes/authoring.h` 的写法不变。

### 阶段 1：Demo 按载体运行，SDK 预检返回 I/O 契约

**目标**：Demo 不再依赖业务名。阶段 2 删除业务层时，Demo 不用再动。

| 文件 | 改动 |
| --- | --- |
| `include/edgeflow/operator/interface.h`、`src/adapter/operator/operator_adapter.cpp` | 新增 `OperatorIoEntry`、`OperatorIoContract`、`ResolveOperatorConfigIo`，删除 `ResolveOperatorConfigBiz`。本阶段的实现从现有 `ValidatedIoPlan` 的 `input_converter` / `output_converter` 读取槽信息（`KeySuffix()`、`type_id`、`required`），按 8.1 每个槽对应一项 |
| `cmake_ext/edgeflow_sdk.map.in`、`scripts/check_sdk_exports.sh` | 换成新符号 |
| `demo/common/demo_io_registry.{h,cpp}`（新增） | 第 9 节的两个注册表和宏 |
| `demo/common/operator_runner.h` | 公共流程：多槽 key、分批、显示、写结果；删除 `ResolveConfigBiz` 和单槽专用的 `RunOperatorWithExtractor` |
| `demo/input/*.cpp`、`demo/output/*.cpp`（新增） | 从 7 个 runner 拆出请求构造和结果显示，`output` 字段保持不变 |
| `demo/biz/`、`demo/common/demo_registry.*` | 删除 |
| `demo/main.cpp`、`demo/common/demo_options.*` | 按载体分派；删除 `example_control`；`control_file` 必须配 `control_cmd` |
| `demo/CMakeLists.txt` | 自动收录 `demo/input/`、`demo/output/` |
| `demo/profiles.json`、`data/keyword_match_control.json`（新增） | 新增 Profile `keyword_match_control` |
| 测试 | `tests/integration/demo/test_demo_runner.cpp`（注册表、分派、Control 规则）；`tests/contract/abi/test_cpp_operator_sdk.cpp`；`tests/integration/operator/test_operator_api.cpp` 与 `tests/unit/adapter/test_io_binding_registry.cpp` 中原 `ResolveOperatorConfigBiz` 的用例 |
| 文档 | `doc/dev_guide/business_onboarding.md` 的 Demo 部分、`doc/dev_guide/operator_output_allocation.md`（链接了 `demo/biz/`）、`doc/dev_guide/first_control.md`、`doc/developer_guide.md`、`doc/architecture.md`（导出函数列表）、`.agents/skills/pipeline-composer/references/workflow.md`（`--example-control`）、`doc/architecture_classes.puml`（运行 `./scripts/render_architecture_diagrams.sh --generate` 重新生成 SVG）、`doc/CHANGELOG.md` |

**验收**：
- 改造前后分别运行 `alg_demo --suite smoke`，以及本地有权重时的 real 套件，`results.jsonl` 中的 `output` 字段逐条一致；
- `demo/json_prompt_demo.py` 的测试通过；
- SDK 导出白名单为 6 个符号；
- 门禁通过。

### 阶段 2：Pipeline 直接选择 converter

建议按下列顺序提交。中间提交尽量保持可编译；PR 最后必须通过门禁。

| 提交 | 内容 | 主要文件 |
| --- | --- | --- |
| C1 Core 去掉业务 | 第 6.1 节中 Core 的删除项；`ValidateAndPlan` / `Validate` 必须传入边界；`BizPortDefinition` 改名 `IoPortDefinition`；诊断码改名；Core 测试改为构造边界 | `include/core/*`、`src/core/*`、`tests/support/pipeline_test_utils.h`、`tests/support/node_plan_fixture.h`、`tests/unit/core/*`、`tests/unit/nodes/*`、`tests/unit/engine/*`、`tests/integration/pipeline/*` |
| C2 converter 登记、参数与输出内存 | `ParameterFieldBinding::Read`；`contracts/parameter_set.h`（5.2）；登记结构改为 `type`、`name`、`service_type` 和单个 `slot`（5.2），新增 `params`，槽新增 `allocator`、`allocator_params`、`metadata_count`、`metadata_type_id`；`MaxBytes`；平台结构登记只保留上限，并声明 `request_id`、`service_type` 成员；`ResolveOutputPoolSpec` 不再填默认值；`IoConverterRegistry::Audit` | `include/contracts/parameters.h`、`include/contracts/parameter_set.h`、`include/adapter/*`、`src/adapter/io_converter_registry.cpp`、`src/adapter/operator/operator_value_type_registry.cpp`、`src/adapter/operator/operator_builtin_value_types.cpp` |
| C3 `io` 文档与部署准备 | `deployment_structure.*` 改为 `io_structure.*`（`IoStructure()`、`PipelineDocumentStructure()`）；`SplitPipelineDocument` 输出 `io` 选择与 Core 文档；`PrepareDeploymentDocument` 按第 7 节执行；`IoBindingSelection` 改为 `IoSelection`；`io_binding_resolver.*` 改为 `io_plan_resolver.*`（`IoPlanResolver`）；`ValidatedIoPlan` 携带参数、在 `resolved_pipeline_json` 中写入生效的 `io`；`OperatorConfigResolver` 删除 `ResolveOutputAllocation`，批次上限用常量；Process 核对 `service_type`、按 5.5 配对多项输入、设置 `options.params`，输出时写入 `service_type`；`GlobalInit` 改用 converter 审计 | `src/adapter/*`、`src/adapter/operator/*`、`src/adapter/shared_algorithm_runtime.cpp` |
| C4 删除 binding、converter 改为按（结构体，业务）登记 | 删除第 6.1 节中接入层的文件；按 5.1 改为登记，图片问答输入拆成 `frame`、`string` 两个登记；平台模拟加 `service_type` 成员和占位取值；8 个输出登记按 5.4 声明尺寸参数；测试用 converter 和嵌套布局夹具改用槽声明中的 `allocator`、`allocator_params`；`scripts/check_layer_isolation.sh` 的规则 2 原来检查 `src/adapter/biz/`，随目录删除改为检查 `src/adapter/input/`、`src/adapter/output/`，自检用例中的 `src/adapter/biz/bad_adapter.cpp` 同步改路径 | `scripts/check_layer_isolation.sh`、`include/adapter/io_binding*.h`、`src/adapter/biz/`、`src/adapter/input/*`、`src/adapter/output/*`、`include/platform_mock/*`、`tests/support/operator_nested_output_fixture.h`、`tests/support/adapter_examples/nested_pointer_tree_adapter.h` |
| C5 命令行工具 | 8.2、8.3 | `src/adapter/io_catalog.*`、`src/cli/alg_pipeline_tool.cpp`、`src/cli/pipeline_json_schema.cpp`、`src/cli/pipeline_authoring.cpp`、`src/cli/pipeline_remediation.cpp`、`src/cli/pipeline_document_validation.cpp`、`src/cli/alg_show.cpp`、`tests/RuntimeTests.cmake`（`PipelineToolCatalogTest` 改为不带参数的 `catalog`） |
| C6 Studio 与 Python 工具 | 8.4、8.5 | `tools/pipeline_studio/*`、`tools/verify_selection.py`、`tools/dev_recipe.py`、`tests/tooling/*` |
| C7 配置与测试迁移 | 按 4.5 迁移全部方案和内嵌配置；更新接入层与 ABI 测试 | 第 4.5 节列出的文件；`tests/unit/adapter/*`（`test_io_binding_registry.cpp` 改名为 `test_io_converter_registry.cpp`）、`tests/contract/abi/*`、`tests/contract/catalog/*`、`tests/integration/operator/*`、`tests/integration/runtime/*` |
| C8 文档与规则 | 第 12 节；删除本文件 | 见第 12 节 |

**验收**：
- 8 个业务的 Operator 黄金测试（`tests/integration/operator/test_operator_golden.cpp`）只改配置路径，期望值一律不改，并全部通过；
- 全部方案改为新格式并通过 `validate` 和 `plan`；kite 方案在 kite 构建中验证，可用 CI 的 kite 任务，或本地 `build/variants/kite-cpu`；
- 第 11 节列出的新增测试全部通过；
- 除 `doc/CHANGELOG.md` 的历史条目外，仓库中不再出现 `biz_name`、`io_binding`、`IoBinding`、`BizDefinition`、`out_mem`、`REGISTER_DEMO_BIZ`、`ResolveOperatorConfigBiz`、`converter_id`，`deployment` 也不再作为配置键出现（本文件随阶段 2 删除）；可以用 `git grep` 检查。不为这些旧名字编写"会被拒绝"的专门测试，它们按通用的未知字段规则处理；
- 输入结构体的 `service_type` 与方案不符时，Process 返回输入非法；图片问答的两项输入按序号配对；
- 6.4 所列遗留与冗余全部清理，没有新增任何兼容分支、别名或迁移代码；
- 门禁通过；本地有真实权重时运行 `scripts/run_real_model_e2e.sh`。

## 11. 测试计划

| 范围 | 用例 | 阶段 |
| --- | --- | --- |
| 参数声明 | 现有 `test_parameter_binding.cpp` 全部通过；新增 `Read()` 读出当前成员值，包括 `Prepare` 修改后的值 | 0 / 2 |
| converter 参数 | 不写 `params` 时使用默认值；覆盖生效；未知参数报 `UNKNOWN_CONFIG_FIELD`，路径 `/io/output/<i>/params/<参数>`；超出范围报 `CONFIG_FIELD_RANGE`；`Prepare` 失败报 `INVALID_COMBINATION`，路径 `/io/output/<i>/params`；测试用的输入、输出 converter 在逐行回调中读到参数；同一句柄并发 Process 读取同一份参数 | 2 |
| 输出内存 | 尺寸来自 converter 默认值；覆盖生效；超过平台上限报 `CONFIG_FIELD_RANGE`；缺少某个字符串字段的 `MaxBytes` 时注册审计报错；默认值超过上限时注册审计报错；翻译输出长度在 2048 到 8191 字节之间且不写配置时成功；`Prepare` 推导出的尺寸生效 | 2 |
| `io` 结构 | 缺少 `io`；`io.input` 不是数组或为空；某项不是对象、有其他键、缺少 `type` 或 `name`；未登记的（`type`, `name`）（含方向用错）报 `UNKNOWN_CONVERTER`，`type` 已登记时列出它的业务；写 `common` 而该结构体没有 `common` 登记时报 `UNKNOWN_CONVERTER`；同一侧重复报 `DUPLICATE_IO_ENTRY` | 2 |
| `service_type` 与多项输入 | 结构体的 `service_type` 与方案不符时 Process 返回输入非法，错误信息含结构体与序号；输出结构体写入登记的 `service_type`；两项输入按序号配对，请求 ID 取自 `frame`；两项都带 `request_id` 而不一致时报错；两项都不带时 Create 报 `INVALID_COMBINATION`；两项发布同名端口时报 `DUPLICATE_PORT_PRODUCER` | 2 |
| 注册审计 | 一个没有任何方案使用的登记的槽结构不一致时，`Init` 返回 -6 并在 `GetOperatorLastError()` 中写明 `Converter audit` 和 `type/name`（新覆盖）；同一 `type` 下 `service_type` 重复、结构体有 `service_type` 成员而非 `common` 登记没填、结构体没有该成员却填了时报错；`allocator_params` 无法被布局解析时 `Init` 报错；结构体没有 metadata 成员却声明了 `metadata_count` 时报错 | 2 |
| 命名布局 | 由原 A3 用例改写：converter 在槽声明中固定布局和参数；同一宿主结构体在不同句柄中选用不同登记，得到不同布局；未声明布局的登记使用结构体的标准布局 | 2 |
| Core | 不再向全局注册假业务；`MISSING_OUTPUT_PRODUCER`；边界为必传参数 | 2 |
| converter 契约 | 新增 `tests/contract/catalog/test_converter_contracts.cpp`：固定 17 个生产登记的 `type`、`name`、`service_type`、槽的结构名、端口（名字、类型）和参数名，取代原 `AllBizDefinitionsAreRegistered` | 2 |
| Demo | 按载体分派；找不到载体时报错并列出已支持的载体；`results.jsonl` 字段不变；`control_file` 必须配 `control_cmd` | 1 |
| SDK | `ResolveOperatorConfigIo` 返回值、槽信息，以及路径越界、空参数等用例 | 1 |
| 工具 | catalog、init、validate-io、resolve-conf、export-schema、edit 的输出；Studio 的 Python 与浏览器测试；dev_recipe、verify_selection | 2 |

## 12. 文档与规则更新（阶段 2，C8）

| 文件 | 改动 |
| --- | --- |
| `AGENTS.md` | 接入层职责一句删除 `IoBinding`，改为 "Registered `InputConverter` and `OutputConverter` own the conversion, not central dispatch or lower layers"；任务路由表的 "Converter, IoBinding" 改为 "Converter" |
| `CONTRIBUTING.md` | 逐条改写涉及业务的句子：§3 "Each biz has exactly one IoBinding……" 改为"一个（结构体，业务）登记标识一种外部载荷格式；发布后的不兼容变更原地修改并随新 SDK 发布；只有新旧格式必须并存时才新增登记"；§3 "the biz that names each external contract" 改为 "the (struct, business) registration that names each external format"；任务表中的 "biz contracts" 改为 "registered converters"；§4 "Adapter/biz/port names" 改为 "Adapter/converter/port names"；删除 "A `biz_name` identifies an I/O contract"；重新判断 "Use `biz` for new internal business identifiers" 是否保留 |
| `doc/architecture.md`、`doc/developer_guide.md`、`doc/architecture_classes.puml`、`doc/architecture_flow.puml` | 接入层流程、类图与流程图；重新生成 SVG。`scripts/check_architecture_docs.sh` 依赖的 `PrepareDeploymentDocument`、`ValidatedIoPlan` 保持原名 |
| `doc/dev_guide/business_onboarding.md` | 接入步骤改为"选已有的（结构体，业务）登记，或新增一个登记 → 写 Pipeline"；删除 binding 与业务注册的步骤；写明 `type`、`name`、`service_type`、`common` 的含义 |
| `doc/dev_guide/operator_output_allocation.md` | `out_mem` 改为 converter 尺寸参数；命名布局与 metadata 改为在槽声明中固定；删除配对校验待办；扩展场景表更新 |
| `doc/dev_guide/source_layout.md`、`doc/dev_guide/adapter_templates/README.md` | 命名表：converter 登记、源码目录；删除 `src/adapter/biz/`、`demo/biz/` 及指向它们的链接 |
| `configs/README.md`、`doc/solutions/translate.md`、`doc/VERIFIABLE_SELECTION.md`、`doc/dev_guide/recipe_*.md`、`doc/dev_guide/first_control.md` | 配置示例与说明 |
| `include/platform_mock/README.md` | metadata 的位置改为 converter 的槽声明；`service_type` 成员与占位取值是外网替身，进内网核对 |
| `src/adapter/input/README.md`、`src/adapter/output/README.md` | 按（结构体，业务）登记、参数、尺寸、`common` |
| `tools/pipeline_studio/README.md` | 输入项、输出项的选择与参数编辑 |
| `.agents/skills/edgeflow-adapter-developer`、`edgeflow-solution-planner`、`json-prompt-solution`、`llm-edgeflow-developer-guide/references/integration.md`、`pipeline-composer/references/workflow.md` | 去掉 binding、`io_binding`、`out_mem`，改为 `io` 与 converter 参数 |
| `plans/FRAMEWORK_SIMPLIFICATION_PLAN.md` | 按 6.3 改写被取代的决定；阶段 4 示例中的配置写法 |
| `doc/CHANGELOG.md` | 各阶段的用户可见变化 |

## 13. 风险与不做的事

| 项目 | 处理 |
| --- | --- |
| 改动面大：阶段 2 涉及 100 多个文件 | 按 C1–C8 分层提交；黄金测试期望值不改；配置只切换一次 |
| 载体名是外网替身 | `type` 跟随载体名，进内网核对后一并调整 |
| `service_type` 的成员与取值是外网替身 | 平台模拟用占位枚举；进内网按真实头文件核对成员位置和取值 |
| `common` 只预留 | 框架只约定选中方式与不核对 `service_type`；各结构体的 `common` 登记由用户后续补充 |
| 嵌套布局参数和 metadata 不能按部署调整 | 换一个登记即可。触发条件：出现第一个"同一格式需要按部署调整布局参数"的生产需求，再把这些属性变成 converter 参数 |
| 可选输出槽总是分配输出池 | 只有测试使用；出现生产需求时再评估内存占用 |
| 不做：converter 在 Create 阶段读文件或加载资源 | 触发条件：出现第一个真实需求 |
| 模型、后端参数改用相同的声明方式 | 见[模型配置设计](PIPELINE_MODEL_DESIGN.md) |
| 端口名与 `biz_*` 常量头去业务化 | 由节点设计阶段 2 随"节点名.端口名"连线一并处理（4.6）；涉及 `structured_verdicts` 等端口名，以及 `adapter/biz_blackboard_keys.h`、`adapter/biz_input_constraints.h` |

## 14. 已确认的决定

| 决定 | 内容 |
| --- | --- |
| 外部契约 | `.conf` 与 `pipe_path` 不改 |
| `io` 位置 | 删除 `deployment` 层，`io` 放在根层 |
| `io` 写法 | `"io": {"input": [{"type", "name", "params"}], "output": [...]}`；两侧都是数组，可以有多项；`params` 可省略 |
| `type` 与 `name` | `type` 为宿主结构体（map key 后缀 `yyy`）；`name` 为业务，与结构体成员 `service_type` 的取值一一对应（外部契约）；同一业务中不会出现两个相同的（结构体，`service_type`） |
| converter 登记 | 按（`type`, `name`）登记；每个请求核对 `service_type`；新增业务只新增登记。翻译登记不加通用化参数 |
| `common` | 保留的业务名，表示结构体的默认处理（switch 的 default）；选中时不核对 `service_type`；处理内容由用户后续补充；未登记的业务名报错，不退回 `common` |
| 多项输入 | 按批内序号配对；请求 ID 取自带 `request_id` 的结构体，多项都带时核对一致。图片和问题无论在一个结构体还是两个结构体中，都不需要改框架 |
| 去掉业务层 | 删除 `io_binding`、`IoBinding`、`BizDefinition`、`biz_name`；业务名只在接入层使用 |
| 输出内存 | 删除 `out_mem`；尺寸是输出 converter 的参数；嵌套布局与 metadata 由 converter 在槽声明中固定 |
| 参数机制 | 与节点同一种写法：`Params` 加 `Field`，默认值写在代码里，配置覆盖，`Prepare` 在 Create 阶段执行；只能读本项的参数；声明代码与类型擦除容器 `ParameterSet` 下沉到 `contracts/`，与模型、后端共用 |
| Demo | 按载体拆成请求构造与结果显示；`ResolveOperatorConfigBiz` 改为 `ResolveOperatorConfigIo` |
| 不做兼容 | 不保留旧字段别名、不识别旧格式、不提交迁移代码；不为旧格式、旧名字编写"会被拒绝"的专门测试，已有的此类测试删除（6.4） |
