# RFC 0042: 文件浏览、模板语义与构建边界修复

- **RFC 编号**：0042-studio-and-contract-boundaries
- **创建日期**：2026-09-07
- **文档状态**：Completed
- **关联分支**：`fix/studio-and-contract-boundaries`
- **目标版本**：v10.x
- **负责人 / 作者**：Codex
- **关联决策**：补充 RFC-0006、RFC-0025、RFC-0030；修订 RFC-0039 的模板语义。

## 1. 问题与范围

审计复核后，用户选择修复 Studio 单文件打开、提示词占位符语义差异、当前构建的
模型可用性提示、路径基础函数重复及各层过宽的 include 搜索路径。本次不扩展目录
浏览功能，不改变公共 C ABI、模型协议或 Control 广播语义。

## 2. 决策与权衡

- **Tooling**：`./show pipeline.json` 默认终端显示，只有显式 `--web` 才启动 Web；
  裸 `./show` 显示帮助，不增加另一种终端模式参数。启动参数可指定单个 JSON 文件，包含 `configs/kite/` 的文件。
  Web 提供原生文件选择按钮，读入用户选择的 JSON 作为当前文档。浏览不要求目标
  Backend 已编译或模型资产存在；校验继续由 C++ Validator 执行。
- CLI 只将显式选择文件的内容提供给浏览器，不提供任意服务端路径读取接口。
  浏览器文件选择通过 File API 读取内容，不获取宿主文件路径。外部导入文档不绑定
  服务端写入路径；修改可通过现有受控另存保存，不自动覆盖原文件。现有受控配置
  的 revision、原子保存与符号链接边界保留。
- 模型选项由已有 Catalog 的协议兼容信息生成提示，明确区分模型已注册与当前有
  匹配 Backend；不新增维护模型事实的手工列表，不把协议匹配当作资产或硬件验收。
- **能力节点层 / Capability Nodes**：标准模板入口共享 `nodes/text_template.h` 解析器，
  `{{name}}` 和兼容的 `{name}` 都是变量，普通 JSON 花括号按字面保留，请求值不再解析。
  PromptGuided 增加 `template_syntax`：默认 `auto` 对不含双括号的旧模板保留旧行为，
  遇 `{{` 或 `}}` 明确拒绝；`standard` 采用共享语义；`legacy` 显式保留旧转义。
  官方示例显式使用 `standard`，避免复制模板后悄悄改变含义。
- **接入适配层 / Integration、流程编排层 / Orchestration**：在中立契约目录提取
  无文件系统副作用的路径原语。Integration 保留部署 root、绝对路径、文件存在性和
  symlink 边界；Orchestration 保留纯词法校验，不接收部署目录职责。
- **Build / 四层**：运行时只传播中立契约头，各层 OBJECT target 私有使用按文件筛选的
  include view，不再接收仓库根、整个 `src/` 或完整 `include/`。Node 的 Core 契约清单
  由 CMake 和 LayerGuard 共用；工具与测试显式保留聚合搜索范围。view 采用指向源码的
  符号链接，平台不支持时使用 CMake 跟踪的副本，头文件增删触发重配。
  LayerGuard 继续处理相对路径、公共头间依赖及 vendor 归属；include 搜索路径不是
  文件系统安全沙箱。

## 3. 兼容与迁移

保留 `show` 默认终端显示的用法；`./show --web` 启动后通过按钮选择文件即可。
浏览器导入和 CLI 指定外部文件只授权读取该文件内容；保存仍需明确的受控目的文件。
PromptGuided 未标注语法且包含双括号的旧模板现在会校验/初始化失败，需要显式选择
`standard`（变量）或 `legacy`（旧转义）。这是一项主动阻止静默变化的迁移要求；
不含双括号的现有模板和 TextTemplate 正常语义不变。不修改历史 RFC 来掩盖旧行为。

## 4. 验证与完成条件

- 现有 Studio 套件验证子目录单文件和外部文件打开、CLI 模式、浏览器导入、非法
  JSON 不破坏当前文档、未保存修改保护及模型无兼容 Backend 提示。
- Node 套件验证跨模板相同占位符语义、字面 JSON、未知变量、迁移诊断与值不递归展开。
- 路径套件验证 root 内外、合法 `..name`、父目录遍历、绝对路径及 symlink，保留各入口策略。
- 构建套件验证正常依赖可编译、普通越层私有 include 不可编译，LayerGuard 自测通过。
- 按 CONTRIBUTING 运行一次完整 `./scripts/run_all_tests.sh`，核对适用的配置和 Demo。

## 5. 实施与最终结果

五项修复均已完成，实际实现遵循上述兼容规则。

- `./scripts/run_all_tests.sh` 默认完整门禁通过，89/89 CTest 成功。现有 Python CLI
  回归保留无模式参数的终端调用；Studio 专项包含 20 个测试，覆盖无文件帮助和显式 Web 启动。
- 两份 custom 配置的 validate/plan 通过，实体抽取和文档问答 Demo 均退出 0；
  请求 30001 的 11 个实体，以及请求 10001/10002 的完整答案、意图、分片数与置信度
  逐项符合预期，summary 均无失败。这是测试模型的工程执行验证。
- Chromium 实测：CLI 选中的 Kite 文件可绘图，原生文件选择器可导入其他文件；
  非法 JSON/不可渲染结构和取消替换均保留当前文档；缺少 Catalog 工具时仍可浏览；
  无兼容 Backend 的模型显示原因且不能提交。未发现页面 JavaScript 异常。
- LayerGuard 使用真实目标 include 参数验证允许与拒绝的头文件；Ninja、Ninja
  Multi-Config、Unix Makefiles 的头新增、修改和删除增量验证通过。

使用方式见 [Pipeline Studio](../../tools/pipeline_studio/README.md)，模板迁移见
[custom Node 作者指南](../../src/custom_nodes/README.md)，当前构建边界见
[架构设计](../architecture.md#编译期边界与-composition-root)。不包含真实模型效果或
目标硬件验收。
