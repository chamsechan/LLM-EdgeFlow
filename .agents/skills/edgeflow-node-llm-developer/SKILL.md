---
name: edgeflow-node-llm-developer
description: 用 MakeLlmTextSpec 新增 LLM-EdgeFlow 文本 LLM Node，编写 BuildPrompt/FormatAnswer 两个函数，保持单输入单输出及一次生成。仅改提示词优先配置；动态采样、多模型、多轮或多路输入转 Batch skill。
---

# 文本 LLM 两函数 Node

先判断已有生成/模板节点的配置是否足够；足够则使用
[pipeline-composer](../pipeline-composer/SKILL.md)。确需 C++ 前后处理时，读取
[Node 共享契约](../llm-edgeflow-developer-guide/references/capability-nodes.md) 和
[两函数练习](../../../doc/dev_guide/first_custom_node.md)。开发流程遵循
[CONTRIBUTING](../../../CONTRIBUTING.md)。

## 开发最小算法

使用 [starter_llm_node.cpp](../../../dev_support/node_authoring/starter_llm_node.cpp)：
`BuildPrompt` 从输入文本构造提示词，`FormatAnswer` 处理模型文本；
`MakeLlmTextSpec` 组合一次批量文本生成，自动保持数量、顺序与 `(req_id, sub_id)`。
这里“一次”指一次生成调用，不是只解码一个 token。平台 JSON 提取/响应序列化仍归 Adapter。

```bash
python3 tools/scaffold_custom_node.py ExtractFactsNode --kind model -m llm \
  --add-to-cmake --write-test
```

名称按真实操作替换。脚手架写入 `src/custom_nodes/` 并登记源码及测试；中性 common
操作仍使用同一 API，但登记到 common 的 CMake 并明确 category。
Spec 用 `REGISTER_FUNCTION_NODE` 注册，无需另写生命周期或 Definition。

需要同时生成完整可运行方案时，可以使用
[text-llm-node Recipe](../../../doc/dev_guide/recipe_text_llm_node.md)，按该文档执行 prepare
和它返回的 verify 命令；不要再重复运行独立脚手架。Recipe 仅支持单输出部署，源方案
必须恰有一个符合条件的文本 LLM 替换点，并且不会移植被替换节点的算法或采样字段。

## 识别何时改用 Batch

轻量模板只要求模型引用 `bind_model`，不自动支持完整生成节点的所有配置。
`MakeLlmTextSpec` 的 `GenerateOptions` 在构建 Spec 时固定；传入 Parameters 并不会让
采样参数随请求快照改变。需要可配置/Control 采样时，用
[Batch skill](../edgeflow-node-batch-developer/SKILL.md) 在 Run 中从参数构造 options。
无字段 Control 的复杂配置可复用 `GenerateOptionsFields` / `ParseGenerateOptions`，
显式选择 token 默认值；需要字段 Control 时优先 typed `Field`，其与 parser-only 字段
的区别及复杂更新路径见 Batch skill。
多输入、上下文汇合、结构化多输出、条件第二次生成或多模型同样走 Batch。

## 验证真实函数行为

测试实际提示词、模型调用次数、后处理文本、空批次、多请求/非零子编号及模型失败。
借助 `NodeHarness` 注入确定性模型，失败不能发布部分输出；不要仅断言创建成功或 Mock 固定答案。
将独立业务期望写进生成测试，再构建节点 runner 和目标工具：

```bash
cmake --build build --target edgeflow_test_nodes_runner alg_pipeline_tool -j 4
./build/edgeflow_test_nodes_runner --gtest_list_tests
./build/alg_pipeline_tool describe-node ExtractFactsNode
```

运行实际过滤器，回到用户 Pipeline 校验和执行。使用测试注册的方案全程选择测试工具，
真实效果仍需对应模型与样例。最终证据和门禁见
[Verification](../llm-edgeflow-developer-guide/references/verification.md)。
