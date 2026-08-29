# RFC 0017: 开发治理与 Agent 工作流收敛

- **RFC 编号**：0017-development-governance-convergence
- **创建日期**：2026-08-29
- **文档状态**：Completed
- **关联分支**：`docs/governance-workflow-audit`
- **目标版本**：v5.2.0
- **负责人 / 作者**：LLM-EdgeFlow Maintainers

## 1. 背景与动机

仓库的架构、RFC、测试和 Git 规则分别散落在 `AGENTS.md`、`CLAUDE.md`、
Copilot 指令、三个项目技能、开发者指南和脚本中。多处内容已经与 v5.1.0 实现漂移：

- Layer 4 仍被描述为已移除的 `IModelEngine` / `EngineDefinition` /
  `REGISTER_ENGINE_WITH_DEFINITION`，而当前实现是 Model / Backend 双注册；
- Layer 3 仍引用不存在的 `src/business/` 或已清空的 `src/biz/`；
- RFC 与 Agent 文档重复要求 `ctest`、格式化和 `run_all_tests.sh`，但 RFC-0016 已将
  这些检查收敛进唯一完整门禁；
- GitHub 技能要求更新 README Changelog，顶层规则却要求更新 `CHANGELOG.md`；
- 上传脚本默认自动合并，并在 PR/CLI 不可用时回退为直接推送 `main`，授权边界过宽；
- RFC 对所有“新需求”一刀切，令配置组合、局部修复和小型重构承担不必要的设计成本。

目标是在不降低架构、安全和回归强度的前提下，消除重复维护、矛盾门禁与隐式外部写入，
让日常开发只执行一次必要流程。

## 2. 范围与非目标

### 2.1 范围内

- 明确架构、开发流程、RFC、质量门禁和交付脚本的唯一事实源；
- 校准 Agent/Claude/Copilot 指令和三个项目技能；
- 以风险阈值决定 RFC 与验证范围；
- 修正 Layer 3、Model / Backend、显式 DAG 等当前架构描述；
- 将 GitHub 交付改为默认只创建 PR，合并必须显式授权；
- 增加治理文档防漂移检查。

### 2.2 非目标

- 不改变 C ABI、Pipeline、Node、Model 或 Backend 运行时行为；
- 不改变统一完整质量门禁覆盖的构建配置与测试集合；
- 不重写历史 RFC；历史文档保留当时设计语境；
- 不在本 RFC 中上传分支、创建 PR 或合并 `main`。

## 3. 事实源与流程设计

### 3.1 治理事实源

| 事实 | 唯一维护位置 | 其他文档的责任 |
| :--- | :--- | :--- |
| 当前四层架构红线、任务路由 | `AGENTS.md` | 链接，不复制规则 |
| 人类与 Agent 的开发生命周期 | `CONTRIBUTING.md` | 链接，不复制命令矩阵 |
| RFC 触发条件、状态与索引 | `doc/rfcs/README.md` | RFC 模板引用该定义 |
| 完整本地质量门禁 | `scripts/run_all_tests.sh` | 只调用，不拆开重复执行 |
| GitHub 交付机械流程 | `scripts/git_branch_upload.sh` | Skill 负责授权和前置判断 |
| Node/Model/Backend 可用能力 | 运行时 Catalog / Definition | 文档和 Skill 不维护静态清单 |

### 3.2 风险分级流程

```text
任务分类
  ├─ 只读审查：不建分支、不写文件
  ├─ Pipeline 配置组合：pipeline-composer，复用 Catalog/Validator
  └─ 代码/文档变更：先建分支
       ├─ 命中 RFC 阈值：RFC → 实现与测试
       └─ 未命中：直接实现与针对性测试
              ↓
        交付前仅运行一次 canonical full gate
              ↓
        用户明确要求上传：创建 PR（默认不合并）
              ↓
        用户明确要求合并：等待 CI 后合并
```

RFC 只用于公共 ABI/契约、跨层架构、兼容性或迁移策略、新依赖/Backend/Model/Node
能力，以及高风险安全、并发、所有权或性能决策。局部 Bug、测试、文档、无行为重构和使用
现有节点的 Pipeline 配置不强制 RFC。

### 3.3 四层映射

- **Layer 1**：`IBizAdapter`、Operator bridge、C ABI 与异常屏障；
- **Layer 2**：Catalog、Validator、显式 DAG、Pipeline、Blackboard 与 Session；
- **Layer 3**：当前生产实现为 `src/common_nodes/` 中按 I/O 契约组织的无请求状态节点；
- **Layer 4**：Model 语义、Backend 中性协议/厂商运行时与 `FixedBatchExecutor`；
- **Tooling / Governance**：本 RFC 的实际修改归属。

## 4. 设计不变量

1. 同一规范只在一个事实源维护，其余位置使用链接或简短路由。
2. `./scripts/run_all_tests.sh` 是唯一交付前完整本地门禁；针对性测试只用于开发反馈。
3. 验证强度按变更风险增加，但不得以重复执行同一测试伪装更高质量。
4. 任何外部上传与合并均需用户明确授权；没有 GitHub CLI 时不得直接推送 `main` 回退。
5. PR 是默认交付终点；自动合并必须使用显式 `--merge`。
6. 历史 RFC 不作为当前架构事实源；当前文档、代码注册与运行时 Catalog 优先。
7. Agent 入口不得复制会随架构或测试数量变化的清单。

## 5. 测试与验收计划

- [x] 三个项目技能通过 `skill-creator` 的 `quick_validate.py`；
- [x] Shell 脚本通过 `bash -n`；
- [x] 架构文档漂移门禁覆盖 Agent/Skill/治理入口与已废弃标识；
- [x] 新增 `GovernanceConsistencyTest`，阻止重复门禁和危险 Git 回退；
- [x] PlantUML 资产重新生成并通过 render gate；
- [x] `git diff --check` 通过；
- [x] `./scripts/run_all_tests.sh` 完整通过（85/85 CTest，111 秒）。

## 6. 实施里程碑

1. [x] 建立独立分支并完成代码、文档、技能和脚本事实盘点。
2. [x] 新增统一开发流程并精简 Agent 入口。
3. [x] 校准三个项目技能及四层参考。
4. [x] 修正 RFC、开发者指南、架构图与交付脚本漂移。
5. [x] 完成静态检查、技能校验和统一完整质量门禁。
6. [x] 将 RFC 与索引更新为 `Completed`。

## 7. 变更记录

| 日期 | 版本 | 变更 |
| :--- | :--- | :--- |
| 2026-08-29 | v5.2.0 | 创建治理与 Agent 工作流收敛 RFC |
| 2026-08-29 | v5.2.0 | 完成治理收敛及 85/85 CTest 验收 |
