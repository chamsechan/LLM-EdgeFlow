# LLM-EdgeFlow Pipeline Studio

本目录是 Pipeline Studio 的模块根目录，集中保存 Python 服务端、Web 资源和使用文档。
根目录 `show` 是指向 `server.py` 的稳定启动入口。Web 工作台和自动化 CLI 调用 C++
Catalog 与 Validator，避免工具端维护另一套 Pipeline 规则。

## 前置条件

先从仓库根目录构建工具和 Demo：

```bash
cmake -B build -G Ninja -DLLM_EDGEFLOW_LINKER=auto
cmake --build build -j$(nproc)
```

## 终端查看

```bash
./build/alg_show configs/pipeline_doc_qa.json
```

该命令忠实输出 JSON 中声明的业务名、节点 ID、节点类型和 `depends_on`，不推导执行
计划，也不启动 Web 服务。需要经过校验的拓扑顺序和波前层时，应使用
`alg_pipeline_tool plan`。模型批处理信息仅显示配置中实际声明的
`backend_config.max_batch_size`、`backend_config.decode_batch_size`；未声明时不推测默认值。

## Web 工作台

```bash
# 打开方案列表
./show --web

# 直接打开指定方案
./show configs/pipeline_dialogue_audit.json --web

# 使用自定义端口
./show --web --port 8081
```

服务只绑定 `127.0.0.1`。启动命令会尝试打开浏览器；无法自动打开时，按终端输出的地址访问即可。按 `Ctrl+C` 停止服务。

工作台支持：

- 打开、新建、编辑、校验和原子保存 `configs/pipeline_[a-z0-9_]+.json`。
- 使用 SHA-256 revision 检测 IDE 或 Git 引起的外部修改。
- 在 Graph 与 JSON 视图间同步，并将 Validator 诊断定位到相关节点。
- 在隔离临时目录中使用既有 Profile 运行未保存草稿。

界面以查看流程为默认入口：全宽画布先展示整体关系，进入编辑后再按需展开工具。
布局与交互优化保持现有 Pipeline 格式和 Catalog / Validator 分工，不增加运行时依赖。
界面验收关注常见桌面宽度下工具栏可达、完整流程可见、连线不会穿过无关节点，以及
表单草稿不会被图表刷新覆盖；业务效果仍按下文的运行与效果验收路径核对。

### 查看与编辑

- 打开方案后默认进入浏览模式。拖动空白处平移，滚轮以鼠标所在位置为中心缩放；
  工具栏的 `−` / `+` 调整比例，点击百分比恢复 100%，“适应画布”显示完整流程。
  双击节点可聚焦并放大阅读。“自动布局”重新排列节点并适应画布；实线表示数据流，
  虚线表示独立的执行依赖。
- 点击“编辑方案”修改节点和连线。已有方案首次进入编辑时收起算子面板，可用
  “算子”“属性”按钮展开或收起两侧面板；在算子面板的“新建方案”中选择契约和 Profile。
  点击“浏览流程”可返回全宽画布。
- 连线单击只选中；编辑模式下点击“删除连线”或按 `Delete` 删除。
  使用工具栏撤销/重做，或按 `Ctrl` / `⌘` + `Z` 撤销、
  `Ctrl` / `⌘` + `Shift` + `Z` 或 `Ctrl` / `⌘` + `Y` 重做。
  在文本输入控件中这些编辑快捷键保留给文本自身。
- 节点、模型或 JSON 表单修改后显示“有未应用修改”。先点击“应用”将表单内容更新到
  方案，或点击“放弃修改”恢复表单，再进行保存、校验或运行。一次编辑一个表单；
  节点属性尚未应用时，切换节点也会提示先处理当前修改。应用后仍需保存到文件。

节点坐标保存在浏览器 `localStorage`，不会写入 Pipeline JSON；重新打开或刷新后恢复
坐标并适应当前画布。撤销/重做历史只保留在当前文档的页面内存中，不跨刷新或文档切换。
工作台不提供账号或令牌鉴权，因此不得修改为对外网卡监听。

### 第一次编排

从无需模型的关键词方案开始，先查询实际契约：

```bash
./build/alg_pipeline_tool catalog --biz keyword_match_v1
./build/alg_pipeline_tool describe-node TextRuleMatchNode
./show --web
```

1. 点击“编辑方案”，展开“算子”面板的“新建方案”，选择业务契约 `keyword_match_v1`，
   从 `keyword_match_mock` Profile 克隆并点击“新建方案”。
2. 在画布检查“业务输入”的 `input_sentences` → 节点 `text`，以及节点 `matches` →
   “业务输出”的 `rule_matches`。可选中连线后点击“删除连线”，再从输出端口拖到
   输入端口重新连接；也可以用“撤销”恢复刚删除的连线。
3. 选择规则节点，在属性中将 `categories` 改为 `{"FIRST_RUN":["VIP"]}` 并点击“应用”。
   在“JSON”页确认端口映射；需要模型的方案则先在“模型”页应用实例，再在节点属性绑定它。
4. 在“校验”页调用 C++ Validator，修复诊断后保存为 `pipeline_first_solution.json`。
5. 在“运行”页选择兼容 Profile 可执行草稿，查看日志和结构化结果。草稿运行沿用 Demo
   默认 Control，关键词示例会覆盖规则；要验证刚设置的 `FIRST_RUN` 规则，使用下方
   [运行当前方案](#运行当前方案)中的命令关闭默认 Control。

该练习复用已有 Node、Adapter 与数据集。缺失业务算法时转到
[第一个自定义 Node](../../doc/dev_guide/first_custom_node.md)，新平台结构转到
[业务接入指南](../../doc/dev_guide/business_onboarding.md)。

## 自动化 CLI

```bash
./build/alg_pipeline_tool catalog --biz smart_doc_qa_v1
./build/alg_pipeline_tool describe-node TextEmbeddingNode
./build/alg_pipeline_tool validate configs/pipeline_doc_qa.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa.json
```

编排或修改 Pipeline 时，应先查询 Catalog 与节点 Definition，再执行 validate 和 plan。完整开发流程参见项目的 `pipeline-composer` skill 与[开发者指南](../../doc/developer_guide.md)。

### 校验工具选择

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

Pipeline JSON 描述算法连线；`.conf` 描述部署路径和输出容量；Profile 保存 Demo 的
业务、配置、数据集等预设。克隆或保存新 JSON 不会自动修改原 Profile 或 `.conf`。

完成上述练习后，复制 `configs/pipeline_keyword_match.conf` 为
`configs/pipeline_first_solution.conf`（已有同名文件时直接编辑），将其中
`data.pipe_path` 改为 `configs/pipeline_first_solution.json`，保留原输出池配置。
从仓库根目录执行：

```bash
./build/alg_pipeline_tool validate configs/pipeline_first_solution.json
./build/alg_pipeline_tool plan configs/pipeline_first_solution.json
./build/alg_demo --profile keyword_match_mock --config configs/pipeline_first_solution.conf --no-default-control --output-dir results/first-solution
```

CLI 的 `--config` 覆盖 Profile 原配置，因此不需要新增 Profile。也可以不带 Profile，
显式传入业务、配置和数据集：

```bash
./build/alg_demo --biz keyword_match --config configs/pipeline_first_solution.conf --dataset data/corpus_keyword_match.txt --chip cpu --batch-size 2 --no-default-control --output-dir results/first-solution
```

只有原 Profile 已指向本次方案时，才能直接用它证明本次修改已运行。

检查 `results/first-solution/keyword_match_mock/results.jsonl` 和 `summary.json`：
本练习应有两条成功结果，第一条命中 `FIRST_RUN`，第二条未命中。核对请求 ID、状态
和业务字段，不只看退出码。无 Profile 运行时，结果子目录改为业务名 `keyword_match`。

复用其他 `.conf` 时，还要核对 `data.model_paths` 的模型路径覆盖和输出池容量是否适合
当前方案；Pipeline 校验不代表部署资源可加载。`--no-default-control` 防止 Demo 内置
热更新覆盖所选规则或提示词，显式 `--control-file` 仍会执行，应只在需要该更新时提供。

新增节点命令可用 `--control-cmd <id> --control-file <payload.json>` 经同一 Demo 下发；
命令必须在当前 Pipeline 的节点 Definition 中声明。Profile 可设置 `control_cmd`，CLI
优先。详见[第一个 Control](../../doc/dev_guide/first_control.md)。

Studio 会为当前草稿生成临时 JSON 和指向它的 `.conf`，但继承 Profile 的模型路径覆盖、
容量和 Demo 默认 Control；它没有关闭默认 Control 的界面选项。草稿执行可用于检查
控制流，严格验收所选配置时使用上述 CLI 或[效果验收工具](../../doc/VERIFIABLE_SELECTION.md)。
测试模型输出仅证明执行链路；真实模型效果和目标平台验收需单独记录。

## 安全边界

- 文件操作限制在仓库的 `configs/` 目录和受控文件名内，并拒绝符号链接与路径逃逸。
- 保存前必须通过 C++ Validator；正式文件使用同目录临时文件原子替换。
- 同一工作台最多运行一个草稿任务，可取消且有超时限制。
- 返回日志最多保留 2 MiB；草稿、临时配置和运行结果不会写入正式 `results/`。

## 端口、模型与验收选择

画布按具体端口连线，显示业务输入/输出，生成实际 `ports` 与 `depends_on`。
模型页支持 Model/Backend 完整字段、资产组合及当前构建的兼容检查。
资产存在不代表已验证，更不代表业务效果通过。使用
[可验证选择指南](../../doc/VERIFIABLE_SELECTION.md) 完成标注数据集验收。
