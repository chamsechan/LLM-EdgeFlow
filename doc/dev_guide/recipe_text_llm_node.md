# 任务：新增文本 LLM Node

`text-llm-node` 自动创建源码、真实测试文件、两处 CMake 登记、Pipeline、conf 和效果样例。
开发者主要编辑 `BuildPrompt`、`FormatAnswer` 以及独立业务期望。

## 创建

先配置构建目录并构建选定工具。以下 Profile 已有上游提示词模板，recipe 只替换中间的文本
LLM Node，保留其上下游 Blackboard 键与依赖关系。

```bash
cmake --build build --target alg_pipeline_tool_test alg_demo
python3 scripts/dev_recipe.py prepare text-llm-node \
  --name MySummaryNode --profile entity_extract_mock \
  --tool build/alg_pipeline_tool_test --build-dir build \
  --pipeline configs/pipeline_my_summary.json --json
```

名字必须是 PascalCase C++ 标识符，省略 `Node` 后缀时会自动添加。目标必须未存在且 Node
类型不能已注册。recipe 从 Catalog 选择唯一一个单输入/单输出、`TextBatch`、`1:1`、
`preserve`、request lifetime 的 LLM 节点；没有兼容点或存在多个点时在写入前报错。
不会通过节点名称猜测能力，也不会回退替换第一个节点。

源 Node 的逻辑端口（如 prompt/text）会映射为新模板的 input/output，实际键保持不变。
新模板不会自动复制被替换 Custom Node 的业务算法；替换自带提示词构造或结果加工的节点时，
应在新的业务函数中实现所需行为，效果验收会检测行为差异。

## 编辑与验证

源码在 `src/custom_nodes/my_summary_node.cpp`，测试在
`tests/unit/nodes/test_my_summary_node.cpp`。业务示例包含输入和独立字面量期望；机械的空输入、
失败隔离、数量与来源检查已生成。修改算法后同步调整业务需求对应的期望，不要删除失败断言。

执行 prepare 打印的唯一 verify 命令，例如：

```bash
python3 scripts/dev_recipe.py verify text-llm-node \
  --name MySummaryNode --pipeline configs/pipeline_my_summary.json \
  --tool build/alg_pipeline_tool_test --build-dir build \
  --effects configs/pipeline_my_summary_effects.json \
  --model-root . --manifest tests/fixtures/asset_manifest_test.json \
  --demo build/alg_demo --json
```

verify 依次完成：

1. 检查输入、效果样例、单输出部署和工具/构建目录的一致性。
2. 增量构建**选定的** CLI、Demo，以及当前 sharded/individual 模式的 Node runner；
   Demo 尚未存在时也可在此构建。
3. 在更新后的 Catalog 中确认新 Node，并确认 Pipeline 实际使用该 Node。
4. 原生 validate、plan 和 resolve-conf，确认部署指向本 Pipeline 和所选模型资产。
5. 发现 `CustomNodeCatalogTest.MySummaryNode_*` 用例并要求非零，执行后要求全部通过。
6. 校验资产哈希，运行实际修改的方案，核对每个请求的业务输出。

任一步失败会保留失败阶段、原生报告及未完成步骤，返回非零状态。新增源码被 Node runner
编译通过，不代表所选 CLI 或 Demo 已更新；verify 会明确构建这些独立目标。

其他 Profile 的 `--effects`、`--model-root`、`--manifest` 使用方式见
[提示词任务](recipe_prompt_config.md)。两条 recipe 只支持 `data.mem_que` 单输出；多输出部署
继续使用原生 Operator 流程。

## 文件冲突

生成器用目录锁协调同时运行的生成任务，并以不替换已有目标的原子发布操作写入新文件。
修改登记文件时先保留实际旧版本，再检查并发布；检测到编辑冲突时中止，回滚只处理本次写入
的版本。如果原路径已被其他编辑占用，保留冲突版本并打印恢复文件位置，避免覆盖用户内容。

更多 Node 作者接口见[首个自定义节点](first_custom_node.md)和
[自定义节点概念](custom_node_concepts.md)。
