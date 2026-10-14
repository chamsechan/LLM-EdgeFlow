# 工作计划

本目录只存放跨多个 PR、仍在进行中的工作计划，例如 RFC 和对应的详细设计。

- [项目精简审查与实施计划](PROJECT_SIMPLIFICATION_PLAN.md)：汇总双方复核结论，按优先实施、补充建议、争议与待决策分组；优先项按九个阶段验收。
- [测试体系重新设计与维护计划](TEST_SYSTEM_REDESIGN.md)：从当前生产职责重新确定必要测试，本地重新实施中。
- [BPE 分词器与 Tensor Graph 文本生成 Model](BPE_TOKENIZER_TENSOR_LLM_PLAN.md)：通用 BPE 分词器（按组件参数区分，不按模型拆分）与 `tensor_causal_lm` Model，不新增 Backend；设计已定，未实施。

- **这里不是现行规则。** 现行的架构、契约和操作步骤以 [doc/](../doc/README.md) 及各自的指南为准。
  计划中描述的状态，在对应 PR 合入之前都不成立。
- **每份计划开头写明状态和基线。** 一个阶段完成后，把由此形成的规则写进对应的现行指南。
- **最后一个阶段合入时，删除该计划。** 开发过程留在 Git 和 PR 记录中，不在这里归档。
- **只涉及一个 PR 的设计，不在这里建文件。** 直接写在 PR 描述中，见
  [CONTRIBUTING](../CONTRIBUTING.md#3-design-and-current-contracts)。
