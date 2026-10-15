# 任务：新增文本 LLM Node

`text-llm-node` 生成 Node 源码、测试、Pipeline、部署配置和效果样例。
主要编辑 `BuildPrompt`、`FormatAnswer`，并补充业务测试。

## 创建

先按 [README](../../README.md#快速开始)配置构建目录，再构建下面的工具。
本例使用无需模型权重的测试 Profile，已有上游提示词模板；Recipe 只替换中间的文本 LLM Node，保留连线。

```bash
cmake --build build --target alg_pipeline_tool_test alg_demo
python3 tools/dev_recipe.py prepare text-llm-node \
  --name my_summary --profile entity_extract_mock \
  --tool build/alg_pipeline_tool_test --build-dir build \
  --pipeline demo/fixtures/mock/pipeline_my_summary.json --json
```

## 编辑与验证

源码在 `src/custom_nodes/my_summary_node.cpp`，测试在
`tests/unit/nodes/test_my_summary_node.cpp`。生成测试使用一条输入，检查模型调用、结果及来源编号。
修改算法后补充自己的业务期望和边界用例；空输入、模型失败等公共机制另由框架测试覆盖。

执行 prepare 打印的唯一 verify 命令，例如：

```bash
python3 tools/dev_recipe.py verify text-llm-node \
  --name my_summary --pipeline demo/fixtures/mock/pipeline_my_summary.json \
  --tool build/alg_pipeline_tool_test --build-dir build \
  --effects demo/fixtures/mock/pipeline_my_summary_effects.json \
  --manifest tests/fixtures/asset_manifest_test.json \
  --demo build/alg_demo --json
```

verify 会重新构建工具与节点，执行生成的测试，校验配置和资产，再运行方案、核对业务输出。
失败时返回非零状态，并指出失败阶段。

其他 Profile 的效果样例与资产要求见[提示词任务](recipe_prompt_config.md)。

## 替换范围与参数

- 名字使用未注册的 snake_case；已有文件不会被覆盖，冲突时按工具提示处理。
- Recipe 要求方案只有一个输出 Converter，并能唯一找到可替换的文本 LLM 节点：
  单输入、单个 `TextBatch` 输出、`1:1 / preserve`、请求生命周期，其他输出未被引用。
  不满足条件时，使用[手工接入](first_custom_node.md)。
- 连线会自动更新。原节点的业务算法需在新函数中实现；生成参数使用默认值，
  需要保留原采样设置时，在新节点的 `params` 中补上。

更多 Node 作者接口见[首个自定义节点](first_custom_node.md)和
[自定义节点概念](custom_node_concepts.md)。
