# 自定义 Node

这里存放领域算法、特定前后处理和模型调用组合，与 `src/common_nodes/` 同属能力节点层。
按操作组织 `*_node.cpp`。同一 Node 可供多个 Pipeline 使用。

## 第一次开发从这里开始

已有能力能通过连线完成时，直接使用 Pipeline。需要新增算法时，跟随
[第一个自定义 Node](../../doc/dev_guide/first_custom_node.md) 完成生成、修改、编译和运行。
[按需参考](../../doc/dev_guide/custom_node_concepts.md)解释端口、来源、模型、Catalog 和并发。

所有 Node 使用同一结构（`Inputs`、可选的 `Params` 与 `Models`、`Run`、`Spec`），通过
`REGISTER_FUNCTION_NODE` 从 Spec 生成绑定、Definition 和执行包装。`NodeBase` 是框架内部运行机制，
业务算法写在普通函数中。

## 通用开发步骤速查

1. 运行 `build/alg_pipeline_tool catalog`，确认已有能力。用 `describe-node` 查看所需节点。
2. 生成普通函数、Spec 和对应测试：

   ```bash
   ./tools/scaffold_custom_node.py CustomFilterNode --kind compute --write-test
   ./tools/scaffold_custom_node.py DomainPromptNode --kind model -m llm --write-test
   ./tools/scaffold_custom_node.py FastAudioNode --kind model -m asr --write-test
   ./tools/scaffold_custom_node.py PrefixControlNode --control-id 1001 --write-test
   ```

   如需预览生成内容，添加 `--dry-run`。脚手架拒绝覆盖已有文件。
   生成测试后，补充独立的业务预期。
3. 在 `Transform`、`BuildPrompt` / `FormatAnswer` 或 `Run` 中实现普通算法，返回值或
   `NodeResult`。配置、端口、模型只在 Spec 中声明。请求数据留在函数局部。
4. 格式化源码。构建 SDK、CLI 和现有测试程序。然后查询 Catalog。
   目录下所有 `.cpp`（含子目录）都会编入，草稿不要用 `.cpp` 后缀。无需修改中央节点列表或 Studio。
5. 编写 Pipeline 配置。执行 `validate` 和 `plan`。最后运行对应 Demo。新平台结构见
   [业务接入指南](../../doc/dev_guide/business_onboarding.md)，交付执行
   [CONTRIBUTING](../../CONTRIBUTING.md) 的统一门禁。

脚手架只提供接口骨架，不判断模型资源是否存在，也不替代业务验收。
默认 `parallel_safe=false`。确认业务函数与共享资源可并发使用后再改为 `true`。

## 本地快速验证

开发过程中只构建并运行节点测试 runner，把过滤器换成实际修改的套件：

```bash
cmake --build build --target edgeflow_test_nodes_runner -j 4
./build/edgeflow_test_nodes_runner --gtest_list_tests
./build/edgeflow_test_nodes_runner --gtest_filter='ScaffoldGeneratedNodeTest.*'
```

[Node 测试辅助](../../tests/support/node_probe.h)用已校验的 Plan 和 Session 初始化注册的 Node。
将请求输入放入新的 `AlgContext`。调用 Process 后，断言实际输出和 `(req_id, sub_id)`。
还需覆盖空输入、非法输入和失败行为。脚手架的 `--write-test` 在
`tests/unit/nodes/` 生成完整测试文件，节点 runner 自动收录，并通过 GoogleTest 原生发现生成 CTest 入口。发现检查核对实际编译用例都有执行归属。
修改注册或 Definition 后重新构建 `alg_pipeline_tool`，再用同一构建检查组合后的方案。
即使首次练习用的是 minimal 构建，最终门禁也会覆盖完整默认配置。交付按
[CONTRIBUTING](../../CONTRIBUTING.md#6-run-one-canonical-delivery-gate) 执行。

脚手架端口写作 `name:Batch`，派生关系可写为 `chunks:TextBatch:1:N:generate_sub_id`。
完整端口、模型、参数与 Control 契约统一见[Node 按需参考](../../doc/dev_guide/custom_node_concepts.md)，
其中包含[复杂参数与派生状态](../../doc/dev_guide/custom_node_concepts.md#复杂参数与派生状态)的生产例子。

## 完整参考样例

[prompt_guided_llm](prompt_guided_llm_node.cpp) 展示提示词构建、LLM 调用与代码围栏清理，
承担进阶参考：多输入上下文、配置化模板、生成参数与严格校验。需要哪部分再参考哪部分，
无需把整份解析逻辑复制到自己的节点。纯提示词组合仍可直接复用通用 Node。

两份方案使用**同一个**注册节点和已有 Adapter / Operator bridge：

| 方案 | 连线 | Profile |
| --- | --- | --- |
| [实体抽取](../../demo/fixtures/mock/pipeline_entity_extract_custom.json) | 输入文本 → custom → JSON 解析 → 实体输出 | `entity_extract_custom_mock` |
| [文档问答](../../demo/fixtures/mock/pipeline_doc_qa_custom.json) | 文档切片 + 问题 → custom；规则匹配 → 意图；统一文档输出 | `doc_qa_custom_mock` |

从仓库根目录运行（先完成默认构建）：

```bash
# 这些 smoke 配置使用测试模型，必须用带测试注册的工具校验
./build/alg_pipeline_tool_test validate demo/fixtures/mock/pipeline_entity_extract_custom.json
./build/alg_pipeline_tool_test plan demo/fixtures/mock/pipeline_entity_extract_custom.json
./build/alg_demo --profile entity_extract_custom_mock --output-dir results/custom-node

./build/alg_pipeline_tool_test validate demo/fixtures/mock/pipeline_doc_qa_custom.json
./build/alg_pipeline_tool_test plan demo/fixtures/mock/pipeline_doc_qa_custom.json
./build/alg_demo --profile doc_qa_custom_mock --output-dir results/custom-node
```

结果位于 `results/custom-node/<profile>/results.jsonl` 与 `summary.json`。两个 Profile
也纳入 `--suite smoke`。测试模型用于验证编排、来源和输出转换，不能据此评价模型效果。

样例统一使用 `{{input}}` / `{{context}}` 占位符。
它与 `text_template` 共用解析器：`{{name}}` 统一替换变量，允许变量名两侧的空格。
普通单个花括号（如 JSON）直接作为字面内容保留，例如 `{"question":"{{input}}"}`。
双花括号用于变量替换。TextTemplate 的主文本变量是 `primary`，本样例使用 `input`。
复制模板时，按目标 Node 调整变量名。两者的 `context` 含义相同。解析器拒绝未知变量。

使用 context 变量必须连入 context。按相同 `req_id` 合并片段，空上下文批次表示
没有参考内容。默认模板只插入 input，不隐式追加 context。
输入和上下文中的花括号保留原文，不参与模板解析。未知占位符、无效生成参数
和非法 stop_words 在原生校验与初始化时拒绝。

固定提示文字直接写入 `prompt_template`。system 消息通过节点 `system_prompt` 参数设置，
由 `GenerateOptions` 传入模型。
需要核对绑定模型的能力时，用 `ValidateModels` 在参数解析、模型绑定之后执行一次。

TextTemplate 引用的变量必须对应已连接的输入端口。某请求没有数据时填空。
聚合输入批次存在而某请求没有结果时，仍表示合法的空上下文。

模型失败、输出数量不符或 `(req_id, sub_id)` 不符时，节点返回错误且不发布结果。
业务降级应携带可辨识的状态，不能伪装成功。

## 复用与边界

领域算法按操作组织，通过 Pipeline 组合，平台转换留在 Adapter。共享纯函数有实际多个使用方时
再提取；通用 Node、Core 和 Engine 不依赖 custom 实现。职责与提升到通用 Node 的边界见
[架构指南](../../doc/architecture.md#能力节点层capability-nodes)。
