# LLM-EdgeFlow RFC 需求与技术设计文档库 (RFC Directory)

本目录是需要长期保留的架构与接口决策及其验收记录。它不是所有需求、Bug 或任务的
流水账；共享开发生命周期由 [`CONTRIBUTING.md`](../../CONTRIBUTING.md) 定义。

---

## 1. RFC 触发条件与生命周期

以下变更必须先建立 RFC：

- 公共 C ABI、Operator 契约、持久 Pipeline Schema 或兼容行为；
- 跨层责任、依赖方向或需要多层协同的架构机制；
- 新的 Node、Model capability、Backend、模态或重大工具链；
- 难以回退的所有权、生命周期、并发、安全或性能决策；
- 需要下游协同的迁移与废弃。

局部 Bug、测试补强、文档修正、无行为机械重构，以及复用现有节点的 Pipeline 配置通常
不需要 RFC。先在独立分支创建并索引 RFC，再实现；交付前只运行
`./scripts/run_all_tests.sh` 这一完整本地门禁，避免重复执行其内部的 format/CTest 步骤。

---

## 2. RFC 生命周期状态

每个 RFC 文档头部必须包含标准的 Metadata 状态标识：

| 状态 (Status) | 说明 |
| :--- | :--- |
| **`Draft`** | 草案阶段：需求初步提出，方案正在探索与讨论中。 |
| **`Proposed`** | 方案已成型，等待设计决策。 |
| **`In Implementation`** | 方案已采用，代码、测试或迁移正在实施。 |
| **`Completed`** | RFC 范围已实现，要求的验证已通过，同一提交集已具备合入条件。Git 合入状态不在文档中重复维护。 |
| **`Deprecated`** / **`Rejected`** | 已废弃 / 已否决：方案被后续 RFC 取代或评审未通过。 |

---

## 3. RFC 文件命名规范

RFC 文档统一存放在 `doc/rfcs/` 根目录下，采用 **四位自增编号 + 英文小写破折号** 命名：

```text
doc/rfcs/NNNN-<kebab-case-title>.md
```

**示例**：
- `doc/rfcs/0001-four-tier-architecture-foundation.md`
- `doc/rfcs/0004-platform-operator-interface-compatibility.md`
- `doc/rfcs/0005-audio-stream-vsl-support.md`

使用 [`RFC_TEMPLATE.md`](RFC_TEMPLATE.md)；删除不适用章节，不要为满足模板而制造空洞内容。

---

## 4. RFC 索引

| 编号 | 标题 | 状态 | 目标版本 | 核心涉及层级 | 链接 |
| :--- | :--- | :---: | :---: | :--- | :--- |
| **RFC-0001** | 4 层架构隔离与统一分层抽象基线 | `Completed` | `v1.0.0` | Layer 1 ~ Layer 4 | [0001-four-tier-architecture-foundation.md](0001-four-tier-architecture-foundation.md) |
| **RFC-0002** | C ABI Adapter 契约安全与内存防越界加固 | `Completed` | `v1.1.0` | Layer 1 (C ABI Adapter) | [0002-c-abi-adapter-security-hardening.md](0002-c-abi-adapter-security-hardening.md) |
| **RFC-0003** | Pipeline 严格解析、Fail-Closed 注册与结构化诊断 | `Completed` | `v1.2.0` | Layer 2 (Pipeline & Blackboard) | [0003-pipeline-dynamic-blackboard-rebaseline.md](0003-pipeline-dynamic-blackboard-rebaseline.md) |
| **RFC-0004** | 平台 Operator 接口与命名 I/O 兼容层设计 | `Completed` | `v1.3.0` | Layer 1 (Platform Operator) | [0004-platform-operator-interface-compatibility.md](0004-platform-operator-interface-compatibility.md) |
| **RFC-0005** | 参数化业务 Demo Runner 与执行配置解耦 | `Completed` | `v1.4.0` | Demo / Integration Tooling | [0005-parameterized-business-demo-runner.md](0005-parameterized-business-demo-runner.md) |
| **RFC-0006** | 图形化算法方案工作台与 Catalog/Validator 单一事实源 | `Completed` | `v1.5.0` | Layer 1 ~ Layer 4 / Tooling | [0006-visual-pipeline-studio.md](0006-visual-pipeline-studio.md) |
| **RFC-0007** | 全库 Pipeline 配置文件显式 DAG 标准化与旧式配置维护解耦 | `Completed` | `v1.6.0` | Layer 2 ~ Layer 3 / Tooling | [0007-explicit-dag-standardization-and-legacy-deprecation.md](0007-explicit-dag-standardization-and-legacy-deprecation.md) |
| **RFC-0008** | 架构契约收敛与文档一致性修复 | `Completed` | `v2.1.0` | Layer 1 ~ Layer 4 / Tooling | [0008-architecture-contract-consolidation.md](0008-architecture-contract-consolidation.md) |
| **RFC-0009** | 公司平台 C 结构体槽位绑定与输出内存池 | `Completed` | `v3.0.0` | Layer 1 (Platform Operator) / Demo | [0009-company-string-and-slot-map-struct-binding.md](0009-company-string-and-slot-map-struct-binding.md) |
| **RFC-0010** | 全栈统一业务命名为 biz | `Completed` | `v3.1.0` | Layer 1 ~ Layer 4 / Tooling | [0010-business-to-biz-naming-unification.md](0010-business-to-biz-naming-unification.md) |
| **RFC-0011** | Operator 与计算 Platform 命名解耦 | `Completed` | `v4.0.0` | Layer 1 (Operator Adapter) / Demo / Layer 4 Terminology | [0011-operator-platform-naming-unification.md](0011-operator-platform-naming-unification.md) |
| **RFC-0012** | I/O 契约驱动的通用 Node 架构 | `Completed` | `v4.1.0` | Layer 1 ~ Layer 4 / Tooling | [0012-node-authoring-experience.md](0012-node-authoring-experience.md) |
| **RFC-0013** | 开发反馈闭环加速 | `Completed` | `v4.2.0` | Tooling / Test Infrastructure | [0013-developer-feedback-loop-acceleration.md](0013-developer-feedback-loop-acceleration.md) |
| **RFC-0014** | 独立公共日志 API 与核心日志统一 | `Completed` | `v4.3.0` | Cross-Cutting / Layer 1 ~ Layer 4 | [0014-public-log-api.md](0014-public-log-api.md) |
| **RFC-0015** | 模型能力与推理运行时解耦实施规范 | `Completed` | `v5.0.0` | Layer 2 ~ Layer 4 | [0015-model-capability-backend-decoupling.md](0015-model-capability-backend-decoupling.md) |
| **RFC-0016** | 构建与测试工作流收敛 | `Completed` | `v5.1.0` | Tooling / Test Infrastructure | [0016-build-and-test-workflow-convergence.md](0016-build-and-test-workflow-convergence.md) |
| **RFC-0017** | 开发治理与 Agent 工作流收敛 | `Completed` | `v5.2.0` | Tooling / Governance | [0017-development-governance-convergence.md](0017-development-governance-convergence.md) |
| **RFC-0018** | 请求黑板与算法句柄并发契约收敛 | `Completed` | `v5.3.0` | Layer 1 ~ Layer 2 | [0018-request-context-and-handle-concurrency-contracts.md](0018-request-context-and-handle-concurrency-contracts.md) |
| **RFC-0019** | 高优先级分层代码收敛 | `Completed` | `v5.4.0` | Layer 1 ~ Layer 4 / Tooling | [0019-high-priority-layer-convergence.md](0019-high-priority-layer-convergence.md) |
| **RFC-0020** | Layer 2 运行时一致性与事务化构建收敛 | `Completed` | `v5.5.0` | Layer 2 / Layer 3 Definition | [0020-layer2-runtime-convergence.md](0020-layer2-runtime-convergence.md) |
| **RFC-0021** | Layer 4 作者体验与执行协议收敛 | `Completed` | `v5.6.0` | Layer 4 | [0021-layer4-authoring-and-protocol-convergence.md](0021-layer4-authoring-and-protocol-convergence.md) |
| **RFC-0022** | 文本规则与 UTF-8 分块安全收敛 | `Completed` | `v5.7.0` | Layer 3 / Layer 4 Text Support | [0022-text-processing-safety.md](0022-text-processing-safety.md) |
| **RFC-0023** | v6 运行时契约破坏性收敛 | `Completed` | `v6.0.0` | Layer 1 ~ Layer 3 / Tooling | [0023-v6-contract-convergence.md](0023-v6-contract-convergence.md) |
| **RFC-0024** | 正式接入前历史兼容契约清理 | `Completed` | `v7.0.0` | Layer 1 ~ Layer 3 / Tooling | [0024-pre-release-contract-cleanup.md](0024-pre-release-contract-cleanup.md) |
| **RFC-0025** | 部署路径、执行目标与可复现验收契约收敛 | `Completed` | `v8.0.0` | Layer 1 ~ Layer 4 / Tooling | [0025-deployment-runtime-contract-convergence.md](0025-deployment-runtime-contract-convergence.md) |
| **RFC-0026** | 统一 LLM 文本生成协议与多 Backend 实现 | `Completed` | `v9.0.0` | Layer 3 ~ Layer 4 | [0026-unified-llm-generation-backends.md](0026-unified-llm-generation-backends.md) |
| **RFC-0027** | 正式接入前源码布局与 C++ 命名空间收敛 | `Completed` | `v10.0.0` | Layer 1 ~ Layer 4 / Tooling | [0027-preproduction-source-layout-and-namespace-convergence.md](0027-preproduction-source-layout-and-namespace-convergence.md) |
| **RFC-0028** | v10.0.0 预发布运行时与 ABI 收口 | `Completed` | `v10.0.0` | Layer 1 ~ Layer 4 / Tooling | [0028-preproduction-runtime-and-abi-hardening.md](0028-preproduction-runtime-and-abi-hardening.md) |
| **RFC-0029** | 外网架构收口与内网 SDK 迁移分阶段整改 | `In Implementation` | `v10.x / 待定` | Layer 1 ~ Layer 4 / Tooling | [0029-external-readiness-and-intranet-sdk-migration.md](0029-external-readiness-and-intranet-sdk-migration.md) |
| **RFC-0030** | 编译期分层边界与轻量运行时计划契约 | `Completed` | `v10.x` | Layer 1 ~ Layer 4 / Build | [0030-compile-time-layer-boundaries.md](0030-compile-time-layer-boundaries.md) |
| **RFC-0031** | 业务 Blackboard Key 所有权拆分 | `Completed` | `v10.x` | Layer 1 ~ Layer 3 | [0031-business-blackboard-key-ownership.md](0031-business-blackboard-key-ownership.md) |
| **RFC-0032** | 从 GitHub 发布包直接接入 kiteLLM | `Completed` | `v10.x` | Layer 4 / Build | [0032-kitellm-direct-github-dependency.md](0032-kitellm-direct-github-dependency.md) |
| **RFC-0033** | 按 kiteLLM 原生接口传递设备选择 | `Completed` | `v10.x` | Layer 4 / Build | [0033-kitellm-native-device-contract.md](0033-kitellm-native-device-contract.md) |
| **RFC-0034** | Kite 原生能力在现有业务中的完整接入 | `Completed` | `v10.x` | Layer 4 / Config / Build | [0034-kitellm-capability-coverage.md](0034-kitellm-capability-coverage.md) |
| **RFC-0035** | Kite 生成 token 向量与中性 Embedding 接入 | `Completed` | `v10.x` | Layer 4 / Config / Build | [0035-generated-token-embedding.md](0035-generated-token-embedding.md) |

---

## 5. 专项验收与评审归档

重大特性的独立评审与验收记录归档于 `doc/rfcs/reviews/`：

- [RFC-0001 初始架构验收评审报告](reviews/0001-four-tier-architecture-acceptance.md)
- [RFC-0003 Pipeline 黑板重构验收报告](reviews/0003-pipeline-dynamic-blackboard-acceptance.md)
- [RFC-0004 平台 Operator 兼容层验收报告](reviews/0004-platform-operator-interface-acceptance.md)
- [RFC-0005 参数化业务 Demo Runner 验收评审报告](reviews/0005-parameterized-business-demo-runner-acceptance.md)
- [RFC-0008 架构契约收敛剩余整改计划](reviews/0008-architecture-contract-consolidation-remediation-plan.md)
- [RFC-0008 架构契约收敛独立验收报告](reviews/0008-architecture-contract-consolidation-acceptance.md)
- [RFC-0008 架构契约收敛复审报告（2026-08-26）](reviews/0008-architecture-contract-consolidation-convergence-review-20260826.md)
- [RFC-0012 Node 架构改造验收与整改指南（2026-08-27）](reviews/0012-node-authoring-experience-acceptance-review-20260827.md)
- [RFC-0015 阶段 3 ONNX Embedding 验收与整改](reviews/0015-stage3-onnx-embedding-acceptance-remediation-20260829.md)
- [RFC-0015 阶段 4 Rerank 验收](reviews/0015-stage4-rerank-acceptance-20260829.md)
- [RFC-0015 阶段 5 llama.cpp + Qwen LLM 验收](reviews/0015-stage5-llm-acceptance-20260829.md)
- [RFC-0015 阶段 6 OCR/ASR 与测试替身验收](reviews/0015-stage6-ocr-asr-fixtures-acceptance-20260829.md)
- [RFC-0015 阶段 7 收口与最终验收](reviews/0015-stage7-closeout-acceptance-20260829.md)
- [RFC-0025 部署运行时契约收敛验收](reviews/0025-deployment-runtime-contract-convergence-acceptance-20260901.md)
- [框架全面审查方案](reviews/framework_comprehensive_review_plan.md)
- [框架全面审查与问题收敛报告](reviews/framework_comprehensive_review_report.md)
