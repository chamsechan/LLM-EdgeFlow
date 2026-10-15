# 任务：新增文本 LLM Node

`text-llm-node` 生成 Node 源码、测试、Pipeline、部署配置和效果样例。
主要编辑 `BuildPrompt`、`FormatAnswer`，并补充业务测试。

## 创建

先按 [README](../../README.md#快速开始)配置构建目录，再构建下面的工具。
本例使用测试 Profile，无需下载权重。上游节点已提供提示词。Recipe 替换文本 LLM Node，并保留连线。

```bash
cmake --build build --target alg_pipeline_tool_test alg_demo
python3 tools/dev_recipe.py prepare text-llm-node \
  --name my_summary --profile entity_extract_mock \
  --tool build/alg_pipeline_tool_test --build-dir build \
  --pipeline demo/fixtures/mock/pipeline_my_summary.json --json
```

## 编辑与验证

1. 编辑 `src/custom_nodes/my_summary_node.cpp` 中的业务函数。
2. 在 `tests/unit/nodes/test_my_summary_node.cpp` 中补充业务预期和边界用例。

生成的测试检查一条输入的模型调用、结果和来源编号。框架测试覆盖空输入、模型失败等公共规则。

执行 prepare 打印的唯一 verify 命令，例如：

```bash
python3 tools/dev_recipe.py verify text-llm-node \
  --name my_summary --pipeline demo/fixtures/mock/pipeline_my_summary.json \
  --tool build/alg_pipeline_tool_test --build-dir build \
  --effects demo/fixtures/mock/pipeline_my_summary_effects.json \
  --manifest tests/fixtures/asset_manifest_test.json \
  --demo build/alg_demo --json
```

verify 重新构建工具和节点，然后执行生成的测试。
接着，verify 校验配置和资产，并运行方案。最后，verify 核对业务输出。
如果验证失败，工具返回非零状态并报告失败阶段。

其他 Profile 的效果样例与资产要求见[提示词任务](recipe_prompt_config.md)。

## 替换范围与参数

- 名字使用未注册的 snake_case。已有文件不会被覆盖，冲突时按工具提示处理。
- Recipe 要求方案只有一个输出 Converter，并能唯一找到可替换的文本 LLM 节点：
  单输入、单个 `TextBatch` 输出、`1:1 / preserve`、请求生命周期，其他输出未被引用。
  不满足条件时，使用[手工接入](first_custom_node.md)。
- Recipe 自动更新连线。在新函数中实现原节点需要保留的业务算法。
- 生成参数使用默认值。如需保留原采样设置，在新节点的 `params` 中填写对应值。

更多 Node 作者接口见[首个自定义节点](first_custom_node.md)和
[自定义节点概念](custom_node_concepts.md)。
