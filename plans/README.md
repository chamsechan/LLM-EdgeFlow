# 工作计划

这里只保留尚未完成的工作及其验收要求。当前用法和契约见 [doc/](../doc/README.md)，
设计讨论、已完成步骤和测试成绩通过 Git 与 PR 追溯。

| 未完成工作 | 范围 |
| --- | --- |
| [Tensor Graph 文本生成 Model](BPE_TOKENIZER_TENSOR_LLM_PLAN.md) | 复用已有 BPE 分词器，完成性能前置验证、Model 和端到端验收 |
| [测试维护规模验收](TEST_SYSTEM_REDESIGN.md) | 按完整统计口径检查维护成本，在保持当前行为覆盖的前提下完成规模验收 |

待需求明确后再安排：

- 多团队接入时确定 CODEOWNERS 与必需审查设置。
- 根据开发者阅读需要决定是否将 CONTRIBUTING 改为中文，保持单一版本。
- 开展真实开发者试用，方法见[验收范围与发布准备](../doc/VERIFIABLE_SELECTION.md#验收范围与发布准备)。
- 出现真实跨请求状态业务时再设计状态接口、所有权和提交边界。

计划只描述未交付工作，不构成现行能力承诺。恢复实施时先核对当前代码与约束。
一个阶段完成后，将形成的规则写入所属指南；最后一个阶段合入后删除计划及本页入口。
单个 PR 的设计直接写在变更说明中，流程见 [CONTRIBUTING](../CONTRIBUTING.md#3-design-and-current-contracts)。
