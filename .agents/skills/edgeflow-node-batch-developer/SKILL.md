---
name: edgeflow-node-batch-developer
description: 用 MakeBatchSpec 新增或修改 LLM-EdgeFlow 多输入输出、拆分聚合排名、可配置 LLM、Embedding/ASR/OCR/Rerank 或多模型 Node。覆盖来源关系、参数、Control 和缓存；单项纯变换或固定文本 LLM 优先对应轻量 skill。
---

# 通用 Batch 与模型能力 Node

使用现有 `MakeBatchSpec` 和普通 Run 函数，不新增 Node 生命周期体系。
先读 [Node 共享契约](../llm-edgeflow-developer-guide/references/capability-nodes.md)，
按需查 [batch 与参数概念](../../../doc/dev_guide/custom_node_concepts.md)。开发流程遵循
[CONTRIBUTING](../../../CONTRIBUTING.md)。算法归属决定 common/custom 目录；执行方式相同。

## 声明数据关系，再写算法

1. 用 `InputsOf` 声明 typed 输入。`Required` 必须存在，`Optional` 可不连接，
   `OptionalValue` 才允许连接后请求中仍缺值。选择符合算法的模型能力和输出关系。
2. 保序结果使用带 anchor 的 `PreservedOutput`；多输出用 `OutputsOf` / `Produced`，
   派生批次用 `ProducedBatch` 与准确的 `PortFlow`。拆分、过滤、排名的来源正确性需算法
   与测试保证，不能只写一个 flow 字符串。全部结果在局部成功后返回，由框架发布。
3. `Parameters` / `Field` 声明参数；跨字段与连线规则用 `Validate` / `ValidateBindings`。
   复杂 JSON 使用 `NodeConfigParser`，不要重复默认值/字段校验。
4. `ModelsOf` / `Model` 声明能力槽；成员使用 `LlmCall`、`EmbeddingCall`、`AsrCall`、
   `OcrCall` 或 `RerankCall`。配置必须显式引用 model_id，模型文件加载归 Model/Backend。
   保留门面返回的 `NodeResult` 失败，不另行映射框架错误码。

## 按当前实现选模板

| 实际需求 | 只读对应例子 |
| --- | --- |
| 多输入、参数、条件生成 | [starter_batch_node](../../../dev_support/node_authoring/starter_batch_node.cpp) |
| 多模型能力 | [starter_multi_model_node](../../../dev_support/node_authoring/starter_multi_model_node.cpp) |
| 1:1 的 Embedding/ASR/OCR/Rerank 起点 | `tools/scaffold_custom_node.py --kind model -m <capability>`；检查生成端口是否符合真实算法 |
| 拆分且分配子编号/输出 counts | [TextChunkNode](../../../src/common_nodes/text_chunk_node.cpp) 的 `SplitPayloads` |
| 排名与候选来源 | [TextRerankNode](../../../src/common_nodes/text_rerank_node.cpp) |
| 可配置 LLM 参数与请求上下文 | [PromptGuidedLlmNode](../../../src/custom_nodes/prompt_guided_llm_node.cpp)，共用 [生成参数 helper](../../../include/nodes/generate_options_config.h) |
| 两批关联/分组/部分调用后回填 | `dev_support/node_authoring/starter_batch_{join,group,select_scatter}_node.cpp` |

通用 Batch 没有单独的 `--kind batch` 生成器；从上述已编译例子提取所需 Spec，不猜命令。
多路数据按完整 key 或明确的 request 分组关系关联，不能默认按数组下标配对。
借用视图只在本次同步调用中使用；跨生命周期的数据必须拥有存储。

只在有需求时加入 Control 或缓存：`WithControls` 只能更新 `Field` 已绑定的参数，
例如需动态更新 temperature/max_tokens 时直接用 typed Fields。仅由 `WithParser` 声明的
字段不能直接加入字段 Control，添加 `Prepare` 也不会使其成为 typed binding。
若 typed Fields 与 parser 同时存在，字段 Control 还要求显式 `Prepare` 重建派生状态。
复杂 `WithControl` 则显式复用规范化/语义校验与状态构建，返回完整有效候选；框架不会再跑
初始化的 `Prepare`。wire schema 和示例见 [Control 指南](../../../doc/dev_guide/first_control.md)。
缓存使用 `SessionResources::GetOrCreateResult`；key 显式纳入语义参数、输入和模型 revision。
框架负责快照/并发更新及 single-flight，不代替算法决定缓存身份。

## 登记与验收

以 `REGISTER_FUNCTION_NODE` 登记 Spec，源码加入所属 common/custom CMake 的
`edgeflow_capability_nodes_objects`。common 显式设置 category；领域算法不为进入 common 而泛化。
优先扩展现有测试文件/套件。`tests/unit/nodes/test_*.cpp` 由
`tests/RuntimeTests.cmake` 自动收集到节点 runner；新测试套件还需纳入 CTest filter。
custom 脚手架用 `--write-test` 生成自动编入的测试，沿用 `CustomNodeCatalogTest`
的现有过滤器；`--add-to-cmake` 登记生产 Node 源码。
只有确需并行且可证明线程安全时设置 `.ParallelSafe(true)`，再验证整个计划的并发限制。

聚焦测试至少执行真实 Run，检查不同请求、非零子编号、空/缺失输入、数量变化、来源关系、
模型失败不发布结果及算法特有错误；Control/cache 改动按涉及行为补回滚/并发测试。
构建 `edgeflow_test_nodes_runner` 和 `alg_pipeline_tool`，运行实际测试过滤器，检查新
`describe-node`，用 [pipeline-composer](../pipeline-composer/SKILL.md) 接回方案并执行。
最终证据与门禁见 [Verification](../llm-edgeflow-developer-guide/references/verification.md)。
