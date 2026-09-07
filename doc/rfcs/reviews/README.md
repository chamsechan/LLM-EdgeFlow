# RFC 评审与验收归档

这些材料记录特定基线的评审、整改过程和验证结果。先从下表选择结论入口，需要追溯时
再展开阶段材料。正文中的“当前”、FAIL、未完成清单和旧交付步骤都属于原始上下文；
RFC 生命周期以 [RFC 索引](../README.md)为准，日常开发见[当前指南](../../README.md)。

## 结论与证据入口

| 决策或审查 | 优先阅读 | 范围说明 |
| --- | --- | --- |
| RFC-0001 | [架构验收](0001-four-tier-architecture-acceptance.md) | 初始架构整改与当时未覆盖的后续阶段 |
| RFC-0003 | [Pipeline 与 Blackboard 基线重评审](0003-pipeline-dynamic-blackboard-acceptance.md) | 指定旧基线的问题和重构建议 |
| RFC-0004 | [Operator 接口验收](0004-platform-operator-interface-acceptance.md) | 当时接口、异常及资源边界验证 |
| RFC-0005 | [Demo Runner 评审](0005-parameterized-business-demo-runner-acceptance.md) | 包含多轮结论及当时失败项，不代表当前 Demo 状态 |
| RFC-0008 | [交付验收](0008-architecture-contract-consolidation-acceptance.md) | 记录 RCR-001～RCR-006 的关闭结果；前序材料见下方 |
| RFC-0009 | [槽位绑定与输出池验收、整改记录](0009-company-string-and-slot-map-struct-binding-remediation-plan.md) | 先看第 2 节与后续复验结论，再按需追溯实施清单 |
| RFC-0012 | [Node 架构验收与整改](0012-node-authoring-experience-acceptance-review-20260827.md) | 当时实现的未通过项与整改依据 |
| RFC-0015 | [阶段 7 最终验收](0015-stage7-closeout-acceptance-20260829.md) | Model/Backend 解耦闭环及 Sanitizer 验证限制；阶段材料见下方 |
| RFC-0025 | [部署运行时验收](0025-deployment-runtime-contract-convergence-acceptance-20260901.md) | 当轮 CPU 模型和部署契约验证 |
| RFC-0037 | [关键词验收 JSON](0037-keyword-acceptance.json) · [五阶段交付记录](../../archive/AUDIT_REMEDIATION_REPORT_2026-09-06.md) | 历史样本与部署指纹；变更后的方案需重新验证 |
| 框架全面审查 | [审查与问题收敛报告](framework_comprehensive_review_report.md) | 2026-08-23 的整改复验和有条件结论 |

## 阶段材料（按需追溯）

<details>
<summary>RFC-0008：整改计划、复审与再复审</summary>

- [剩余整改计划](0008-architecture-contract-consolidation-remediation-plan.md)
- [收敛复审](0008-architecture-contract-consolidation-convergence-review-20260826.md)
- [收敛再复审](0008-architecture-contract-consolidation-convergence-recheck-20260826.md)

</details>

<details>
<summary>RFC-0015：初评、接续指南及阶段 3–6</summary>

本次决策将模型语义与运行时资源拆开，使 Node 通过类型化模型能力调用 Model，
Model 再使用 Backend 的中性执行协议。正文见 [RFC-0015](../0015-model-capability-backend-decoupling.md)；
以下旧接口和过渡方案用于解释迁移过程，当前扩展方式见[开发者指南](../../developer_guide.md)。

| 阶段 | 过程记录 | 验证记录 |
| --- | --- | --- |
| 初评与整改 | [整改计划](0015-model-capability-backend-decoupling-remediation-plan-20260829.md) · [后续实现指南](0015-model-capability-backend-decoupling-continuation-guide-20260829.md) | [Stub 实现评审](0015-model-capability-backend-decoupling-stub-review-20260828.md) |
| 3：ONNX Embedding | 整改要求与验收合并记录 | [阶段 3 验收与整改](0015-stage3-onnx-embedding-acceptance-remediation-20260829.md) |
| 4：Rerank | [阶段 4 实施指南](0015-stage4-rerank-implementation-guide-20260829.md) | [阶段 4 验收](0015-stage4-rerank-acceptance-20260829.md) |
| 5：LLM | [阶段 5 实施指南](0015-stage5-llm-implementation-guide-20260829.md) | [阶段 5 验收](0015-stage5-llm-acceptance-20260829.md) |
| 6：OCR/ASR 与测试替身 | 迁移与边界见验收记录 | [阶段 6 验收](0015-stage6-ocr-asr-fixtures-acceptance-20260829.md) |

</details>

<details>
<summary>框架全面审查：原始审查方案</summary>

- [审查方法与证据范围](framework_comprehensive_review_plan.md)

</details>

新增材料沿用 [RFC 维护规则](../README.md#编写与维护)。现有历史文件保留原路径，
不要把旧阶段清单复制成新待办，也不要用后来的通过结果改写早期失败证据。
