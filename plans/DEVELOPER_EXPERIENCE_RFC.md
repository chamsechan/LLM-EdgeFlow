# RFC：业务开发者体验改进

| 项 | 内容 |
| --- | --- |
| 状态 | 实施中；按已确认范围分阶段验收 |
| 基线 | `main@26d60f2`（2026-10-02） |
| 详细设计 | [DEVELOPER_EXPERIENCE_DESIGN.md](DEVELOPER_EXPERIENCE_DESIGN.md) |
| 关联文档 | [FRAMEWORK_SIMPLIFICATION_PLAN.md](FRAMEWORK_SIMPLIFICATION_PLAN.md)：本 RFC 取代其阶段 1、2 |

本文件是工作文档，按 [plans/ 的规则](README.md)存放：每一项落地时，现行规则写进对应的指南；
全部合入后删除本 RFC 和详细设计（CONTRIBUTING §3、§5）。

**进度**（第一阶段验收后更新）：

| 项 | 状态 |
| --- | --- |
| WI-1 业务源码自动收录 | 已完成 |
| WI-2 注册类诊断（含 R1–R3、WI-8b） | 已完成 |
| WI-4 签名编译期提示、WI-5 速查与教程精简、WI-8a | 已完成 |
| WI-7 整理计划文件 | 已完成 |
| WI-3 文档链接检查 | 已完成 |
| WI-3 业务接入指南拆分、WI-8c | 已完成 |
| WI-6 CODEOWNERS | 未开始 |
| O-1、O-2 | 待决定 |
| 真实开发者试用（第 11 节） | 未开始 |

## 1. 背景

本 RFC 来自一次面向业务开发者（能读写简单 C++）的项目审查。结论是：用已有能力编排方案、
编写简单 Node 已经比较顺手；新增外部契约、出错时的提示和部分文档还不够友好。

下表是要解决的问题。证据都在基线上实测或对照源码得出，复现方法见详细设计附录 A。

| # | 问题 | 证据 |
| --- | --- | --- |
| P1 | 新增业务源码要手工登记 CMake；漏登记时文件不编译，直到 `validate` 才报 `Unknown node_type` | `src/custom_nodes/CMakeLists.txt` 等是显式列表；测试目录早已使用 `file(GLOB ... CONFIGURE_DEPENDS)`（`tests/RuntimeTests.cmake:41`） |
| P2 | 配置引用了当前构建没有注册的类型时，诊断只有一句英文，没有下一步，还会连带误报 | 用生产工具校验 mock 配置：`UNKNOWN_MODEL_TYPE`、`UNKNOWN_BACKEND` 没有 remediation，另有一条 `UNKNOWN_MODEL_REFERENCE` 说模型"不符或未声明"，实际已声明。`node_type` 拼错一个字母时，除 `UNKNOWN_NODE_TYPE` 外还有两条互相重复的 `MISSING_BIZ_OUTPUT` |
| P3 | 业务接入指南把写给转换器作者的内容和写给宿主集成方的内容混在一起，其中一节与输出分配文档重复 | `doc/dev_guide/business_onboarding.md` 共 319 行。第 4 节（行 181–206）与 `operator_output_allocation.md` 的"实现与注册"一节内容重复；第 6 节（行 238–291）大半是宿主调用与生命周期规则 |
| P4 | Batch `Run` 签名写错时，编译报错有误导性 | 把 Options 和 Models 参数对调后，GCC 报 `function_node.h:144` 的 `return fn(inputs, params)` "invalid initialization of reference"，指向两参数的兜底分支。Map 已有 static_assert（`function_node.h:96`）；LLM 两函数路径较宽容 |
| P5 | 入门文档夹带维护历史和写死的数量 | `first_custom_node.md:84-94` 一段列出约 10 个 API 名，并写"12 个生产 Node 已统一迁移"；同类数量描述共 5 处 |
| P6 | 多团队共用时，框架代码和业务代码没有审查边界 | 仓库没有 CODEOWNERS；"只在证明有缺口时才改 Core/Model/Backend"只靠文档约束 |
| P7 | 计划文件与现状不一致 | `FRAMEWORK_SIMPLIFICATION_PLAN.md:297`（基线时位于仓库根目录，现已移到 `plans/`）写阶段 3"尚未提交"，实际已合入；阶段 1、2 没有实施，但文件里没有状态标记 |
| P8 | 若干细节 | 手写的测试套件会被编译但不会运行，报错不说改哪里；未知配置字段的顶层 `suggestions` 按 schema 顺序排列，而不是按相似度；面向方案作者的"快速反馈"一节是英文 |

## 2. 目标

1. 新增 Node、转换器、业务绑定、Demo 源文件后，直接构建就能编入，不需要改 CMake。
2. "当前构建没有注册该类型"这一类错误给出可执行的下一步，并且不再连带误报。
3. 写转换器的业务开发者只需要读与自己相关的内容；同一条规则只在一处维护。
4. Node 签名写错时，第一条编译错误就给出可接受的签名。
5. 为多团队共用明确框架代码与业务代码的审查边界。
6. 工作文档与现状一致。

## 3. 约束与非目标

### 3.1 外部契约不变（硬约束）

以下内容一律不改：Operator API 与导出符号、宿主结构体、槽位名、输出池语义、Demo 与 SDK 的
交互方式、`.conf` 格式、Pipeline JSON 格式、Control 命令 ID（宿主按 ID 下发命令）。
详细设计为每一项单独写明对外部契约的影响。

### 3.2 允许调整的内部约定

项目尚未投产。与内部约定冲突、但收益明显的改动可以做。本 RFC 调整的内部约定如下：

| 现有约定 | 调整为 | 所属项 |
| --- | --- | --- |
| 源码在 CMakeLists 中逐个登记 | 业务目录自动收录；框架机制文件、Core、Engine 仍显式登记 | WI-1 |
| 脚手架提供 `--add-to-cmake` | 删除，不保留别名（CONTRIBUTING §3：发布前不保留别名） | WI-1 |
| Validator 对同一根因报告多条诊断 | 只抑制完全由未注册项造成的连带诊断；同一位置上可以独立判断的错误照常报告 | WI-2 |
| LLM 钩子返回值：`BuildPrompt` 要求能隐式转换为 `std::string`，`FormatAnswer` 只要求能赋值给 `std::string` | 两个钩子统一规则。`FormatAnswer` 不再接受 `char`、`int` 等算术类型（**收紧**）；`BuildPrompt` 可以返回 `std::string_view`（放宽） | WI-4 |
| 计划第 9 节："大规模改写文档：不做" | 只对业务接入指南做一次按读者的定向拆分 | WI-3 |

### 3.3 非目标

- 任何改动外部契约的方案：拆分部署配置与模型配置、合并两处 `max_batch_size`、把接线挪到
  JSON、新增 SDK 查询接口、Demo 自动分批、Control 命令 ID 自动分配。
- 计划第 9 节已经否决、理由仍然成立的方案（BizDefinition 由 Binding 派生、派生基类写节点、
  同义注册宏等），本 RFC 不重新讨论。
- 统一错误类型：收益不足。
- 本地门禁不再强制 Java：CI 已经强制跑图检查，本地放宽只省一次安装。
- 在公开头文件 `include/edgeflow/operator/interface.h` 中补充宿主生命周期注释：该文件随
  SDK 交付给宿主，留到内网集成阶段与真实 SDK 一起处理。
- 等试用结果再决定：Adapter 脚手架、Batch 拆分/过滤预设工厂。

## 4. 方案总览

| 编号 | 内容 | 收益 | 规模 | 依赖 |
| --- | --- | --- | --- | --- |
| WI-1 | 业务源码自动收录（原计划阶段 1） | 消除漏登记导致的静默失败；教程少一步 | 小 | — |
| WI-2 | 注册类诊断：remediation、相近名称、去掉连带误报、工具层提示 | 新手最常碰到的错误有明确的下一步 | 中 | — |
| WI-3 | 业务接入指南按读者拆分；新增文档链接检查 | 转换器作者要读的内容净减约 50 行，去掉重复内容和宿主层概念 | 中 | WI-1 |
| WI-4 | Batch `Run` 与 LLM 钩子签名的编译期提示（原计划阶段 2） | 签名写错时第一条报错就给出答案 | 小 | — |
| WI-5 | Node 签名速查表；教程去掉维护历史和写死的数量 | 三种写法一处可查 | 小 | WI-4 |
| WI-6 | CODEOWNERS 划分框架与业务的审查边界 | 多团队共用的治理前提 | 小 | — |
| WI-7 | 整理计划文件 `plans/FRAMEWORK_SIMPLIFICATION_PLAN.md` | 工作文档与现状一致 | 小 | 本 RFC |
| WI-8 | 小修：测试未被调度时指明修改位置；`suggestions` 按相似度排序；"快速反馈"一节移入中文文档 | 细节体验 | 小 | WI-2、WI-3 |
| O-1 | 示例配置删除等于默认值的显式字段 | 示例更易读 | 小 | 待决定 |
| O-2 | CONTRIBUTING 直接改为中文（只保留一份） | 语言统一 | 小 | 待决定 |

以上各项都不改外部契约。

## 5. 各项要点

### WI-1 业务源码自动收录

- `src/custom_nodes/`、`src/common_nodes/`（含 `support/`）、`src/adapter/` 下的
  `input/`、`output/`、`biz/`，以及 `demo/biz/`，改用 `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)`。
  源码仍加入原来的 OBJECT 目标，`check_layer_isolation.sh` 的归属检查继续成立。
- 删除脚手架的 `--add-to-cmake`、recipe 中修改 CMakeLists 的步骤，以及相关测试和文档步骤。
- 注册表用 `unordered_map` 存储，转换器与绑定的交叉校验在 Init 审计时进行，因此源文件顺序
  变化不影响行为。
- 取舍：选自动收录，不选"漏登记检查"，因为后者仍要手工登记，只是把静默失败变成报错。
  如果将来内网构建系统不接受 GLOB，再退回"显式列表 + 漏登记检查"，详细设计给出了这个方案。

### WI-2 注册类诊断

- Core：为 `UNKNOWN_NODE_TYPE`、`UNKNOWN_MODEL_TYPE`、`UNKNOWN_BACKEND` 增加 remediation
  （新增 3 个 cause），列出按编辑距离排序的相近已注册名称。文案保持中立，不提具体工具。
- Core：同一根因引起的连带诊断不再报告：
  - model_type 未知时，引用该模型的节点不再报 `UNKNOWN_MODEL_REFERENCE`；
  - 节点类型未知时，只有当某个键除了它之外没有任何已知来源（已知节点或 ingress）时，才不再报该键
    缺少生产者；已知生产者类型不符、生产者不唯一、与 ingress 冲突等独立错误照常报告，并有组合错误测试；
  - 业务出口检查与 IO 边界检查对同一个缺失键只报一次。
- CLI：生产版 `alg_pipeline_tool` 遇到未注册的模型或后端时，向 stderr 输出提示：可选
  Backend 见构建变体；使用测试替身的配置改用 `alg_pipeline_tool_test`。stdout 的 JSON
  结构不变，Studio 和自动化解析不受影响。
- 宿主不受影响：SDK 报错只取第一条诊断的 code、path、message
  （`src/adapter/io_binding_resolver.cpp:198-210`），而根因诊断总是排在连带诊断之前。
- 取舍：
  - 不在 Core 文案里写工具名，因为 Core 校验也被 SDK 使用；
  - 不生成"已知但未编入的 Backend 清单"，因为 Backend 名要在 CMake 里再写一遍，会漂移。

### WI-3 业务接入指南按读者拆分

- 第 4 节（新宿主类型）与 `operator_output_allocation.md` 重复：删除正文，改为一段指引。
- 第 6 节：
  - 保留业务开发者需要的输出容量配置、`-4` 的含义、有效批次查询；
  - 宿主调用与生命周期规则移到 `operator_output_allocation.md` 新增的"宿主调用与生命周期"
    一节，该文档标题改为"Operator 宿主类型、输出池与生命周期"。
- 第 3 节中特殊槽位命名的细节已经写在 `operator_output_allocation.md` 的"选择参数"表里，改为链接。
- 保留第 5 节的锚点（被 Skill 引用）；更新指向第 6 节的 3 处链接。
- 新增 `scripts/check_doc_links.py`，检查 Markdown 相对链接和锚点，并纳入门禁。基线扫描结果：
  53 个 Markdown 文件、59 处跨文件锚点链接，0 处失效。
- 取舍：只拆这一篇，不做全面改写。拆分依据是读者不同和内容重复，不依赖试用数据。

### WI-4 签名编译期提示

- `BatchSpec` 中用 static_assert 检查 `Run` 的可调用形式（与 `InvokeBatch` 实际接受的集合
  一致）以及返回类型 `NodeResult<OutputBatch>`。
- `MakeLlmTextSpec` 检查 `BuildPrompt`、`FormatAnswer` 的签名。
- 错误文案直接列出可接受的签名；签名不合法时跳过函数体的实例化，让 static_assert 成为唯一的报错。
- 新增编译失败契约测试，沿用 `test_layer_header_views.cmake` 的编译清单和 `-fsyntax-only`
  方式。测试同时包含一个必须编译成功的对照文件，防止因无关原因失败而误判通过。
- 取舍：两个 LLM 钩子目前接收返回值的方式不同（`BuildPrompt` 靠构造，`FormatAnswer` 靠赋值），
  统一为一种：接受能隐式转换为 `std::string` 的类型和 `std::string_view`。
  - `FormatAnswer` 返回 `char`、`int` 目前能编译，但结果会被当作单个字符，改为拒绝。这是作者接口的
    收紧，写入 CHANGELOG。
  - `BuildPrompt` 返回 `std::string_view` 由不能编译变为可以，属于放宽。
  - 仓库中现有的钩子都返回 `std::string` 或 `NodeResult<std::string>`，不受影响。

### WI-5 签名速查与教程精简

- `custom_node_concepts.md` 开头增加"三种写法速查"：Map、LLM、Batch 各自的工厂、签名和返回值，
  包括"没有 Parameters 时参数类型是 `NoParameters`"这个易错点；另附"常见编译错误对照"。
- WI-4 的契约测试从 `function_node.h` 的 static_assert 文案中提取签名，检查速查表是否都包含，
  防止两处漂移。
- `first_custom_node.md` 第 3 节末尾那一段改为指向速查表；删除 5 处写死的数量。

### WI-6 CODEOWNERS

- 新增 `.github/CODEOWNERS`，框架路径与业务路径分段列出，目前都由 @chamsechan 审查；
  业务团队接入时，在业务段追加该团队。
- `check_governance.sh` 检查该文件存在。分支保护中的"Require review from Code Owners"是仓库
  设置，由维护者在第二个团队接入时开启。
- 迁入内网平台时，按该平台的 CODEOWNERS 位置约定调整。

### WI-7 整理计划文件

- 文件开头加状态表。
- 删除已完成阶段的过程记录（7.7 实施记录、第 10 节对讨论的修正），以及已被本 RFC 取代的
  阶段 1、2 设计。
- 保留阶段 4 的设计、第 9 节的"不做事项"和附录。

### WI-8 小修

- a：测试未被任何 CTest 过滤器覆盖时，报错补充"在 `tests/RuntimeTests.cmake` 增加过滤器，
  或放进已有套件"。
- b：未知配置字段的顶层 `suggestions` 与 remediation 中的 `candidate_fields` 统一按编辑距离排序。
  随 WI-2 一起实施。
- c：tests/README 中的 "Fast feedback for solution authors" 按读者拆开：
  - Node 部分移入 `src/custom_nodes/README.md`；
  - Adapter、Demo 部分移入业务接入指南第 7 节；
  - tests/README 保留维护者内容，并留一句指引。

  随 WI-3 一起实施。

## 6. 被否决的替代方案

| 方案 | 否决原因 |
| --- | --- |
| 把测试替身链接进生产版 `alg_pipeline_tool` | 生产 Catalog 会列出测试类型；引用它们的配置能通过校验，到 SDK Create 时才失败 |
| 在 Core 诊断里直接写"改用 alg_pipeline_tool_test" | Core 校验也被 SDK 使用，宿主不需要开发工具的提示；工具提示放在 CLI |
| CMake 生成"已知但未编入的 Backend 清单"供诊断使用 | Backend 名要在 CMake 里再写一遍，形成第二份来源；静态提示加已注册列表已经够用 |
| 显式列表 + 漏登记检查，代替自动收录 | 仍要手工登记；保留为内网构建不接受 GLOB 时的退路 |
| 新增一页独立的 Node 速查 | 与 `custom_node_concepts.md` 重复；改为放进归属文档，并用测试防漂移 |
| CONTRIBUTING 维护中英两份 | 两份要同步维护；如果需要中文，按 O-2 直接改写 |
| Adapter 脚手架 | WI-1 之后，复用宿主类型的新契约只需要：biz 文件、必要的转换器、一行 `REGISTER_DEMO_BIZ`、Pipeline 和 `.conf`；另有 Adapter Skill。收益取决于新契约出现的频率，等试用数据 |

## 7. 实施计划

每个阶段独立分支、独立 PR，可以单独回退（还原该 PR 即可）。

- PR 描述写明问题、方案、受影响的契约和验收结果。
- 本地交接时运行 `./scripts/run_all_tests.sh`。
- 只有在明确授权后，才通过 `scripts/git_branch_upload.sh` 推送远端。

| 顺序 | 分支 | 内容 | 依赖 |
| --- | --- | --- | --- |
| 0 | `docs/developer-experience-rfc` | 本 RFC、详细设计、WI-7 | — |
| 1 | `chore/auto-collect-sources` | WI-1 | 0 |
| 2 | `feat/registration-diagnostics` | WI-2、WI-8b | 0 |
| 3 | `refactor/spec-signature-diagnostics` | WI-4、WI-5、WI-8a | 0 |
| 4 | `docs/onboarding-audience-split` | WI-3（含链接检查）、WI-8c | 1 |
| 5 | `chore/codeowners` | WI-6 | 0 |
| — | 视决定而定 | O-1、O-2 | 4 |

阶段 1、2、3、5 互不依赖，可以并行。阶段 4 等阶段 1 合入后再做，避免两次修改同一段文档。
全部合入后，删除本 RFC 和详细设计。

## 8. 验收

### 8.1 外部契约未变（每个 PR 都要提供）

- `SdkExportSurfaceTest`，以及 Operator ABI、安全、SDK 消费者测试全部通过。
- `alg_pipeline_tool catalog` 按 ID 排序后与基线完全一致。
- smoke 套件中每个 Profile 都有完整结果（结果文件齐全、样本数一致），逐条结果（去掉耗时字段后）
  与基线一致；两次采集都使用重新构建后的程序。方法见详细设计 0.1。
- 没有修改 `configs/`、`demo/fixtures/` 下的 `.conf` 和 Pipeline JSON（O-1 除外，O-1 自带等价性证明）。

### 8.2 各项验收

见详细设计中各节的"验收"清单。

## 9. 风险

| 风险 | 缓解 |
| --- | --- |
| 自动收录会把业务目录里的草稿 `.cpp` 编译进去 | 文档写明"目录下所有 `.cpp` 都会编入"；草稿放在仓库外，或不用 `.cpp` 后缀 |
| GLOB 带来重新配置的开销 | CONFIGURE_DEPENDS 每次构建时检查目录；业务目录文件少，开销可以忽略；测试目录已经这样做 |
| 诊断变化影响依赖完整诊断列表的测试或工具 | 只去掉连带诊断，根因诊断不变；Studio 只渲染 summary 和 suggestions；实施时全量运行 Studio 测试 |
| 连带诊断抑制过宽，隐藏了独立错误 | 抑制条件限定为"完全由未注册项造成"（详细设计 2.3），并用组合错误测试验证独立错误仍被报告 |
| 基线比较空跑，或用旧程序采集，造成假通过 | 采集脚本先重新构建，任何命令失败即中止；比较脚本以 catalog 中的 smoke Profile 为准检查结果完整性，缺失即失败。链接检查和签名契约测试在"什么都没检查"时同样判失败 |
| 移动文档造成链接失效 | 先合入链接检查，再移动内容 |
| static_assert 误拒现有可编译的代码 | 可接受的形式与 `InvokeBatch` 一致；全部生产 Node、starter 和脚手架夹具都在门禁中编译 |
| CODEOWNERS 只有一位审查人，审查人不在时会阻塞合并 | 暂不开启强制审查；第二个团队接入时再开启，并补充审查人 |

## 10. 待决事项

| 事项 | 选项 | 建议 |
| --- | --- | --- |
| O-1：示例配置删除等于默认值的字段 | 删除 / 保留 | 先出清单，由你标出"有意固定"的字段，其余删除 |
| O-2：CONTRIBUTING 直接改为中文 | 改 / 不改 | 如果业务开发者需要直接阅读流程规范，就改 |
| rerank 候选段落上限（转换器 64 KiB，Operator 10 MiB，来自计划第 12 节） | 维持 / 调整 | 与本 RFC 无关；整理计划文件时移到这里，以免遗漏 |
| 何时开启"Require review from Code Owners" | 现在 / 第二个团队接入时 | 第二个团队接入时 |
| 真实开发者试用 | 安排 / 暂缓 | 安排，见第 11 节 |

## 11. 试用验证（建议与实施同步进行）

按 [VERIFIABLE_SELECTION.md](../doc/VERIFIABLE_SELECTION.md#验收范围与发布准备) 中已有的方法，
请 2–3 位没有参与框架开发、能读写简单 C++ 的开发者，在 WI-1 至 WI-5 合入前后各做一次：

| 任务 | 内容 | 完成标准 |
| --- | --- | --- |
| T1 编排 | 修改关键词规则并运行 Demo | 结果符合预期 |
| T2 写 Node | 新增一个文本 LLM Node，接入现有方案 | Demo 结果正确，节点测试通过 |
| T3 新契约 | 复用文本输入转换器，新增一种输出协议 | 契约测试和 Demo 通过 |

需要记录：代码与环境基线、完成耗时、求助次数与原因、修改的文件数、遇到的报错原文。
用这些结果决定 Adapter 脚手架、Batch 预设和后续的文档调整。
