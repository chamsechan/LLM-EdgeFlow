# LLM-EdgeFlow Pipeline Studio

Pipeline Studio 的 Python 服务端、Web 资源和使用文档位于本目录。工作台与自动化接口复用
C++ Catalog、参数声明与 Validator；Core 负责端口、生命周期和 DAG 校验。

## 前置条件

先按根目录 [README 的最小构建步骤](../../README.md#1-获取并构建)准备工具与 Demo。
关键词编排无需模型权重；真实模型使用[匹配的构建变体](../../doc/VERIFIABLE_SELECTION.md#构建变体)。
以下命令从仓库根执行。

## 终端查看

```bash
./tools/pipeline_studio/server.py configs/pipeline_doc_qa_default.json
./build/alg_show configs/pipeline_doc_qa_default.json
```

查看器展示声明的 I/O 项、节点 `type` / `name`、`inputs` 引用、`depends_on`，以及模型类别、
Backend、文件和显式参数。原生查看器只显示已配置的 `backend.params.max_batch_size` 与
`decode_batch_size`，不推测默认值。查看不会加载模型或派生执行计划；拓扑顺序和波前层用 `plan` 查询。

## Web 工作台

```bash
./tools/pipeline_studio/server.py --web
./tools/pipeline_studio/server.py configs/pipeline_doc_qa_kite.json --web
./tools/pipeline_studio/server.py --web --port 8081
```

服务只绑定 `127.0.0.1`，按 `Ctrl+C` 停止；启动后访问终端显示的地址。只有 `--web` 启动服务，
不提供文件时可在页面用“浏览文件…”选择单个 JSON。文件上限为 4 MiB。

受控方案为 `configs/pipeline_[a-z0-9_]+.json`。其他位置的文件作为导入文档浏览、编辑和校验，
保存时另存到受控目录。打开只要求可解析；缺失模型、Backend 或工具时仍可浏览。
保存前执行原生校验，并用 SHA-256 revision 检测外部修改。

### 查看与编辑

- 默认进入浏览模式；拖动空白处平移、滚轮缩放，双击节点聚焦。“适应画布”显示完整流程，
  “自动布局”重新排列；实线表示数据流，虚线表示额外执行顺序。节点位置只存浏览器 `localStorage`。
- “新建”可直接克隆 Profile，也可分别添加多个输入、输出 Converter，生成空草稿。
  I/O 页按 `(type, name)` 选择登记，参数使用同一属性表单；更换登记时清除原登记的覆盖和输出连接。
- “编辑方案”启用新增、删除、重命名与端口连接。节点属性使用 `params`，连接直接写
  `inputs: {逻辑端口: "节点名.端口名"}`；外部输入引用 `input.端口名`，输出连接写入对应
  `io.output[]` 的 `inputs`。重命名或删除同步所有引用和 `depends_on`。
- 连线单击选中后用“删除连线”或 `Delete` 删除。断开数据连接保留显式 `depends_on`；
  属性页可单独编辑顺序约束。数据依赖由 Core 推导。
- 撤销/重做支持 `Ctrl` / `⌘` + `Z`、`Shift` + `Z` 或 `Y`；文本框保留自身快捷键。
  历史只属于当前文档，不跨刷新和文档切换。
- 节点、I/O、模型或 JSON 表单有未应用修改时可应用或放弃。校验、运行、保存先应用合法表单；
  非法字段保留输入并定位错误。切换对象可选择应用、放弃或留在当前；应用后还需保存文件。
- 数值、布尔、枚举、数组、对象、映射和 JSON 字段的默认值只作提示，不自动写入配置。
  已有显式值原样保留；清空相应覆盖可恢复缺省。嵌套字段使用 JSON 文本编辑。
  自由字符串支持多行，保留空串、空白和换行；精确删除字段可使用 JSON 页。
- 模型按类别与兼容 Backend 选择，界面显示当前构建对应的实现；Model 与 Backend 各自有 `params`。
  节点的模型候选按其声明的类别筛选。无法执行的已有模型仍可浏览，兼容提示不代替资源与效果验收。
- 修复候选先展示改动供审阅，应用一次产生一个撤销步骤；文档或工具变化会使旧候选失效。

### 第一次编排

```bash
./build/alg_pipeline_tool catalog
./build/alg_pipeline_tool describe-node text_rule_match
./tools/pipeline_studio/server.py --web
```

1. 点击“新建”，从 `keyword_match_rules` Profile 克隆。
2. 检查 `input.sentence_text` → 规则节点 `text`，以及规则节点 `matches` → 输出 Converter `matches`。
   可删除后重新拖动连接，或撤销恢复。
3. 将规则 `categories` 改为 `{"FIRST_RUN":["VIP"]}` 并应用，在 JSON 页核对配置。
4. 校验后，在运行页选择兼容的 `keyword_match_rules`，另存为 `pipeline_first_solution.json`，生成配套 `.conf`。
5. 执行当前草稿并核对业务结果：第一条命中 `FIRST_RUN`，第二条未命中。

运行页“接入契约”展示有序输入、输出 `(type, name)`、Converter 和宿主类型。
Catalog 中的 `slot` 给出类型、必需性、分配器及固定布局元数据，`logical_ports` 是内部端口，
`config_fields` 是参数声明。完整外部请求/响应仍以 Converter 契约为准，内部端口不定义 JSON 载荷。

缺失算法时使用[第一个自定义 Node](../../doc/dev_guide/first_custom_node.md)，
新增平台载体使用[业务接入指南](../../doc/dev_guide/business_onboarding.md)。

## 自动化 CLI

```bash
./build/alg_pipeline_tool catalog
./build/alg_pipeline_tool describe-node text_embedding
./build/alg_pipeline_tool describe-model embedding onnxruntime
./build/alg_pipeline_tool describe-backend onnxruntime
./build/alg_pipeline_tool export-schema
./build/alg_pipeline_tool validate configs/pipeline_doc_qa_default.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa_default.json --explain
./build/alg_pipeline_tool validate-io configs/pipeline_doc_qa_default.conf
./build/alg_pipeline_tool resolve-conf configs/pipeline_doc_qa_default.conf --root . --depth 2
```

Catalog 模型条目用 `model_type` 表示类别，`impl_name` 表示实现，`backends` 列出当前可选后端；
节点依赖用 `model_type` 和 `config_field` 表示绑定要求。Schema 从当前构建的递归声明导出，
覆盖数组、映射、嵌套结构和 JSON 值；端口语义与执行合法性仍由原生 Validator 判断。

克隆使用 `init --profile NAME`，直接保存 JSON 时加 `--raw`。新建空草稿使用可重复的
`--input TYPE/NAME --output TYPE/NAME`；只填 I/O 选择，`models`、`pipeline` 是空数组，待补齐后校验。
Profile 的 `io` 摘要只包含有序 `(type, name)`；容量覆盖不改变外部契约匹配。

`validate` / `plan` 接受 FILE 或 `--stdin`。FILE 按自身目录解析资源；stdin 没有文件位置，
仅校验路径写法。Model 的 `file` 与声明的文件参数相对 Pipeline JSON 目录，拒绝绝对路径、
父目录分量和符号链接越界。预检不加载权重。

### 原生编辑请求

`alg_pipeline_tool edit --stdin` 接受完整文档 `pipeline` 和互斥的 `operation` 或非空 `operations`。
`require_valid` 默认为 `false`；为 `true` 时最终候选必须通过校验。最多 128 个动作、4 MiB。

| 动作 `kind` | 字段 |
| --- | --- |
| `add_node` | `type`；可选 `name`、对象 `params`，省略名称时按类型分配唯一名称 |
| `remove_node` | `node` |
| `rename_node` | `node`、`new_name` |
| `connect` / `disconnect` | `source`、`target`，各含 `node`、`port` |
| `add_dependency` / `remove_dependency` | `node`、`depends_on` |

伪节点 `input` / `output` 代表 I/O 边界；节点名非空、不得含点、不得占用这两个名称。
目标 `output` 的逻辑端口若匹配多个选中 Converter，返回 `AMBIGUOUS_OUTPUT_PORT` 并列出候选，
不隐式选择或广播。新增连接和依赖由 Core 检查环，产生 `DAG_CYCLE` 时始终拒绝该动作。

批量动作作用于文档副本。任一动作或严格校验失败不返回候选，动作错误给出
`failed_operation_index`；成功的 `ok` 表示编辑成功，不完整草稿仍须检查 `validation.ok`。
命令只输出 JSON。接口见 [PipelineAuthoring](../../src/cli/pipeline_authoring.h)。

### 工具选择

生产方案使用目标构建的 `alg_pipeline_tool`；测试 Model/Backend 使用 `alg_pipeline_tool_test`，
Catalog、克隆、编辑与校验保持同一工具。新增登记后需重新构建；测试工具通过不能代替生产验证。

```bash
LLM_EDGEFLOW_PIPELINE_TOOL=./build/alg_pipeline_tool_test ./tools/pipeline_studio/server.py --web
./build/alg_pipeline_tool_test validate demo/fixtures/mock/pipeline_entity_extract_custom.json
```

## 运行当前方案

Pipeline JSON 包含节点、模型、I/O 与输出参数；`.conf` 仅以 `pipe_path` 定位该 JSON。
Profile 提供数据集和运行参数，按有序 I/O 配对。所有模型文件以 Pipeline JSON 目录为基准；
受控方案默认使用 `configs/` 中的资产。导入方案保存到这里前应确保这些相对引用可用。

“检查运行条件”调用与 Create 相同的解析器，区分配置检查和文件存在性，不加载模型。
`configuration.model_files` 逐项报告 `model`、原字段 `path`、绝对 `resolved`；`io` 报告生效参数，
`output_pools` 报告容量。参数覆盖写在对应 Converter 的 `params`；分配器、布局与元数据归槽声明。
池深、单次批次上限与硬上限分别由 `effective_frame_depth`、`effective_process_batch_limit`、
`max_frame_depth_limit` 报告。

“另存为可运行方案”创建 JSON、`.conf` 并提供完整命令；已有目标拒绝覆盖。会话创建的配套文件，
以及在运行页明确“关联部署配置”的文件，会在保存时共同校验 revision、预检并更新；
冲突或预检失败不写入，失败回滚保留其他编辑。关联只确认 `.conf` 指向当前方案，不写文件。
服务重启后需重新关联；普通保存和另存只写 JSON。

也可自行创建 `.conf`，在 `configs/pipeline_first_solution.conf` 中写
`{"pipe_path":"pipeline_first_solution.json"}`，再执行：

```bash
./build/alg_pipeline_tool validate configs/pipeline_first_solution.json
./build/alg_pipeline_tool plan configs/pipeline_first_solution.json
./build/alg_demo --profile keyword_match_rules --config configs/pipeline_first_solution.conf --output-dir results/first-solution
./build/alg_demo --config configs/pipeline_first_solution.conf --dataset data/corpus_keyword_match.txt --output-dir results/first-solution
```

指定 Profile 时结果目录使用 Profile 名；无 Profile 时使用 `.conf` 文件名主体。
按记录顺序核对 `results.jsonl` 的状态与业务字段，以及 `summary.json`。
`chip`、`device_id`、`batch_size`、`depth` 来自 Profile，缺省为 cpu、0、1、1，
自定义时用 `--profiles-file`。Studio 生成的命令引用配套 `demo-profile.json`，不会凭空补齐缺失字段。

草稿预检、保存校验与运行在实际配置目录直接创建自己持有的隐藏临时 JSON / `.conf`，
保持资源路径，结束后清理；结果和日志保存在独立临时目录。方案或运行设置变化会标记旧结果，
切换文档隔离异步返回。同一服务只运行一个任务，可取消并有超时；日志限制 2 MiB，卡片最多 50 条，
完整结果可展开。页面不保存跨刷新运行历史。

Demo 默认执行配置初值。热更新使用显式 `--control-cmd` / `--control-file`，
见[第一个 Control](../../doc/dev_guide/first_control.md)。模型输出与目标设备效果仍需
[独立验收](../../doc/VERIFIABLE_SELECTION.md)。

## 安全边界与交互回归

保存限定受控文件名和目录，拒绝符号链接与路径逃逸。配置先经原生校验，再同目录原子替换。
浏览文件只读取显式选中的单个文件；服务不提供任意路径读取、目录浏览或账号鉴权，不应对外监听。

[操作验收记录](acceptance.md) 描述主要任务与桌面布局。Python 套件覆盖 API 和 Web 模块；
提供已安装的 Playwright 可额外执行真实浏览器回归：

```bash
STUDIO_PLAYWRIGHT_MODULE=/path/to/playwright python3 tests/tooling/test_pipeline_studio.py
```

可用 `STUDIO_CHROMIUM_PATH` 指定浏览器、`STUDIO_SCREENSHOT_DIR` 保存截图；受限容器可显式设置
`STUDIO_BROWSER_NO_SANDBOX=1`。测试仅操作自动清理的临时方案，复用真实关键词 Demo，不需权重。
最终验收入口为 `./scripts/run_all_tests.sh`，可传入同样的浏览器环境变量。
