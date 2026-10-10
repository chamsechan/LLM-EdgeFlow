# 任务：新增文本 LLM Node

`text-llm-node` 自动创建源码、真实测试文件、Pipeline、conf 和效果样例；
测试由节点 runner 按目录自动收集。
开发者主要编辑 `BuildPrompt`、`FormatAnswer` 以及独立业务期望。recipe 与独立脚手架统一生成普通函数和 Spec，使用同一注册及执行机制。

## 创建

先配置构建目录并构建选定工具。以下 Profile 已有上游提示词模板，recipe 只替换中间的文本
LLM Node，保留其上下游 Blackboard 键与依赖关系。

```bash
cmake --build build --target alg_pipeline_tool_test alg_demo
python3 tools/dev_recipe.py prepare text-llm-node \
  --name my_summary --profile entity_extract_mock \
  --tool build/alg_pipeline_tool_test --build-dir build \
  --pipeline demo/fixtures/mock/pipeline_my_summary.json --json
```

名字使用 snake_case，同时作为登记的节点类型；C++ 类型与源码名由脚手架生成。目标必须未存在且
类型不能已注册。Recipe 从 Catalog 选择唯一一个单输入、单个 `TextBatch` 输出、`1:1`、
`preserve`、request lifetime 的 LLM 节点；其他输出必须未被引用。没有兼容点、存在多个点或
结构化输出仍被使用时，在写入前报错。
不会通过节点名称猜测能力，也不会回退替换第一个节点。

源 Node 的逻辑端口映射为新模板的 input/output；所有下游与输出 Converter 的引用同步改为
`新节点名.output`，额外顺序依赖同步更新。
新模板不会自动复制被替换 Custom Node 的业务算法；替换自带提示词构造或结果加工的节点时，
应在新的业务函数中实现所需行为，效果验收会检测行为差异。新节点的 `params` 只写 `bind_model`，
`max_tokens`、`temperature` 等生成参数使用默认值；源节点调过的采样字段不会复制，需要时在
方案配置中补上。

## 编辑与验证

源码在 `src/custom_nodes/my_summary_node.cpp`，测试在
`tests/unit/nodes/test_my_summary_node.cpp`。业务示例包含输入和独立字面量期望；机械的空输入、
失败隔离、数量与来源检查已生成。修改算法后同步调整业务需求对应的期望，不要删除失败断言。

执行 prepare 打印的唯一 verify 命令，例如：

```bash
python3 tools/dev_recipe.py verify text-llm-node \
  --name my_summary --pipeline demo/fixtures/mock/pipeline_my_summary.json \
  --tool build/alg_pipeline_tool_test --build-dir build \
  --effects demo/fixtures/mock/pipeline_my_summary_effects.json \
  --manifest tests/fixtures/asset_manifest_test.json \
  --demo build/alg_demo --json
```

verify 依次完成：

1. 检查输入、效果样例、单输出部署和工具/构建目录的一致性。
2. 增量构建**选定的** CLI、Demo，以及 `edgeflow_test_nodes_runner`；
   Demo 尚未存在时也可在此构建。
3. 在更新后的 Catalog 中确认新 Node，并确认 Pipeline 实际使用该 Node。
4. 原生 validate、plan 和 resolve-conf，确认部署指向本 Pipeline 和所选模型资产。
5. 发现 `CustomNodeCatalogTest.my_summary_*` 用例并要求非零，执行后要求全部通过。
6. 校验资产哈希，运行实际修改的方案，核对每个请求的业务输出。

任一步失败会保留失败阶段、原生报告及未完成步骤，返回非零状态。新增源码被 Node runner
编译通过，不代表所选 CLI 或 Demo 已更新；verify 会明确构建这些独立目标。

其他 Profile 的 `--effects`、`--manifest` 与资源目录要求见
[提示词任务](recipe_prompt_config.md)。两条 Recipe 只支持一个输出 Converter；多输出部署使用原生 Operator 流程。

## 文件冲突

生成器用目录锁协调同时运行的生成任务，并以不替换已有目标的原子发布操作写入新文件。
修改登记文件时先保留实际旧版本，再检查并发布；检测到编辑冲突时中止，回滚只处理本次写入
的版本。如果原路径已被其他编辑占用，保留冲突版本并打印恢复文件位置，避免覆盖用户内容。

更多 Node 作者接口见[首个自定义节点](first_custom_node.md)和
[自定义节点概念](custom_node_concepts.md)。
