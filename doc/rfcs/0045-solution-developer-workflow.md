# RFC 0045: 方案开发者日常工作路径收敛

- **RFC 编号**：0045-solution-developer-workflow
- **创建日期**：2026-09-08
- **文档状态**：Completed
- **关联分支**：`fix/solution-developer-workflow`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow contributors
- **关联决策**：补充 RFC-0039、0041、0043、0044；调整 RFC-0041 的 Control 寻址能力

## 1. 问题与范围

方案开发者在编排、编写 custom Node、转换平台结构及调参时，需要能验证自己修改的
方案。审查发现 OCR Demo 忽略显式 Control、默认示例更新覆盖方案配置、Control schema
声明与执行不一致，以及保存方案、错误定位和参数说明的断点。本次按独立问题提交，
复用既有 Catalog、Validator、Resolver、Node 支持类和测试 runner。

范围包括可复现缺陷、高频工具操作和与实现配套的作者指南。保留四层依赖、typed ports、
请求来源、静态注册和 Adapter/Operator 注册完整性；不接入公司内部 SDK。

## 2. 决策与权衡

- **Integration / Demo**：单槽与 OCR 路径共用显式 Control 的读取、校验和执行。
  普通 Demo 默认使用方案配置；内置热更新改为显式 `--example-control`，保留
  `--no-default-control` 的关闭含义。显式文件仍按用户选择执行。补齐可达的结果状态
  丢失、注册诊断和公开内存租约说明，不改变输出池所有权或等待策略。
- **Orchestration / Control**：保持现有 C ABI 和 Operator 参数结构。节点命令 ID 仍由
  原调用参数指定；JSON 可显式包装为
  `{"$edgeflow_control":1,"node_id":"rules_a","payload":{...}}`，只更新该实例。
  包装必须是这三个字段，版本为整数 1，实例名非空，payload 为 object。普通 JSON
  保持广播。保留广播的既有部分成功语义，不引入多节点事务或模型热加载。
  `$edgeflow_control` 为传输保留字段；Node 只收到解包后的业务 payload。
  未知版本、目标、字段或不支持的命令在执行更新前失败。
- **Contracts / Capability Nodes**：Control 的基础 schema 校验执行公开声明的数值范围，
  对不支持或无效声明明确失败。参数说明由现有 Definition 的 semantic 字段输出；
  复用字段规范化和语义解析，补齐普通 Node 的正确作者样例和高频错误原因。
  复杂算法继续以普通 C++ 实现，按实际重复成本补充小型样例和测试辅助。
- **Tooling**：CLI init 增加显式原始 Pipeline 输出，保留默认响应格式。
  Studio 展示原生诊断和字段说明；复用既有草稿配置生成逻辑，提供保存可运行方案的
  JSON、部署 conf 和准确命令。所有配置检查继续调用原生 Validator/Resolver。
  显示可获得的部署解析结果，不另建一套路径或参数优先级实现。
- **Docs / Skills**：按新增参数、Control、复杂算法、协议转换和模型替换补齐连续任务。
  复用已存在的 minimal 构建与可编译样例。常规 custom Node 复用既有端口类型、模型能力
  和公共契约时，用 Definition、变更说明和针对性测试记录；新增公共能力、类型、
  兼容性或跨层/生命周期决策仍按 CONTRIBUTING 进入 RFC。

不建设通用协议 DSL、深层 Node 继承体系、动态插件、新项目文件格式或默认全量运行追踪。
没有具体使用场景证明收益的抽象留待实际作者试用。本次不以模拟运行替代模型效果、
性能或目标设备验收。

## 3. 兼容与迁移

已有普通 Control JSON 继续广播；需要独立调参的调用方主动使用版本化包装。旧版框架
不支持该包装，调用方须与本次 SDK 同步。Demo 依赖内置演示规则或提示词的命令须显式
添加 `--example-control`；已有 `--control-file` 不受这一默认值变化影响。
CLI 默认响应和已有 Pipeline/conf 格式保持不变。新增 schema 检查可能拒绝此前被忽略
的声明，作者应使用支持的基础规则，并把业务语义留在 Node 解析函数中。

## 4. 验证与完成条件

- Demo：OCR 显式 Control 的缺失文件、非法参数和执行失败能够被观察；默认配置与
  显式示例更新分别验证；受影响结果按请求 ID 和状态检查。
- Control：基础范围边界、非法 schema、两个同类实例的定向更新，以及错误目标或
  payload 不改变已生效值；覆盖已有广播与公共传输路径。
- Tooling：默认与 raw init 输出、诊断展示、保存文件指向和冲突保护、所选参数在运行
  产物中保持一致；使用现有 Python/JS 和集成套件。
- Node / Integration：保留来源和错误传播，注册诊断可定位；新增作者片段参与现有编译
  或行为测试，文档链接现行实现。
- 完成所有独立问题后执行 canonical gate，再按授权 PR 合并并验证该 merge SHA 的 main CI。

## 5. 实施与最终结果

实现按独立问题提交。完成 Control 执行与定向路由、schema 校验、配置说明、原生部署
解析、可运行方案保存及后续同步、Node/Bridge 诊断、Demo 状态和作者文档/skill 的修复。
保存同步仅接管本次服务会话创建的文件；服务重启后对已有模型路径覆盖明确提示，避免
擅自改写原 Profile 的部署文件。

2026-09-08 验证结果：

- `./scripts/run_all_tests.sh` 完整默认构建与 **89/89 CTest 检查通过**。
- 最小构建完成 SDK、Demo、Pipeline CLI、终端查看器；原生校验与部署解析通过，
  四条入门样本的状态、命中结果与预期一致。
- Studio Python **33 项通过**，现有两套 JS 检查通过；覆盖模型重命名/换路径后同步保存、
  双文件修改冲突、失败回滚及服务重启后的覆盖保护。
- 针对性 C++ 验证覆盖规则校验、按实例 Control、Init/Process 原因、Bridge 完整性、
  混合样本状态与实际生成的 Control 作者模板；当前生产 Catalog 88 个配置字段均有说明。
- 两份修改的 skill 通过校验，变更文档的本地文件链接检查通过。

远程交付按用户授权执行仓库 PR/merge 脚本，PR 与 main 的验证状态以 GitHub 为准。
真实模型业务效果、目标硬件与开发者本人试用继续在
[既有验收计划](../plans/solution_developer_acceptance.md)记录。
