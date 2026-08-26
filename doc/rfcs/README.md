# LLM-EdgeFlow RFC 需求与技术设计文档库 (RFC Directory)

本目录（`doc/rfcs/`）是 **LLM-EdgeFlow** 项目所有**待实现需求、重大架构重构、接口协议演进及技术设计规范**的唯一事实源（Single Source of Truth）。

---

## 🧭 1. RFC 驱动开发治理流程 (RFC-Driven Development)

在 LLM-EdgeFlow 中，任何新需求、新特性或架构改动均须严格遵守以下 **5 步生命周期规范**：

```text
[ 1. 创建特性分支 ]
       │  (git checkout -b feat/<feature-name>)
       ▼
[ 2. 撰写 RFC 设计文档 ]
       │  (复制 RFC_TEMPLATE.md -> doc/rfcs/NNNN-<feature-name>.md，状态为 Proposed / In Implementation)
       ▼
[ 3. 隔离分支开发与测试编写 ]
       │  (严格遵循 4 层架构隔离，同步编写 Google Test 测试套件)
       ▼
[ 4. 全量回归测试门禁 ]
       │  (100% CTest 通过 + ./scripts/run_all_tests.sh 六阶段全量通过 + format.sh)
       ▼
[ 5. 更新 RFC 状态并合入 main ]
       │  (更新 RFC 状态为 Completed，提交 PR 并合并至 main 分支)
       ▼
[ 6. 需求正式发布闭环 ]
```

---

## 🏷️ 2. RFC 生命周期状态定义 (Lifecycle States)

每个 RFC 文档头部必须包含标准的 Metadata 状态标识：

| 状态 (Status) | 说明 |
| :--- | :--- |
| **`Draft`** | 草案阶段：需求初步提出，方案正在探索与讨论中。 |
| **`Proposed`** | 评审阶段：技术方案已成型，等待团队或架构师评审通过。 |
| **`In Implementation`** | 实现阶段：已在特性分支（`feat/*`）中进行代码与测试用例开发。 |
| **`Completed`** | 已完成：功能已完整实现并通过 100% 测试门禁，已成功合入 `main` 主分支。 |
| **`Deprecated`** / **`Rejected`** | 已废弃 / 已否决：方案被后续 RFC 取代或评审未通过。 |

---

## 📑 3. RFC 文件命名规范

RFC 文档统一存放在 `doc/rfcs/` 根目录下，采用 **四位自增编号 + 英文小写破折号** 命名：

```text
doc/rfcs/NNNN-<kebab-case-title>.md
```

**示例**：
- `doc/rfcs/0001-four-tier-architecture-foundation.md`
- `doc/rfcs/0004-platform-operator-interface-compatibility.md`
- `doc/rfcs/0005-audio-stream-vsl-support.md`

> 💡 **编写模板**：请直接复制并参考 [`RFC_TEMPLATE.md`](RFC_TEMPLATE.md) 进行撰写。

---

## 📚 4. 历史与现存 RFC 索引目录 (RFC Index)

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
| **RFC-0009** | 公司平台 C 结构体槽位绑定与输出内存池 | `In Implementation` | `v3.0.0` | Layer 1 (Platform Operator) / Demo | [0009-company-string-and-slot-map-struct-binding.md](0009-company-string-and-slot-map-struct-binding.md) |

---

## 🔍 5. 专项验收与评审归档 (Reviews & Acceptance)

重大特性的独立评审与验收记录归档于 `doc/rfcs/reviews/`：

- [RFC-0001 初始架构验收评审报告](reviews/0001-four-tier-architecture-acceptance.md)
- [RFC-0003 Pipeline 黑板重构验收报告](reviews/0003-pipeline-dynamic-blackboard-acceptance.md)
- [RFC-0004 平台 Operator 兼容层验收报告](reviews/0004-platform-operator-interface-acceptance.md)
- [RFC-0005 参数化业务 Demo Runner 验收评审报告](reviews/0005-parameterized-business-demo-runner-acceptance.md)
- [RFC-0008 架构契约收敛剩余整改计划](reviews/0008-architecture-contract-consolidation-remediation-plan.md)
- [RFC-0008 架构契约收敛独立验收报告](reviews/0008-architecture-contract-consolidation-acceptance.md)
- [RFC-0008 架构契约收敛复审报告（2026-08-26）](reviews/0008-architecture-contract-consolidation-convergence-review-20260826.md)
- [框架全面审查方案](reviews/framework_comprehensive_review_plan.md)
- [框架全面审查与问题收敛报告](reviews/framework_comprehensive_review_report.md)
