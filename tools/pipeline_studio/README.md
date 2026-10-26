# LLM-EdgeFlow Pipeline Studio

Pipeline Studio 是 Pipeline JSON 的可视化编辑器，附带示例数据试运行。JSON 是唯一的方案文档；画布、参数表单和高级 JSON 编辑共用这份文档。节点能力与执行合法性仍来自 C++ Catalog 与 Validator。
本目录包含 Python 服务端、Web 资源和使用文档。

## 前置条件

先按根目录 [README 的最小构建步骤](../../README.md#1-获取并构建)准备工具与 Demo。
关键词编排无需模型权重。真实模型使用[匹配的构建变体](../../doc/VERIFIABLE_SELECTION.md#构建变体)。
以下命令从仓库根执行。

## 终端查看

```bash
./tools/pipeline_studio/server.py configs/pipeline_doc_qa_default.json
./build/alg_show configs/pipeline_doc_qa_default.json
```

查看器展示声明的 I/O 项、节点 `type` / `name`、`inputs` 引用、`depends_on`，以及模型类别、
Backend、文件和显式参数。原生查看器只显示已配置的 `backend.params.max_batch_size` 与
`decode_batch_size`，不推测默认值。查看操作不加载模型，也不生成执行计划。用 `plan` 查询执行顺序和并行层。

## Web 工作台

```bash
./tools/pipeline_studio/server.py --web
./tools/pipeline_studio/server.py configs/pipeline_doc_qa_kite.json --web
./tools/pipeline_studio/server.py --web --port 8081
```

使用 `--web` 启动服务。服务只监听 `127.0.0.1`。
启动后，打开终端显示的地址。按 `Ctrl+C` 停止服务。
如果启动时未指定文件，可在页面点击“打开 JSON…”选择单个 JSON。文件大小不得超过 4 MiB。

页面直接进入编辑。方案保存为服务 `configs/` 目录下的 `pipeline_[a-z0-9_]+.json`，
保存时只需输入名称，前缀与扩展名自动补齐。其他位置的文件可通过“打开 JSON…”导入，
保存为新文件，导入源文件保持原样。
保存只检查文档结构并原子写入 JSON，允许缺失连接、模型或尚未完成的参数，
不依赖校验工具、测试样例或模型资源。执行前必须通过原生校验。
SHA-256 revision 用于防止覆盖其他程序或页签的修改。

Studio 不管理 `.conf`，没有部署关联或配套保存。服务端试运行时自动创建、清理临时
JSON / `.conf`，复用现有 Operator / Demo 入口，保持 Pipeline 目录的资源解析基准。
测试样例提供数据及运行参数，不写入方案 JSON。

### 查看与编辑

- 从示例开始，或打开已有 JSON。已有方案也可从顶部列表打开。
- 默认隐藏节点库与参数面板；点击“添加步骤”搜索，点击节点查看参数。
  节点库与参数表单直接显示 Catalog 中的节点类型、说明、字段名和字段语义，可按类型或说明搜索。
- 常规节点和输入输出参数完成输入后自动更新。保存或试运行会先处理尚未完成的表单；
  正在提交的重命名完成后会继续执行本次操作，无需再次点击。
  非法输入保留并展开定位错误，也可用“撤回输入”恢复当前表单，不影响之前已完成的修改。
  必填及已设置的参数直接显示，其余可选参数收在“更多参数”中。
  映射与对象数组字段按 Definition 的 `items` 声明提供逐项编辑。
  嵌套 JSON、模型设置及完整 JSON 编辑保留高级入口；模型设置需点击“更新模型”应用。
- 修改当前方案不会立即写磁盘。文件状态（已保存／未保存）与检查状态独立显示。
  不完整方案也能保存；试运行前会检查连接、类型与配置。
- 有未保存修改时，刷新或关闭页面前会提示确认；修改不在浏览器中暂存。
- 拖动空白处平移、滚轮缩放。复杂图首次打开优先保持节点可读；“查看全图”主动切换为概览，
  双击节点或“放大选中节点”可返回局部阅读。参数面板左边缘可拖动调宽，键盘方向键也可调整。
- 从输出端口拖到输入端口，或依次点击两个端口连接。类型相同的目标高亮；松开或第二次点击时
  靠近高亮端口即吸附，其他输入端口需直接点中，最终由原生校验判断。拖动接近画布边缘时自动平移。
  缩小画布时端口命中区放大，但不会覆盖相邻端口。Esc 取消；端口支持键盘 Enter / 空格选择。
- 点击连线后用“删除连线”或 Delete 删除。额外执行顺序位于步骤高级设置，断开数据连接保留
  显式 `depends_on`。撤销/重做支持 Ctrl / ⌘ + Z、Shift + Z 或 Y；文本框保留自身快捷键。
- 默认值只作提示，不把未修改的默认值写入 JSON。未知参数在编辑已知字段时保留。
- “试运行”打开测试面板，使用当前修改和所选样例，无需先保存。
  无匹配数据时仍可编辑、保存。结果对应提交时的文档，后续修改会提示结果过期。
- 检查诊断保留节点定位与原生详情。修复候选先预览，再确认应用；一次修复可以整体撤销。

### 第一次编排

```bash
./tools/pipeline_studio/server.py --web
```

1. 点击“从示例开始”，选择 `keyword_match_rules`，创建方案。
2. 点击 `text_rule_match` 节点，在 `categories` 中将键改为 `FIRST_RUN`，值改为 `VIP`。
3. 点击“试运行”，使用默认匹配的测试样例，再点击“试运行”。
4. 检查第一条结果命中 `FIRST_RUN`，第二条未命中。
5. 点击“保存”，输入名称 `first_solution`，保存为 `configs/pipeline_first_solution.json`。

可以删除一条连接再重新连接，也可撤销恢复。无需了解 `.conf`、Profile 或 Backend 的配置组织。
输入输出的完整外部载荷仍以 Converter 契约为准；内部节点端口不能替代外部请求/响应契约。
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

Catalog 的模型条目包含 `model_type`（类别）、`impl_name`（实现）和 `backends`（可选后端）。
节点依赖用 `model_type` 和 `config_field` 表示绑定要求。Schema 从当前构建的递归声明导出，
覆盖数组、映射、嵌套结构和 JSON 值。端口语义与执行合法性仍由原生 Validator 判断。

克隆使用 `init --profile NAME`，直接保存 JSON 时加 `--raw`。新建空草稿使用可重复的
`--input TYPE/NAME --output TYPE/NAME`。只填 I/O 选择，`models`、`pipeline` 是空数组，待补齐后校验。
Profile 的 `io` 摘要只包含有序 `(type, name)`。容量覆盖不改变外部契约匹配。
Catalog 的 Profile 条目保留全部显式运行设置（包括 `depth`、`control_cmd` 与 `control_file`），
并附加 `name` 和 `io`；未设置的运行默认值由 Demo 处理，不写入条目。

`validate` / `plan` 接受 FILE 或 `--stdin`。FILE 按自身目录解析资源。stdin 没有文件位置，
仅校验路径写法。Model 的 `file` 与声明的文件参数相对 Pipeline JSON 目录，拒绝绝对路径、
父目录分量和符号链接越界。预检不加载权重。

### 原生编辑请求

`alg_pipeline_tool edit --stdin` 接受完整文档 `pipeline` 和互斥的 `operation` 或非空 `operations`。
`require_valid` 默认为 `false`。为 `true` 时最终候选必须通过校验。最多 128 个动作、4 MiB。

| 动作 `kind` | 字段 |
| --- | --- |
| `add_node` | `type`；可选 `name`、对象 `params`，省略名称时按类型分配唯一名称 |
| `remove_node` | `node` |
| `rename_node` | `node`、`new_name` |
| `connect` / `disconnect` | `source`、`target`，各含 `node`、`port` |
| `add_dependency` / `remove_dependency` | `node`、`depends_on` |
| `apply_patch` | `patch`：JSON Patch 数组，使用原生 JSON 库执行 |

伪节点 `input` / `output` 代表 I/O 边界。节点名非空、不得含点、不得占用这两个名称。
目标 `output` 的逻辑端口若匹配多个选中 Converter，返回 `AMBIGUOUS_OUTPUT_PORT` 并列出候选，
不隐式选择或广播。新增连接和依赖由 Core 检查环，产生 `DAG_CYCLE` 时始终拒绝该动作。

批量动作作用于文档副本。任一动作或严格校验失败不返回候选，动作错误给出
`failed_operation_index`。成功的 `ok` 表示编辑成功，不完整草稿仍须检查 `validation.ok`。
命令只输出 JSON。接口见 [PipelineAuthoring](../../src/cli/pipeline_authoring.h)。
修复预览也使用 `edit` 的 `apply_patch` 操作，共用候选校验。浏览器应用前重新核对文档与工具版本，
直接展示该次预览的报告，不重复校验同一候选。

<a id="校验工具选择"></a>
### 工具选择

生产方案使用 `alg_pipeline_tool`。测试 Model/Backend 使用 `alg_pipeline_tool_test`。
查询 Catalog、克隆、编辑和校验时，使用同一工具。
Catalog 在页面启动时加载一次，打开、编辑、撤销不重新加载。
新增注册后，重新构建工具并刷新页面。测试工具通过不代表生产验证通过。

```bash
LLM_EDGEFLOW_PIPELINE_TOOL=./build/alg_pipeline_tool_test ./tools/pipeline_studio/server.py --web
./build/alg_pipeline_tool_test validate demo/fixtures/mock/pipeline_entity_extract_custom.json
```

## 运行当前方案

Studio 使用当前 JSON 与所选测试样例运行，不要求先保存。
“检查运行条件”复用原生配置解析器，检查模型文件与运行组件，不加载权重。
模型文件及声明为文件的参数相对 Pipeline JSON 目录解析；导入方案在受控目录试运行时，
需要在该目录准备相应资源。真实模型效果和目标设备仍需[独立验收](../../doc/VERIFIABLE_SELECTION.md)。

运行日志保留末尾 2 MiB。异常、超时也保留日志；取消后回收子进程才发布终态。
同一服务一次执行一个任务。切换方案隔离异步结果，旧任务仍可取消；不保存跨刷新运行历史。
测试样例由开发者维护在 `demo/profiles.json`，按有序输入输出契约匹配。
界面展示实际数据集，完整运行参数仍由 Demo 处理。

### 在 Studio 外调用 Demo

宿主及 CLI 的部署入口仍使用 `.conf`，其唯一字段 `pipe_path` 指向 JSON。
这属于调用方的启动配置，Studio 保存 JSON 时不会创建、修改或校验旁边的 `.conf`。
例如手动创建 `configs/pipeline_first_solution.conf`，内容为
`{"pipe_path":"pipeline_first_solution.json"}`，然后执行：

```bash
./build/alg_pipeline_tool validate configs/pipeline_first_solution.json
./build/alg_pipeline_tool plan configs/pipeline_first_solution.json
./build/alg_demo --profile keyword_match_rules --config configs/pipeline_first_solution.conf --output-dir results/first-solution
./build/alg_demo --config configs/pipeline_first_solution.conf --dataset data/corpus_keyword_match.txt --output-dir results/first-solution
```

按记录顺序核对结果目录中的 `results.jsonl` 和 `summary.json`。
需要自定义 `chip`、`device_id`、`batch_size`、`depth` 等运行参数时使用 Demo 的
`--profiles-file`。热更新见[第一个 Control](../../doc/dev_guide/first_control.md)。

## 安全边界与交互回归

保存限定受控文件名和目录，拒绝符号链接与路径逃逸。文档先检查结构，再同目录原子替换；执行前调用原生校验。
文件选择功能只读取用户选中的文件。服务不提供任意路径读取、目录浏览或账号鉴权。
不要将服务开放到外部网络。

[操作验收标准](acceptance.md) 描述主要任务与桌面布局。Python 套件覆盖 API 和 Web 模块。
提供已安装的 Playwright 可额外执行真实浏览器回归：

```bash
STUDIO_PLAYWRIGHT_MODULE=/path/to/playwright python3 tests/tooling/test_pipeline_studio.py
```

可用 `STUDIO_CHROMIUM_PATH` 指定浏览器、`STUDIO_SCREENSHOT_DIR` 保存截图。受限容器可显式设置
`STUDIO_BROWSER_NO_SANDBOX=1`。测试仅操作自动清理的临时方案，复用真实关键词 Demo，不需权重。
最终验收入口为 `./scripts/run_all_tests.sh`，可传入同样的浏览器环境变量。
