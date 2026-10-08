---
name: edgeflow-node-developer
description: 新增或修改 LLM-EdgeFlow 的 common/custom Node。所有 Node 使用同一结构（Inputs/Params/Models/Run/Spec + MakeNodeSpec），覆盖逐项变换、文本 LLM、多输入输出、拆分聚合排名、Embedding/ASR/OCR/Rerank、多模型、参数、Control 与缓存。
---

# Node 开发

先从目标 Catalog 确认现有节点或配置不能完成需求；只改提示词或参数时用
[pipeline-composer](../pipeline-composer/SKILL.md)。确需 C++ 时读
[Node 共享契约](../llm-edgeflow-developer-guide/references/capability-nodes.md)，按需查
[概念与签名速查](../../../doc/dev_guide/custom_node_concepts.md)。开发流程遵循
[CONTRIBUTING](../../../CONTRIBUTING.md)。中性通用操作放 `src/common_nodes/`，领域算法按操作放
`src/custom_nodes/`；两者写法相同。

## 统一结构

每个 Node 是一份 `.cpp`：`Inputs`（输入）、可选的 `Params`（配置）与 `Models`（模型）、
`Run`（全部处理逻辑）、`Spec`（把前三者登记给框架）、`REGISTER_FUNCTION_NODE`。
多输出时结果结构叫 `Outputs`。`Run` 的参数依次为 `const Inputs&`、`const Params&`、
`const Models&`，只写 Spec 实际声明的部分；用到会话缓存时再追加 `const SessionResources&`。
改处理逻辑只动 `Run`；增减输入、配置项、模型或改变数量关系时，同时改结构体和 Spec。
不要继承 `NodeBase`、手工操作 Context，也不要为某个业务往通用 Node 加分支。

## 从模板开始

| 实际需求 | 起点 |
| --- | --- |
| 逐项文本变换，数量与来源不变 | `tools/scaffold_custom_node.py <Name> --kind compute --write-test`；`Run` 用 `MapPayloads` 调用逐项函数 |
| 文本前处理 → 一次 LLM → 文本后处理 | `--kind model -m llm`（[starter_llm_node](../../../dev_support/node_authoring/starter_llm_node.cpp)）；生成参数由 `GenerateParameters()` 从配置读取 |
| 同时生成可运行方案 | [text-llm-node Recipe](../../../doc/dev_guide/recipe_text_llm_node.md)，不要再单独运行脚手架 |
| 1:1 的 Embedding/ASR/OCR/Rerank | `--kind model -m <capability>`；检查生成端口是否符合真实算法 |
| 多输入、参数、条件生成 | [starter_batch_node](../../../dev_support/node_authoring/starter_batch_node.cpp) |
| 多模型能力 | [starter_multi_model_node](../../../dev_support/node_authoring/starter_multi_model_node.cpp) |
| 拆分且分配子编号/输出 counts | [TextChunkNode](../../../src/common_nodes/text_chunk_node.cpp) 的 `SplitPayloads` |
| 排名与候选来源 | [TextRerankNode](../../../src/common_nodes/text_rerank_node.cpp) |
| 生成参数加自有配置、请求上下文 | `Parameters<Params>({Field(...)}).Include(&Params::generation, GenerateParameters())`（[生成参数 helper](../../../include/nodes/generate_parameters.h)）；完整示例见 [PromptGuidedLlmNode](../../../src/custom_nodes/prompt_guided_llm_node.cpp) |
| 两批关联/分组/部分调用后回填 | `dev_support/node_authoring/starter_batch_{join,group,select_scatter}_node.cpp` |

示例名称替换成实际操作名；已有实现直接修改，不用 `--force` 覆盖。脚手架默认生成 custom；
common Node 放在 `src/common_nodes/` 并明确 `.Category("common")`。生成的"未实现"占位不是可用算法。

## 声明数据关系

1. `InputsOf` 声明 typed 输入：`Required` 必须存在，`Optional` 可不连接，`OptionalValue`
   才允许连接后请求中仍缺值。多路数据按完整 key 或明确的请求分组关联，不按数组下标配对。
2. 保序结果用带 anchor 的 `PreservedOutput`；多输出用 `OutputsOf` / `Produced`；派生批次用
   `ProducedBatch` 与准确的 `PortFlow`。拆分、过滤、排名的来源正确性由算法与测试保证。
   全部结果在局部成功后返回，由框架发布。
3. `Parameters` / `Field` 声明参数；跨字段与连线规则用 `Validate` / `ValidateBindings`。
   复杂 JSON 用 `ConfigParser`，不重复默认值和字段校验。
4. `ModelsOf` / `Model` 声明能力槽；成员类型 `LlmCall`、`EmbeddingCall`、`AsrCall`、`OcrCall`、
   `RerankCall` 决定能力。配置必须显式引用 model_id；保留门面返回的 `NodeResult` 失败。

只在有需求时加入 Control 或缓存。`WithControls` 只能更新 `Field` 已绑定的参数（可以是数组、映射、对象数组等任何类型，payload 格式由声明生成，至少给出一个受控参数，给出的参数整体替换）；仅由
`WithParser` 声明的字段不能直接加入字段 Control。typed Fields 与 parser 同时存在时，字段
Control 还要求显式 `Prepare`。复杂 `WithControl` 返回完整有效候选，框架不会再跑初始化的
`Prepare`；见 [Control 指南](../../../doc/dev_guide/first_control.md)。缓存使用
`SessionResources::GetOrCreateResult`，key 显式纳入语义参数、输入和模型 revision。
借用视图只在本次同步调用中使用。只有可证明线程安全时设置 `.ParallelSafe(true)`。

## 验证与接入

优先扩展现有测试；`tests/unit/nodes/test_*.cpp` 自动收集到节点 runner，新套件还需纳入
CTest filter；脚手架 `--write-test` 沿用 `CustomNodeCatalogTest`。聚焦测试执行真实 `Run`，
检查实际业务输出、空/缺失输入、多个请求及非零 `sub_id`、数量与来源、模型失败不发布结果；
Control/缓存改动补回滚与并发测试。仅创建成功或检查 Catalog 不证明算法正确。

```bash
cmake --build build --target edgeflow_test_nodes_runner alg_pipeline_tool -j 4
./build/edgeflow_test_nodes_runner --gtest_list_tests
./build/alg_pipeline_tool describe-node <NodeType>
```

确认 Definition 后用 [pipeline-composer](../pipeline-composer/SKILL.md) 接回方案并执行。
最终证据与门禁见 [Verification](../llm-edgeflow-developer-guide/references/verification.md)。
