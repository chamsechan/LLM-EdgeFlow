---
name: edgeflow-node-map-developer
description: 新增或修改 LLM-EdgeFlow 单输入、逐项一对一、无需模型的纯计算 Node，使用 MakeMapSpec 自动保持顺序和来源。适用于 common/custom 的文本或载荷变换；拆分、过滤、聚合转 Batch skill。
---

# 逐项纯计算 Node

先从目标 Catalog 确认现有节点或配置不能完成需求，再读取
[Node 共享契约](../llm-edgeflow-developer-guide/references/capability-nodes.md)。
[CONTRIBUTING](../../../CONTRIBUTING.md) 管理开发流程。中性通用操作放 `src/common_nodes/`；
领域算法放 `src/custom_nodes/`，按操作命名，二者使用同一作者 API。

## 选择与实现

适用条件：一项输入产生一项输出，不调用模型、不跨样本关联、不改变数量或来源。
使用普通载荷函数与 `MakeMapSpec`，通过 `REGISTER_FUNCTION_NODE` 从 Spec 生成运行时与
Definition。框架负责批循环、数量、顺序、来源和发布；不要继承 `NodeBase` 或手工操作 Context。

TextBatch → TextBatch 的 custom Node 可使用现有脚手架：

```bash
python3 tools/scaffold_custom_node.py NormalizeTextNode --kind compute \
  --in-port input:TextBatch --out-port output:TextBatch --add-to-cmake --write-test
```

示例名称需替换成实际操作名，已有实现直接修改，不用 `--force` 覆盖。
脚手架的最简 Map 路径只自动生成该文本形态；其他载荷类型按实际 `MakeMapSpec` 接口实现，
不要把生成的“未实现”占位当可用算法。填入真实算法和独立业务期望；需要字段时用
`Parameters` / `Field` 绑定普通参数结构，
默认值、范围与语义只声明一次。common Node 选择相同 API，登记到
`src/common_nodes/CMakeLists.txt` 并明确 `.Category("common")`；脚手架默认生成 custom。

以下情况改用 [Batch skill](../edgeflow-node-batch-developer/SKILL.md)：过滤/拆分/聚合、
多输入/输出、请求分组、需要检查整批数据或模型调用。逐项函数自身可以返回 `NodeResult`
表达失败，框架会阻止发布部分结果。文本 LLM 前后处理使用
[LLM skill](../edgeflow-node-llm-developer/SKILL.md)。不要让 Map 的输出悄悄丢项或重新编号。

## 验证与接入

复用生成的测试或 `tests/unit/nodes/` 的现有套件，参考
[NodeHarness](../../../tests/support/node_harness.h) 的真实接口。
断言实际业务输出、空输入、多个请求及非零 `sub_id`、输入未修改；增加算法自身的边界。
仅创建成功或检查 Catalog 不证明算法正确。

```bash
cmake --build build --target edgeflow_test_nodes_runner alg_pipeline_tool -j 4
./build/edgeflow_test_nodes_runner --gtest_list_tests
./build/alg_pipeline_tool describe-node NormalizeTextNode
```

按实际套件运行聚焦测试。确认 Definition 的类型、字段和 `1:1`/`preserve` 后，
用 [pipeline-composer](../pipeline-composer/SKILL.md) 连接节点，validate/plan 并执行用户方案。
最终交付按 [Verification](../llm-edgeflow-developer-guide/references/verification.md)。
