# LLM-EdgeFlow Pipeline Studio

本目录是 Pipeline Studio 的模块根目录，集中保存 Python 服务端、Web 资源和使用文档。
根目录 `show` 是指向 `server.py` 的稳定启动入口。Web 工作台和自动化 CLI 调用 C++
Catalog 与 Validator，避免工具端维护另一套 Pipeline 规则。

## 前置条件

首次使用先按根目录 [README 的最小构建步骤](../../README.md#1-获取并构建)准备工具和 Demo；
完成过快速开始后可直接继续。关键词编排练习无需推理后端或模型权重。
需要运行真实模型时，再按[构建变体](../../doc/VERIFIABLE_SELECTION.md#构建变体)选择匹配的
Backend 并重建对应产物。以下命令都从仓库根目录执行。

## 终端查看

`./show 配置.json` 默认在终端显示方案；不提供文件时显示帮助。也可以使用原生
C++ 查看工具，展示更多声明信息：

```bash
./show configs/pipeline_doc_qa_default.json
./build/alg_show configs/pipeline_doc_qa_default.json
```

该命令忠实输出 JSON 中声明的 I/O 契约、节点 ID、节点类型、`inputs` / `outputs` 数据映射，
以及表示额外顺序的 `depends_on`。未声明的映射会标明缺省；查看器不推导执行计划，也不启动
Web 服务。需要经过校验的拓扑顺序和波前层时，应使用
`alg_pipeline_tool plan`。模型批处理信息仅显示配置中实际声明的
`backend_config.max_batch_size`、`backend_config.decode_batch_size`；未声明时不推测默认值。

## Web 工作台

```bash
# 启动工作台，使用“浏览文件…”选择一个 JSON
./show --web

# 直接打开指定方案
./show configs/pipeline_dialogue_audit_default.json --web

# 可直接指定子目录或其他位置的单个文件
./show configs/pipeline_doc_qa_kite.json --web

# 使用自定义端口
./show --web --port 8081
```

服务只绑定 `127.0.0.1`。启动命令会尝试打开浏览器；无法自动打开时，按终端输出的地址访问即可。按 `Ctrl+C` 停止服务。
错误提示会保留至手动关闭或被后续提示替换；启动接口失败时显示接口路径、HTTP 状态和错误原因，便于核对启动与端口转发问题。

只有显式传入 `--web` 才启动 Web；进入后可通过“浏览文件…”选择配置，无需在命令行指定文件。

点击顶部“浏览文件…”可从浏览器所在电脑选择单个 Pipeline JSON；不需要选择目录。
启动参数也可直接指定单个文件。受控 `configs/pipeline_*.json` 以外的文件以导入文档打开，
可以浏览、编辑和校验；修改后“保存”会提示另存到受控 `configs/` 文件，不覆盖导入源。
顶层受控方案仍保留原有保存和 revision 冲突检查。文件上限为 4 MiB。
打开文件只要求可解析的 Pipeline 文档；缺少模型资产、兼容 Backend 或 Catalog 工具时
仍可浏览流程，配置合法性与运行条件由后续校验和运行检查决定。

工作台支持：

- 打开、新建、编辑、校验和原子保存 `configs/pipeline_[a-z0-9_]+.json`。
- 使用 SHA-256 revision 检测 IDE 或 Git 引起的外部修改。
- 在 Graph 与 JSON 视图间同步，并将 Validator 诊断定位到相关节点。
- 在隔离临时目录中使用既有 Profile 运行未保存草稿。

界面以查看流程为默认入口：全宽画布先展示整体关系，点击节点可展开只读属性；“详情”可收起面板，画布工具栏可直接校验或进入运行页。
布局与交互优化保持现有 Pipeline 格式和 Catalog / Validator 分工，不增加运行时依赖。
界面验收关注常见桌面宽度下工具栏可达、完整流程可见、连线不会穿过无关节点，以及
表单草稿不会被图表刷新覆盖；业务效果仍按下文的运行与效果验收路径核对。

### 查看与编辑

- 打开方案后默认进入浏览模式。拖动空白处平移，滚轮以鼠标所在位置为中心缩放；
  工具栏的 `−` / `+` 调整比例，点击百分比恢复 100%，“适应画布”显示完整流程。
  双击节点可聚焦并放大阅读。“自动布局”重新排列节点并适应画布；实线表示数据流，
  虚线表示独立的执行依赖。
- 顶部“新建”直接展开新建表单，选择 I/O 契约和 Profile 即可克隆，也可从空图开始。
  点击“编辑方案”修改节点和连线。已有方案首次进入编辑时收起算子面板，可用
  “算子”“详情”按钮展开或收起两侧面板。浏览时参数只读，校验和草稿运行仍可直接使用。
  点击“浏览流程”收起工具回到全宽画布，再次点击节点可查看属性。
- 连线单击只选中；编辑模式下点击“删除连线”或按 `Delete` 删除。
  使用工具栏撤销/重做，或按 `Ctrl` / `⌘` + `Z` 撤销、
  `Ctrl` / `⌘` + `Shift` + `Z` 或 `Ctrl` / `⌘` + `Y` 重做。
  在文本输入控件中这些编辑快捷键保留给文本自身。
- 节点、模型或 JSON 表单修改后显示“有未应用修改”。点击“应用”更新方案，或点击
  “放弃修改”恢复表单。校验、运行和保存会一次应用当前合法表单并继续；非法字段保留原输入并
  定位错误，不启动运行或保存。切换节点或返回浏览时，可选择“应用并继续”“放弃并继续”或
  “留在当前”。一次编辑一个表单；应用后仍需保存到文件。
- 图上的新增、删节点、重命名和连接操作统一使用原生工具。连接只更新节点顶层 `inputs` /
  `outputs`，数据依赖由 Core Validator 推导；断开直接删除输入映射，保留手写 `depends_on`。
  节点属性可单独增删额外执行顺序；不完整草稿仍可编辑，运行和保存要求校验成功。
- 校验诊断的修复按钮打开页内候选审阅，展开详情检查改动，选择应用或取消。草稿或工具变化会
  使旧候选失效；一次应用只产生一个撤销步骤。
- 自由字符串支持多行编辑；原样应用保留换行、空白、已有空串和可选字段的缺省状态。
  修改后保存实际字符串，清空已有文本会保存 `""`。删除字段恢复缺省、精确设置转义字符，
  或在原本缺省且默认为空时显式设置 `""`，使用已有 JSON 页编辑。

节点坐标保存在浏览器 `localStorage`，不会写入 Pipeline JSON；重新打开或刷新后恢复
坐标并适应当前画布。撤销/重做历史只保留在当前文档的页面内存中，不跨刷新或文档切换。
工作台不提供账号或令牌鉴权，因此不得修改为对外网卡监听。

### 第一次编排

从无需模型的关键词方案开始，先查询实际契约：

```bash
./build/alg_pipeline_tool catalog --io-binding keyword_match.operator.v1
./build/alg_pipeline_tool describe-node TextRuleMatchNode
./show --web
```

1. 点击顶部“新建”，选择 I/O 契约 `keyword_match.operator.v1`，
   从 `keyword_match_rules` Profile 克隆并点击“新建方案”。
2. 在画布检查“业务输入”的 `input_sentences` → 节点 `text`，以及节点 `matches` →
   “业务输出”的 `rule_matches`。可选中连线后点击“删除连线”，再从输出端口拖到
   输入端口重新连接；也可以用“撤销”恢复刚删除的连线。
3. 选择规则节点，在属性中将 `categories` 改为 `{"FIRST_RUN":["VIP"]}` 并点击“应用”。
   在“JSON”页确认端口映射；需要模型的方案则先在“模型”页应用实例，再在节点属性绑定它。
4. 在“校验”页调用 C++ Validator，修复诊断；在“运行”页选择 `keyword_match_rules`，
   点击“另存为可运行方案”，输入 `pipeline_first_solution.json`，同时生成配套 `.conf`。
5. 在“运行”页选择兼容 Profile 可执行草稿，查看日志和结构化结果。Demo 默认使用 Pipeline 配置，
   因此草稿会使用刚设置的 `FIRST_RUN` 规则。检查第一条命中 `FIRST_RUN`、
   第二条未命中；保存后也可使用下方[运行当前方案](#运行当前方案)中的命令验收。

该练习复用已有 Node、Adapter 与数据集。缺失业务算法时转到
[第一个自定义 Node](../../doc/dev_guide/first_custom_node.md)，新平台结构转到
[业务接入指南](../../doc/dev_guide/business_onboarding.md)。

在“运行”页展开“接入契约”，可核对当前方案的 `io_binding`、输入/输出 Converter
及槽位名称。Pipeline 只通过 `deployment.io.io_binding` 选择外部 I/O 契约；
Demo 从 SDK 解析结果选择运行函数，Profile 仅保存配置、数据集和运行选项。
Catalog v4 的 `external_slots` 导出 `slot_name`、`type_id`、`type_suffix` 和有效
`key_suffix`，分别表示逻辑槽、宿主类型、类型注册后缀和外部键后缀。工具未提供的字段
显示为“未提供”；应使用与工作区匹配的 `alg_pipeline_tool`。详情中的内部端口
与槽位类型不代表外部 JSON 载荷协议，完整请求响应仍以对应 Converter 契约为准。

## 自动化 CLI

```bash
./build/alg_pipeline_tool catalog --io-binding doc_qa.operator.v1
./build/alg_pipeline_tool describe-node TextEmbeddingNode
./build/alg_pipeline_tool validate configs/pipeline_doc_qa_default.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa_default.json
```

可将版本化编辑请求交给 `alg_pipeline_tool edit --stdin`，返回候选 Pipeline、变更和校验报告；
请求包含整数 `schema_version: 1`、完整 Pipeline 文档 `pipeline`，以及互斥的单动作对象
`operation` 或非空动作数组 `operations`。可选布尔字段 `require_valid` 默认为 `false`；
设为 `true` 时要求最终候选合法。单次最多 128 个动作、4 MiB。

| 动作 `kind` | 字段 |
| --- | --- |
| `add_node` | 必需 `node_type`；可选 `id` 和对象 `config`，省略 `id` 时分配唯一 ID |
| `remove_node` | `node_id` |
| `rename_node` | `node_id`、`new_id` |
| `connect` / `disconnect` | `source`、`target`，均为包含 `node_id` 和 `port` 的对象 |
| `add_dependency` / `remove_dependency` | `node_id`、`depends_on_id` |

字段中的名称和 ID 必须是非空字符串，未知字段或动作会被拒绝。连线来源的 `node_id` 为
节点实例 ID 或业务输入 `$ingress`，目标为节点实例 ID 或业务输出 `$egress`；`port` 为对应端口名。
批量动作依次作用于同一份候选，最后执行一次校验。动作失败或严格校验失败不返回候选；
动作失败时 `failed_operation_index` 给出从 0 开始的位置。允许不完整草稿时，响应的 `ok: true`
仅表示编辑成功，仍需检查 `validation.ok`。命令只输出 JSON，不写配置文件。
请求和响应实现见 [PipelineAuthoring](../../src/tools/pipeline_authoring.h)。

工具支持新增、删除、重命名节点、连接、断开和单独增删执行依赖。无需补数据依赖，输入也没有
隐式同名绑定。外部文档必须指定 `deployment.io.io_binding`，在 `validate`、`plan`、`edit` 中统一执行部署准备：
模型路径仅保存在 `models[].model_path`，必须为合法非空字符串；
`edit` 同样严格校验未知 I/O 绑定或非法输出分配。

编排或修改 Pipeline 时，应先查询 Catalog 与节点 Definition，再执行 validate 和 plan。完整开发流程参见项目的 `pipeline-composer` skill 与[开发者指南](../../doc/developer_guide.md)。

### 校验工具选择

CLI 克隆默认返回包含 `pipeline` 的版本化响应。需要直接保存 Pipeline JSON 时使用
`init --io-binding <binding_id> --profile <profile_name> --raw`，确认命令成功后再对保存文件执行
`validate`。`--empty --raw` 生成待填写草稿；`--empty` 与 `--profile` 不能同时指定。

正式配置使用目标构建的 `alg_pipeline_tool`；有意使用测试 Model/Backend 的 Smoke
配置使用 `alg_pipeline_tool_test`，查询 Catalog、克隆、校验和计划都保持同一工具。
例如自定义 Node 的测试样例：

```bash
./build/alg_pipeline_tool_test validate demo/fixtures/mock/pipeline_entity_extract_custom.json
./build/alg_pipeline_tool_test plan demo/fixtures/mock/pipeline_entity_extract_custom.json
```

按配置实际引用的注册选择工具，不按 Profile 名称是否包含 `mock` 判断；上面的关键词
练习使用生产注册，无需切换。工具缺失时先构建，新增注册后重新编译。正式配置出现
未知模型或 Backend 时，应检查目标构建与配置，不能用测试工具的通过代替正式校验。

需要在 Studio 编排测试配置时，启动服务前显式选择测试工具：

```bash
LLM_EDGEFLOW_PIPELINE_TOOL=./build/alg_pipeline_tool_test ./show --web
```

### 运行当前方案

Pipeline JSON 描述算法连线，`models[].model_path` 保存唯一模型路径，`deployment.io` 保存接入绑定和输出容量；`.conf` 仅包含 `pipe_path` 用于定位 Pipeline JSON；Profile 保存 Demo 的
配置、数据集和执行参数。“运行”页的“另存为可运行方案”会一起生成 JSON 和 `.conf`，
并提供从项目根执行的完整命令；已有同名文件会拒绝覆盖。选择与 I/O 契约匹配的 Profile，
复用其数据集与运行选项，并从它指向的 Pipeline 读取输出分配配置。模型路径相对项目根解析，
例如 `models/model.onnx` 或 `demo/fixtures/mock/artifacts/neutral-llm.fixture`。新选资产目录默认为 `models`，
仅在资产表单选择新资产时生成模型路径；修改该目录不会改写已有模型条目。运行与已保存方案各自的展开区显示原生部署解析结果，已保存方案同时提供完整命令。

运行页的“检查运行条件”独立调用原生部署解析，不加载模型、不执行 Demo；摘要区分配置检查与
文件存在性，不能代替实际加载和效果验收。方案、部署关联或运行设置改变后，旧摘要标为过期。

本次服务会话创建的配套文件，以及运行页“关联部署配置”明确关联的已有文件，后续点击“保存”
会同步 JSON 和 `.conf`。关联本身不写文件，并校验该 conf 确实指向当前保存的 Pipeline。
预检、运行和保存共用候选配置。关联后，Profile 提供数据集及运行选项，输出分配
来自已关联 Pipeline 的 `deployment.io`。模型表单和原始 JSON 都直接修改 `models[].model_path`；
模型重命名只同步节点引用，保存与运行使用当前模型条目中的路径。
两份文件都会检查修改冲突，预检失败不会写入。
更新后的运行命令和解析结果会一起刷新。详情面板的“保存目标”来自服务端，打开或
保存后列明本次普通保存会写入的文件；按钮悬停也可查看。保存成功显示实际文件名，
保存或运行遇到 Validator 错误时展示可定位诊断；文件冲突以持续可见的提示展示。

其他方案的普通“保存”/“另存”只写 JSON。服务重启后不会自动接管已有 `.conf`，
模型路径仍可直接保存。可在运行页明确关联配套文件后继续成套编辑，也可在外部更新 Pipeline
并用 `resolve-conf` 检查，或另存可运行副本。

若手动使用只写 JSON 的路径，需自行配套 `.conf`：

完成上述练习后，复制 `configs/pipeline_keyword_match_rules.conf` 为
`configs/pipeline_first_solution.conf`（已有同名文件时直接编辑），将其中
`pipe_path` 改为 `pipeline_first_solution.json`（相对 `.conf` 所在目录）；部署 I/O 绑定与输出分配在 `pipeline_first_solution.json` 的 `deployment.io` 下配置，模型路径在 `models[].model_path` 中配置。
从仓库根目录执行：

```bash
./build/alg_pipeline_tool validate configs/pipeline_first_solution.json
./build/alg_pipeline_tool plan configs/pipeline_first_solution.json
./build/alg_demo --profile keyword_match_rules --config configs/pipeline_first_solution.conf --output-dir results/first-solution
```

CLI 的 `--config` 覆盖 Profile 原配置，因此不需要新增 Profile。也可以不带 Profile，
显式传入配置和数据集，Demo 会从配置中的 I/O 契约确定运行入口：

```bash
./build/alg_demo --config configs/pipeline_first_solution.conf --dataset data/corpus_keyword_match.txt --output-dir results/first-solution
```

`chip`、`device_id`、`batch_size`、`depth` 仅从 Profile JSON 读取，不支持同名 CLI
选项。需要自定义时修改 Profile，或通过 `--profiles-file <path> --profile <name>` 选择自有
Profile；未指定时默认值分别为 `cpu`、`0`、`1`、`1`。无 Profile 的命令按单条提交。
平台名只接受 `ax650`、`ascend310p`、`ascend910b`、`rk3588`、`cuda`、`cpu`
（大小写不敏感）。
Studio 将这四项连同配置和数据集写入运行 Profile；保存方案返回的命令引用输出目录
中的 `demo-profile.json`；每次保存更新该文件，复制的命令读取最新运行配置。
Studio 不为缺失执行字段补值，预检按批次和深度缺省 1 计算。
选择 Profile 时通过原生 `resolve-conf` 获取实际 Pipeline 路径，
相对 `pipe_path` 始终基于 `.conf` 所在目录，不搜索项目根目录下的同名文件。

只有原 Profile 已指向本次方案时，才能直接用它证明本次修改已运行。

检查 `results/first-solution/keyword_match_rules/results.jsonl` 和 `summary.json`：
本练习应有两条成功结果，第一条命中 `FIRST_RUN`，第二条未命中。核对请求 ID、状态
和业务字段，不只看退出码。无 Profile 运行时，结果子目录为 SDK 解析出的业务名 `keyword_match_v1`。

每份方案显式填写 `deployment.io.io_binding`，无需填写根级 `biz_name`。
`deployment.io.out_mem` 可省略，必需输出采用注册默认分配；可选输出通过显式槽位配置启用。
输出类型由注册槽位确定；非默认容量、分配器和布局参数在 `out_mem` 中覆盖。
复用其他配置时，还要核对 Pipeline `models[].model_path` 的模型路径和 `deployment.io.out_mem` 输出池容量是否适合
当前方案；Pipeline 校验不代表部署资源可加载。Demo 默认不发送内置
热更新覆盖所选规则或提示词，显式 `--control-file` 仍会执行，应只在需要该更新时提供。

新增节点命令可用 `--control-cmd <id> --control-file <payload.json>` 经同一 Demo 下发；
命令必须在当前 Pipeline 的节点 Definition 中声明。Profile 可设置 `control_cmd`，CLI
优先。详见[第一个 Control](../../doc/dev_guide/first_control.md)。

Studio 为草稿生成项目内的临时 JSON 和 `.conf`。未关联部署时，从 Profile 指向的 Pipeline
复用输出分配配置，保留当前 Pipeline 的 `models[].model_path`，并使用原生
解析器预检；运行结束清理临时文件。草稿运行
使用配置初值，Control 练习通过 CLI 显式下发。解析成功说明部署配置可接受，不代表模型
已加载；日志和样本结果用于确认实际执行。
运行页先展示运行状态、Demo 返回的成功/失败样本数和可展开的逐请求输出，再按需查看
日志与原始 JSON。样本卡片最多展示前 50 条，完整内容在原始结果中。运行记录包含提交时
的方案名、时间、Profile 和新选资产目录；方案、表单草稿或运行设置变化后，旧结果会标记为
提交时的版本。撤销回到相同内容和设置后可恢复匹配状态。切换文档会隔离旧结果，即使
任务启动或轮询稍后返回也不会填入新文档；仍在执行的任务可在运行页取消。同一服务仍
仅允许一个任务，页面不保存跨刷新运行历史。进程完成与样本成功分别展示。

需要显式 Control 测试时使用上述 CLI；业务效果验收使用
[效果验收工具](../../doc/VERIFIABLE_SELECTION.md)。
测试模型输出仅证明执行链路；真实模型效果和目标平台验收需单独记录。

## 安全边界

- 服务端保存限制在仓库的 `configs/` 目录和受控文件名内，并拒绝符号链接与路径逃逸。
- CLI 仅提供显式选中文件的读取快照；浏览器文件选择只读取用户选择的内容，不提供任意服务端路径读取或目录浏览接口。
- 保存前必须通过 C++ Validator；正式文件使用同目录临时文件原子替换。
- 同一工作台最多运行一个草稿任务，可取消且有超时限制。
- 返回日志最多保留 2 MiB；草稿、临时配置和运行结果不会写入正式 `results/`。

## 端口、模型与验收选择

画布按具体端口连线，显示业务输入/输出，生成节点顶层 `inputs` / `outputs`；数据依赖由框架推导。
模型页支持 Model/Backend 完整字段、资产组合及当前构建的兼容检查。
选择资产时，主模型路径相对项目根，词表等附属文件相对模型文件所在目录；
资产模板若将附属文件放在模型目录之外，需在模型表单中填写可用的绝对附属文件路径。
无兼容 Backend 的 Model 类型会标注“当前构建无兼容 Backend”，选择后显示缺失的协议，
并禁止提交该模型组合；已有不可用模型仍可浏览。兼容提示来自 Catalog，不能代替模型
资产、设备和业务效果验收。
资产存在不代表已验证，更不代表业务效果通过。使用
[可验证选择指南](../../doc/VERIFIABLE_SELECTION.md) 完成标注数据集验收。

## 交互回归

[操作验收记录](acceptance.md) 记录核心任务路径和桌面布局检查。现有 Studio Python
套件覆盖 API 与 Web 模块；已安装 Playwright 时，可通过 `STUDIO_PLAYWRIGHT_MODULE`
指定其 Node 模块路径，让同一套件额外执行真实浏览器回归。可选
`STUDIO_CHROMIUM_PATH` 指定浏览器，`STUDIO_SCREENSHOT_DIR` 保存截图；受限容器中如需
关闭浏览器沙箱，可显式设置 `STUDIO_BROWSER_NO_SANDBOX=1`。

```bash
STUDIO_PLAYWRIGHT_MODULE=/path/to/playwright python3 tests/tooling/test_pipeline_studio.py
```

浏览器测试仅操作自动清理的临时方案，复用原生 Validator 和真实关键词 Demo，无需模型权重。
最终交付仍使用仓库统一门禁 `./scripts/run_all_tests.sh`；上述环境变量也可传给门禁。
