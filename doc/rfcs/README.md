# RFC 架构与接口决策库

本目录保存需要长期保留的架构与接口决策。首次开发从[任务导航](../README.md#按任务开始)
进入；查阅历史 RFC 是为了理解决策背景，无需按编号通读。

## 阅读顺序

- 查当前职责与接口：[架构设计](../architecture.md)和[开发者扩展指南](../developer_guide.md)。
- 跟进尚未完成的设计或迁移：见下方进行中的 RFC。
- 追溯已交付决策：从已完成索引进入正文；验证证据先看[评审归档入口](reviews/README.md)。
- 查日期型审计与整改：[历史报告归档](../archive/README.md)。

历史正文中的“当前”、阶段待办、代码路径与测试数量均对应当时基线。
`Completed` 表示该 RFC 范围的实施与验证完成，不表示所有细节仍适用于最新版本，也不表示决策失效。
索引使用当前职责名称；历史标题和正文保留决策时的术语。决策被后续 RFC 修改时，
应明确取代范围，不能仅按编号或完成日期判断适用性。

## 进行中的 RFC

| 编号 | 标题 | 状态 | 目标版本 | 涉及职责 | 链接 |
| :--- | :--- | :---: | :---: | :--- | :--- |
| **RFC-0029** | 外网架构收口与内网 SDK 迁移分阶段整改 | `In Implementation` | `v10.x / 待定` | 接入适配层、流程编排层、能力节点层、模型执行层 / Tooling | [0029-external-readiness-and-intranet-sdk-migration.md](0029-external-readiness-and-intranet-sdk-migration.md) |
| **RFC-0036** | Whisper ASR 与 whisper.cpp Backend 接入设计及实施指南 | `In Implementation` | `v10.x` | 模型执行层 / Config / Demo / Build | [0036-whisper-asr-backend.md](0036-whisper-asr-backend.md) |

## 已完成的 RFC

| 编号 | 标题 | 状态 | 目标版本 | 涉及职责 | 链接 |
| :--- | :--- | :---: | :---: | :--- | :--- |
| **RFC-0001** | 4 层架构隔离与统一分层抽象基线 | `Completed` | `v1.0.0` | 接入适配层、流程编排层、能力节点层、模型执行层 | [0001-four-tier-architecture-foundation.md](0001-four-tier-architecture-foundation.md) |
| **RFC-0002** | C ABI Adapter 契约安全与内存防越界加固 | `Completed` | `v1.1.0` | 接入适配层 (C ABI Adapter) | [0002-c-abi-adapter-security-hardening.md](0002-c-abi-adapter-security-hardening.md) |
| **RFC-0003** | Pipeline 严格解析、Fail-Closed 注册与结构化诊断 | `Completed` | `v1.2.0` | 流程编排层 (Pipeline & Blackboard) | [0003-pipeline-dynamic-blackboard-rebaseline.md](0003-pipeline-dynamic-blackboard-rebaseline.md) |
| **RFC-0004** | 平台 Operator 接口与命名 I/O 兼容层设计 | `Completed` | `v1.3.0` | 接入适配层 (Platform Operator) | [0004-platform-operator-interface-compatibility.md](0004-platform-operator-interface-compatibility.md) |
| **RFC-0005** | 参数化业务 Demo Runner 与执行配置解耦 | `Completed` | `v1.4.0` | Demo / Integration Tooling | [0005-parameterized-business-demo-runner.md](0005-parameterized-business-demo-runner.md) |
| **RFC-0006** | 图形化算法方案工作台与 Catalog/Validator 单一事实源 | `Completed` | `v1.5.0` | 接入适配层、流程编排层、能力节点层、模型执行层 / Tooling | [0006-visual-pipeline-studio.md](0006-visual-pipeline-studio.md) |
| **RFC-0007** | 全库 Pipeline 配置文件显式 DAG 标准化与旧式配置维护解耦 | `Completed` | `v1.6.0` | 流程编排层、能力节点层 / Tooling | [0007-explicit-dag-standardization-and-legacy-deprecation.md](0007-explicit-dag-standardization-and-legacy-deprecation.md) |
| **RFC-0008** | 架构契约收敛与文档一致性修复 | `Completed` | `v2.1.0` | 接入适配层、流程编排层、能力节点层、模型执行层 / Tooling | [0008-architecture-contract-consolidation.md](0008-architecture-contract-consolidation.md) |
| **RFC-0009** | 公司平台 C 结构体槽位绑定与输出内存池 | `Completed` | `v3.0.0` | 接入适配层 (Platform Operator) / Demo | [0009-company-string-and-slot-map-struct-binding.md](0009-company-string-and-slot-map-struct-binding.md) |
| **RFC-0010** | 全栈统一业务命名为 biz | `Completed` | `v3.1.0` | 接入适配层、流程编排层、能力节点层、模型执行层 / Tooling | [0010-business-to-biz-naming-unification.md](0010-business-to-biz-naming-unification.md) |
| **RFC-0011** | Operator 与计算 Platform 命名解耦 | `Completed` | `v4.0.0` | 接入适配层 (Operator Adapter) / Demo / 模型执行层 Terminology | [0011-operator-platform-naming-unification.md](0011-operator-platform-naming-unification.md) |
| **RFC-0012** | I/O 契约驱动的通用 Node 架构 | `Completed` | `v4.1.0` | 接入适配层、流程编排层、能力节点层、模型执行层 / Tooling | [0012-node-authoring-experience.md](0012-node-authoring-experience.md) |
| **RFC-0013** | 开发反馈闭环加速 | `Completed` | `v4.2.0` | Tooling / Test Infrastructure | [0013-developer-feedback-loop-acceleration.md](0013-developer-feedback-loop-acceleration.md) |
| **RFC-0014** | 独立公共日志 API 与核心日志统一 | `Completed` | `v4.3.0` | Cross-Cutting / 接入适配层、流程编排层、能力节点层、模型执行层 | [0014-public-log-api.md](0014-public-log-api.md) |
| **RFC-0015** | 模型能力与推理运行时解耦实施规范 | `Completed` | `v5.0.0` | 流程编排层、能力节点层、模型执行层 | [0015-model-capability-backend-decoupling.md](0015-model-capability-backend-decoupling.md) |
| **RFC-0016** | 构建与测试工作流收敛 | `Completed` | `v5.1.0` | Tooling / Test Infrastructure | [0016-build-and-test-workflow-convergence.md](0016-build-and-test-workflow-convergence.md) |
| **RFC-0017** | 开发治理与 Agent 工作流收敛 | `Completed` | `v5.2.0` | Tooling / Governance | [0017-development-governance-convergence.md](0017-development-governance-convergence.md) |
| **RFC-0018** | 请求黑板与算法句柄并发契约收敛 | `Completed` | `v5.3.0` | 接入适配层、流程编排层 | [0018-request-context-and-handle-concurrency-contracts.md](0018-request-context-and-handle-concurrency-contracts.md) |
| **RFC-0019** | 高优先级分层代码收敛 | `Completed` | `v5.4.0` | 接入适配层、流程编排层、能力节点层、模型执行层 / Tooling | [0019-high-priority-layer-convergence.md](0019-high-priority-layer-convergence.md) |
| **RFC-0020** | Layer 2 运行时一致性与事务化构建收敛 | `Completed` | `v5.5.0` | 流程编排层 / 能力节点层 Definition | [0020-layer2-runtime-convergence.md](0020-layer2-runtime-convergence.md) |
| **RFC-0021** | Layer 4 作者体验与执行协议收敛 | `Completed` | `v5.6.0` | 模型执行层 | [0021-layer4-authoring-and-protocol-convergence.md](0021-layer4-authoring-and-protocol-convergence.md) |
| **RFC-0022** | 文本规则与 UTF-8 分块安全收敛 | `Completed` | `v5.7.0` | 能力节点层 / 模型执行层 Text Support | [0022-text-processing-safety.md](0022-text-processing-safety.md) |
| **RFC-0023** | v6 运行时契约破坏性收敛 | `Completed` | `v6.0.0` | 接入适配层、流程编排层、能力节点层 / Tooling | [0023-v6-contract-convergence.md](0023-v6-contract-convergence.md) |
| **RFC-0024** | 正式接入前历史兼容契约清理 | `Completed` | `v7.0.0` | 接入适配层、流程编排层、能力节点层 / Tooling | [0024-pre-release-contract-cleanup.md](0024-pre-release-contract-cleanup.md) |
| **RFC-0025** | 部署路径、执行目标与可复现验收契约收敛 | `Completed` | `v8.0.0` | 接入适配层、流程编排层、能力节点层、模型执行层 / Tooling | [0025-deployment-runtime-contract-convergence.md](0025-deployment-runtime-contract-convergence.md) |
| **RFC-0026** | 统一 LLM 文本生成协议与多 Backend 实现 | `Completed` | `v9.0.0` | 能力节点层、模型执行层 | [0026-unified-llm-generation-backends.md](0026-unified-llm-generation-backends.md) |
| **RFC-0027** | 正式接入前源码布局与 C++ 命名空间收敛 | `Completed` | `v10.0.0` | 接入适配层、流程编排层、能力节点层、模型执行层 / Tooling | [0027-preproduction-source-layout-and-namespace-convergence.md](0027-preproduction-source-layout-and-namespace-convergence.md) |
| **RFC-0028** | v10.0.0 预发布运行时与 ABI 收口 | `Completed` | `v10.0.0` | 接入适配层、流程编排层、能力节点层、模型执行层 / Tooling | [0028-preproduction-runtime-and-abi-hardening.md](0028-preproduction-runtime-and-abi-hardening.md) |
| **RFC-0030** | 编译期分层边界与轻量运行时计划契约 | `Completed` | `v10.x` | 接入适配层、流程编排层、能力节点层、模型执行层 / Build | [0030-compile-time-layer-boundaries.md](0030-compile-time-layer-boundaries.md) |
| **RFC-0031** | 业务 Blackboard Key 所有权拆分 | `Completed` | `v10.x` | 接入适配层、流程编排层、能力节点层 | [0031-business-blackboard-key-ownership.md](0031-business-blackboard-key-ownership.md) |
| **RFC-0032** | 从 GitHub 发布包直接接入 kiteLLM | `Completed` | `v10.x` | 模型执行层 / Build | [0032-kitellm-direct-github-dependency.md](0032-kitellm-direct-github-dependency.md) |
| **RFC-0033** | 按 kiteLLM 原生接口传递设备选择 | `Completed` | `v10.x` | 模型执行层 / Build | [0033-kitellm-native-device-contract.md](0033-kitellm-native-device-contract.md) |
| **RFC-0034** | Kite 原生能力在现有业务中的完整接入 | `Completed` | `v10.x` | 模型执行层 / Config / Build | [0034-kitellm-capability-coverage.md](0034-kitellm-capability-coverage.md) |
| **RFC-0035** | Kite 生成 token 向量与中性 Embedding 接入 | `Completed` | `v10.x` | 模型执行层 / Config / Build | [0035-generated-token-embedding.md](0035-generated-token-embedding.md) |
| **RFC-0037** | 审计问题的五阶段最小整改 | `Completed` | `v10.x` | 接入适配层、流程编排层、能力节点层 / Tooling | [0037-audit-remediation.md](0037-audit-remediation.md) |
| **RFC-0038** | 自定义 Node 的统一源码扩展目录 | `Completed` | `v10.x` | 能力节点层 / Build / Governance | [0038-custom-node-extension-directory.md](0038-custom-node-extension-directory.md) |
| **RFC-0039** | 自定义节点开发路径与失败契约修复 | `Completed` | `v10.x` | 接入适配层、流程编排层、能力节点层 / Tooling / Demo | [0039-custom-node-authoring-closure.md](0039-custom-node-authoring-closure.md) |
| **RFC-0040** | 上线前实现、验证与命名收敛 | `Completed` | `v10.x` | 四层 / Build / Tooling | [0040-prelaunch-audit-convergence.md](0040-prelaunch-audit-convergence.md) |
| **RFC-0041** | Control 开发路径的最小收敛 | `Completed` | `v10.x` | 接入适配层、流程编排层、能力节点层 / Tooling / Demo | [0041-control-authoring.md](0041-control-authoring.md) |
| **RFC-0042** | 文件浏览、模板语义与构建边界修复 | `Completed` | `v10.x` | 四层 / Build / Tooling | [0042-studio-and-contract-boundaries.md](0042-studio-and-contract-boundaries.md) |
| **RFC-0043** | 参数配置与执行行为的最小一致性整改 | `Completed` | `v10.x` | 流程编排层、能力节点层、模型执行层 / Tooling | [0043-config-parameter-consistency.md](0043-config-parameter-consistency.md) |
| **RFC-0044** | 投产前跨实现契约一致性整改与实施指南 | `Completed` | `v10.x` | 接入适配层、流程编排层、能力节点层、模型执行层 | [0044-preproduction-contract-consistency.md](0044-preproduction-contract-consistency.md) |
| **RFC-0045** | 方案开发者日常工作路径收敛 | `Completed` | `v10.x` | 四层 / Tooling / Docs | [0045-solution-developer-workflow.md](0045-solution-developer-workflow.md) |
| **RFC-0046** | 接入目录、头文件边界与契约命名整理 | `Completed` | `v10.x` | 四层 / Tooling / Docs | [0046-naming-and-header-boundaries.md](0046-naming-and-header-boundaries.md) |
| **RFC-0047** | 平台公共模拟声明隔离 | `Completed` | `v10.x` | 接入适配层 / Demo / Build | [0047-platform-mock-header-isolation.md](0047-platform-mock-header-isolation.md) |

## 专项验收与评审归档

[评审归档](reviews/README.md)按 RFC 聚合结论、验收证据和阶段材料。
例如 RFC-0015 可先读决策摘要与阶段 7 最终验收，需要追溯某次迁移时再展开阶段记录。
历史报告中的 FAIL、未勾选任务或旧生命周期说明保留原始上下文，不作为新任务直接执行。

## RFC 生命周期状态

| 状态 | 说明 |
| --- | --- |
| `Draft` | 草案阶段，需求和方案仍在探索。 |
| `Proposed` | 方案已成型，等待设计决策。 |
| `In Implementation` | 方案已采用，代码、测试或迁移正在实施。 |
| `Completed` | 范围内实现和要求的验证已完成，同一提交集具备合入条件；Git 合入状态不在文档中重复维护。 |
| `Deprecated` / `Rejected` | 方案已废弃或被否决；注明原因及适用的后续决策。 |

## 编写与维护

是否需要 RFC，以及分支、实施和交付要求，统一遵循
[CONTRIBUTING.md](../../CONTRIBUTING.md#3-design-only-when-the-decision-needs-a-durable-record)。
局部修复、测试补强、文档修正、机械重构和复用已有能力的方案配置通常不需要 RFC。

编号正文统一保留在 `doc/rfcs/NNNN-<kebab-case-title>.md`，使用四位自增编号，
例如 `0041-control-authoring.md`。从 [RFC_TEMPLATE.md](RFC_TEMPLATE.md) 开始，
删除不适用小节，按决策复杂度说明问题、方案、权衡和验收。

- 每篇正文头部维护标准状态，索引中保留一行对应记录；状态变化时同步移动到相应分组。
- 完成时优先在原 RFC 补充最终结果与现行指南链接，阶段进度直接更新原记录。
- 独立评审确有额外证据时放入 `reviews/` 并更新其索引，不为普通进度另建接续指南。
- 已完成 RFC 保留正文与编号路径；历史报告保留独有决策、失败证据、验收基线和验证限制。
- 日期型审计、整改记录和已完成计划归入 `doc/archive/`；待完成的普通规划放在 `doc/plans/`。
