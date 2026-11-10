# LLM-EdgeFlow Pipeline Studio

Pipeline Studio 是本机 Pipeline JSON 编辑器。画布、参数表单和高级 JSON 编辑共用一份文档，
能力声明来自目标构建的 C++ Catalog，执行合法性由原生 Validator 判定。
Studio 负责编辑与保存；模型资产检查和实际执行使用独立工具。

## 前置条件

按根目录 [README 的最小构建步骤](../../README.md#1-获取并构建)准备 `alg_pipeline_tool`。
关键词编排无需模型权重；真实模型使用[匹配的构建变体](../../doc/VERIFIABLE_SELECTION.md#构建变体)。
以下命令从仓库根执行。

## 终端查看

```bash
./build/alg_show configs/pipeline_keyword_match_rules.json
./tools/pipeline_studio/server.py configs/pipeline_keyword_match_rules.json
```

查看器展示声明的 I/O、节点、连接、额外顺序和模型，不加载模型、不生成执行计划。
用 `alg_pipeline_tool plan` 查看执行顺序和并行层。

## Web 编辑器

```bash
./tools/pipeline_studio/server.py --web
./tools/pipeline_studio/server.py configs/pipeline_keyword_match_rules.json --web
./tools/pipeline_studio/server.py --web --port 8081
```

服务仅监听 `127.0.0.1`。打开终端显示的地址，按 `Ctrl+C` 停止服务。
启动时可指定文件，也可从页面顶部选择已有方案或通过“打开 JSON…”导入单个文件，大小上限 4 MiB。
“新建方案”选择有序输入输出 Converter 后创建空草稿；复用示例时直接打开对应 JSON，再另存为。

### 编辑与保存

- 从 Catalog 节点库添加步骤，点击节点修改参数。输入输出由 Converter `(type, name)` 选择，
  与内部节点端口一起显示在画布上。
- 从输出端口拖到输入端口，或依次点击两个端口连接。类型高亮仅辅助选择，连接由原生工具检查。
  点击连线后用“删除连线”或 Delete 删除。Esc 取消，端口支持 Enter / 空格选择。
- 重命名节点同时维护数据引用与额外顺序。断开数据连线保留显式 `depends_on`。
  撤销/重做支持 Ctrl / ⌘ + Z、Shift + Z 或 Y，文本框保留自己的快捷键。
- 普通参数完成输入后自动更新；模型设置点击“更新模型”应用。保存、检查会先处理尚未完成的表单，
  重命名与参数修改在同一次编辑请求中提交。非法输入保留并定位，可用“撤回输入”恢复当前表单。
- 基本参数表单来自 Definition，复杂值可直接编辑 JSON。模型实例、Backend 参数与节点模型绑定均可编辑。
  未修改的默认值不写入 JSON；编辑已知字段保留未知参数及暂时缺失的模型绑定。
- 拖动空白处平移、滚轮缩放；“查看全图”与“放大选中节点”切换阅读范围。连线采用简单折线，
  可手动移动节点或使用自动布局改善阅读。
- “检查”展示原生错误码、JSON Pointer 和节点定位。检查状态与保存状态独立，修改后检查结果失效。

保存仅写 `configs/pipeline_[a-z0-9_]+.json`，输入名称后自动补齐前缀和扩展名。
允许保存缺失连接、模型或参数的半成品，不依赖校验工具和模型资源。
保存使用 revision 检测覆盖冲突，并在同目录原子发布；拒绝路径逃逸和符号链接。
导入外部文件不会覆盖源文件，需另存为。离开未保存的文档会提示；浏览器不暂存草稿。

### 第一次编排

1. 打开 `pipeline_keyword_match_rules.json`。
2. 点击 `match_keywords`，把 `categories` 中的规则改为需要的关键词。
3. 删除一条数据连接，点击“检查”，查看缺失输入诊断。
4. 重新连接或撤销，确认检查通过。
5. 点击“另存为”，输入 `first_solution`，重开文件核对修改。
6. 按[运行当前方案](#运行当前方案)调用 Demo 验证实际输出。

完整外部请求/响应以 Converter 契约为准，内部节点端口不能替代 SDK 业务契约。
缺失算法见[第一个自定义 Node](../../doc/dev_guide/first_custom_node.md)，
新增平台载体见[业务接入指南](../../doc/dev_guide/business_onboarding.md)。

## 自动化 CLI

```bash
./build/alg_pipeline_tool catalog
./build/alg_pipeline_tool describe-node text_embedding
./build/alg_pipeline_tool describe-model embedding onnxruntime
./build/alg_pipeline_tool describe-backend onnxruntime
./build/alg_pipeline_tool export-schema
./build/alg_pipeline_tool validate configs/pipeline_keyword_match_rules.json
./build/alg_pipeline_tool plan configs/pipeline_keyword_match_rules.json
./build/alg_pipeline_tool validate-io configs/pipeline_keyword_match_rules.conf
./build/alg_pipeline_tool resolve-conf configs/pipeline_keyword_match_rules.conf --root . --depth 2
./build/alg_pipeline_tool init --input keyword_in/keyword_match --output json_out/keyword_match --raw
```

Catalog 仅提供当前构建的注册能力，不读取 Demo Profiles。模型包含 `model_type`、`impl_name` 和
`backends`，节点以 `model_type`、`config_field` 声明绑定要求。Schema 从同一套递归字段声明导出。
类型端口、图与执行语义仍由原生 Validator 校验。

`init` 的 `--input` / `--output` 可重复；只生成 I/O 选择及空 `models`、`pipeline` 数组。
默认返回 `{ok, pipeline}`，`--raw` 直接输出文档。克隆已有方案直接复制 JSON，注意保持相对资源路径。
`validate` / `plan` 接受 FILE 或 `--stdin`。FILE 按自身目录解析资源，stdin 仅校验路径写法。
Model 文件与声明的文件参数相对 Pipeline JSON 目录，拒绝绝对路径、父目录分量和符号链接越界。
预检不加载权重。

### 原生编辑请求

`alg_pipeline_tool edit --stdin` 接受完整文档 `pipeline` 和互斥的 `operation` 或非空 `operations`。
`require_valid` 默认 `false`，为 `true` 时最终候选必须通过校验。最多 128 个动作、4 MiB。

| 动作 `kind` | 字段 |
| --- | --- |
| `add_node` | `type`；可选 `name`、对象 `params`，省略名称时分配唯一名称 |
| `remove_node` | `node` |
| `rename_node` | `node`、`new_name` |
| `connect` / `disconnect` | `source`、`target`，各含 `node`、`port` |
| `add_dependency` / `remove_dependency` | `node`、`depends_on` |
| `apply_patch` | `patch`：JSON Patch 数组 |

`input` / `output` 是 I/O 伪节点。节点名非空、不得含点或占用这两个名称。
连接时输出端口匹配多个 Converter 会报告歧义，不隐式选择。
断线按已保存的引用精确匹配，允许清理已失效的节点或端口引用；多个输出项拥有同一目标端口时仍拒绝歧义。
新增连接和依赖复用 Core 的环检查，I/O 准备错误不会跳过该检查。
批量动作作用于副本，失败不返回候选；动作错误带 `failed_operation_index`。
成功返回候选及最终原生校验报告；`ok` 表示编辑成功，不完整草稿还需检查 `validation.ok`。
编辑不会生成修复候选。接口见 [PipelineAuthoring](../../src/cli/pipeline_authoring.h)。

<a id="校验工具选择"></a>
### 工具选择

生产方案使用 `alg_pipeline_tool`，测试 Model/Backend 使用 `alg_pipeline_tool_test`。
查询 Catalog、编辑和校验使用同一工具。Catalog 在页面启动时加载一次；工具新增注册后重新构建并刷新页面。
测试工具通过不代表生产验证通过。

```bash
LLM_EDGEFLOW_PIPELINE_TOOL=./build/alg_pipeline_tool_test ./tools/pipeline_studio/server.py --web
```

## 运行当前方案

Studio 保存 JSON 后，在同目录准备 `.conf`，其唯一字段 `pipe_path` 指向 JSON，例如：

```json
{"pipe_path":"pipeline_first_solution.json"}
```

然后使用现有 Operator / Demo 入口运行：

```bash
./build/alg_pipeline_tool validate configs/pipeline_first_solution.json
./build/alg_pipeline_tool resolve-conf configs/pipeline_first_solution.conf --root . --depth 1
./build/alg_demo --profile keyword_match_rules --config configs/pipeline_first_solution.conf --output-dir results/first-solution
```

也可用 `--dataset` 显式指定匹配的数据集。核对结果目录的 `results.jsonl` 和 `summary.json`。
运行设置由 Demo Profile 管理，Studio 不管理 `.conf`、数据集、运行进程或结果。
模型资源检查与真实模型效果使用[独立验收工具](../../doc/VERIFIABLE_SELECTION.md)。

## 交互回归

[操作验收标准](acceptance.md) 覆盖编辑、诊断和安全保存。Python 套件运行 HTTP 与 Web 模块测试。
已有 Playwright 时可执行真实浏览器回归，无需新增项目依赖：

```bash
STUDIO_PLAYWRIGHT_MODULE=/path/to/playwright python3 tests/tooling/test_pipeline_studio.py
```

可用 `STUDIO_CHROMIUM_PATH` 指定浏览器、`STUDIO_SCREENSHOT_DIR` 保存截图，
受限容器可显式设置 `STUDIO_BROWSER_NO_SANDBOX=1`。测试使用自动清理的临时文档，不需模型或 Demo。
最终验收入口为 `./scripts/run_all_tests.sh`，可传入相同浏览器环境变量。
