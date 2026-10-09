# 自定义 Node

这里存放领域算法、特定前后处理和模型调用组合，与 `src/common_nodes/` 同属能力节点层。
按操作组织 `*_node.cpp`，不按业务建目录；同一个 Node 可以被多个 Pipeline 复用。

## 第一次开发从这里开始

已有能力能通过连线完成时，直接使用 Pipeline。需要新增算法时，跟随
[第一个自定义 Node](../../doc/dev_guide/first_custom_node.md) 完成生成、修改、编译和运行。
[按需参考](../../doc/dev_guide/custom_node_concepts.md)解释端口、来源、模型、Catalog 和并发。

所有 Node 使用同一结构（`Inputs`、可选的 `Params` 与 `Models`、`Run`、`Spec`），通过
`REGISTER_FUNCTION_NODE` 从 Spec 生成绑定、Definition 和执行包装。`NodeBase` 是框架内部运行机制，
不是业务作者的另一个入口。

## 通用开发步骤速查

1. 查询 `build/alg_pipeline_tool catalog` 和 `describe-node`，确认已有能力。
2. 生成普通函数、Spec 和对应测试：

   ```bash
   ./tools/scaffold_custom_node.py CustomFilterNode --kind compute --write-test
   ./tools/scaffold_custom_node.py DomainPromptNode --kind model -m llm --write-test
   ./tools/scaffold_custom_node.py FastAudioNode --kind model -m asr --write-test
   ./tools/scaffold_custom_node.py PrefixControlNode --control-id 1001 --write-test
   ```

   `--dry-run` 查看计划；已有文件默认拒绝覆盖。生成测试后补充独立业务期望。
3. 在 `Transform`、`BuildPrompt` / `FormatAnswer` 或 `Run` 中实现普通算法，返回值或
   `NodeResult`。配置、端口、模型只在 Spec 中声明；请求数据留在函数局部。
4. 格式化、构建 SDK、CLI 和现有测试 runner，再查询 Catalog。
   目录下所有 `.cpp`（含子目录）都会编入，草稿不要用 `.cpp` 后缀。无需修改中央节点列表或 Studio。
5. 编排 Pipeline，执行原生 `validate`、`plan` 与匹配 Demo。新平台结构走
   [业务接入指南](../../doc/dev_guide/business_onboarding.md)，交付执行
   [CONTRIBUTING](../../CONTRIBUTING.md) 的统一门禁。

脚手架只提供接口骨架，不判断模型资源是否存在，也不替代业务验收。
默认 `parallel_safe=false`；确认业务函数与共享资源可并发使用后再改为 `true`。

## 本地快速验证

开发过程中只构建并运行节点测试 runner，把过滤器换成实际修改的套件：

```bash
cmake --build build --target edgeflow_test_nodes_runner -j 4
./build/edgeflow_test_nodes_runner --gtest_list_tests
./build/edgeflow_test_nodes_runner --gtest_filter='CommonNodesTest.*'
```

[Node 测试辅助](../../tests/support/node_test_utils.h)用已校验的 Plan 和 Session 初始化注册的 Node。
把请求输入放进新的 `AlgContext`，调用 Process，断言实际输出和 `(req_id, sub_id)`，不要只检查
工厂能否创建；还要覆盖空输入、非法输入和失败行为。脚手架的 `--write-test` 在
`tests/unit/nodes/` 生成完整测试文件，节点 runner 自动收录，并由现有的 `CustomNodeCatalogTest`
过滤器覆盖；自己新建的套件名需要在 [tests/RuntimeTests.cmake](../../tests/RuntimeTests.cmake) 中加入过滤器。
修改注册或 Definition 后重新构建 `alg_pipeline_tool`，再用同一构建检查组合后的方案。
即使首次练习用的是 minimal 构建，最终门禁也会覆盖完整默认配置；交付按
[CONTRIBUTING](../../CONTRIBUTING.md#6-run-one-canonical-delivery-gate) 执行。

### 端口与模型

- 端口语法为 `name:Batch` 或 `name:Batch:1:1:preserve`；
  `chunks:TextBatch:1:N:generate_sub_id` 完整表达拆分契约。
- `InputsOf` 绑定普通输入结构的只读批次指针。`Required` 要求输入，`Optional` 允许不连接；
  连接后仍允许当前请求缺值的业务，显式使用 `OptionalValue`。
- `PreservedOutput` 声明 anchor，框架检查数量、顺序和来源。`ProducedBatch` 声明派生单输出，
  `OutputsOf` / `Produced` 声明多输出；`PortFlow` 提供数量、来源、生命周期；`FollowLifetime("text")` 声明输出跟随实际输入生命周期。
  拆分、聚合等派生输出的正确性由算法及测试保证，声明不会自动证明这些关系。
- `ModelsOf` 中的 `Model` 根据成员类型绑定五种模型能力：`LlmCall`、`EmbeddingCall`、
  `AsrCall`、`OcrCall`、`RerankCall`。调用门面处理空批次、模型错误及保序校验。
  绑定写作 `Model("generator", "bind_model", &Models::generator)`，只传三个参数；框架按成员
  能力生成引用 `models[].name` 的说明。模型骨架要求批类型匹配能力接口；包括 OCR 的 `ImageRefBatch`。
- 同类型、保序的 compute 骨架可透传；异类型或派生输出保留明确失败的待实现入口。
  新内部批类型提供 `BlackboardTypeTraits`；平台 DTO 不进入 Node。

## 自定义参数的最小约定

在 `Parameters<Params>` 中用 `Field("name", &Params::member)` 声明成员，显式选择
`.Required()` 或 `.Default(value)`，再写范围、枚举及说明。字段同时用于预检、初始化和 Catalog；
业务函数收到普通参数结构，不再从 JSON 重复读取。跨字段检查用 `Validate`，连线约束用
`ValidateBindings`。数组用 `std::vector<T>`，映射用 `std::map<std::string, T>`；结构体元素用
`.Items(Parameters<Element>{...})`。`Range` / `Enum` 约束标量叶子，错误路径指出下标或映射键；
`nlohmann::json` 参数接受非 null JSON 值。完整声明见
[参数与 Definition](../../doc/dev_guide/custom_node_concepts.md#3-definition让连线工具和运行器看懂你的操作)。
参数 Control 使用 `WithControls` / `ReplaceFields`：payload 至少提供一个受控字段，只替换
提供的字段，容器整体替换；重跑 `Prepare` / `Validate`，失败保持旧快照。参考
[Control 练习](../../doc/dev_guide/first_control.md)。

LLM 生成参数复用 [`GenerateParameters()`](../../include/nodes/generate_parameters.h)，默认
`max_tokens = 128`。有自有字段时用
`Parameters<Params>{Field(...)}.Include(&Params::generation, GenerateParameters())`；生成字段
在 JSON 中平铺，重名报错，被并入组的 `Prepare` / `Validate` 先执行。它们也是普通 `Field`，
可以加入 `WithControls`。

字段说明应明确单位：TextChunk 按 Unicode 码点切分，TextTemplate 的长度是 UTF-8 字节预算，
生成的 `max_tokens` 是 token 数。类型、范围和字段组合错误应拒绝，不静默改用默认值。

### 复杂参数与派生状态

参数通过 `Field` 声明，结构体元素用 `Items`，共享参数组用 `Include`。
字段赋值后在 `Prepare` 中构建持有自身数据的派生状态，再执行语义与连线校验。
字段 Control 会复用同一流程重建并校验候选，失败保持旧值，每次请求读取一次一致快照。

[text_template](../common_nodes/text_template_node.cpp) 在 `Prepare` 中编译模板，
只允许已连接的 `primary`、`context`、`matches`、`document` 变量；未知变量或引用未连接端口
在此拒绝。连接了端口但某请求没有数据时，该变量为空字符串。`update_prompt` 只替换 `template`；
Operator 的 `prompt_id` 仍由接入层检查长度，但不转发到节点。

[text_rule_match](../common_nodes/text_rule_match_node.cpp) 用映射声明 `categories`，
用 `Field("rules", ...).Items(...)` 声明规则字段，并在元素的 `Prepare` 中编译正则。
`update_rules` 整体替换给出的 `categories` 或 `rules`，省略的项保持原值。
[structured_json_parse](../common_nodes/structured_json_parse_node.cpp) 的 `fallback` 直接填写
非 null JSON 值，例如 `{"fallback":{"category":"NONE"}}`。
[text_corpus_source](../common_nodes/text_corpus_source_node.cpp) 没有输入端口，`corpus` 必填，
可以是空数组。生成参数的 `Include` 参考 [prompt_guided_llm](prompt_guided_llm_node.cpp)。

会话缓存显式向 `Run` 注入 `const SessionResources&`，通过 `GetOrCreateResult<T>`
调用返回 `NodeResult<T>` 的工厂，通过 `GetModelRevision` 取得缓存身份所需的模型版本；参考
[text_embedding](../common_nodes/text_embedding_node.cpp)。不要将请求输入指针保存在缓存中。

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

结果位于 `results/custom-node/<profile>/results.jsonl` 与 `summary.json`；两个 Profile
也纳入 `--suite smoke`。测试模型用于验证编排、来源和输出转换，不能据此评价模型效果。

样例统一使用 `{{input}}` / `{{context}}` 占位符。
它与 `text_template` 共用解析器：`{{name}}` 统一替换变量，允许变量名两侧的空格；
普通单个花括号（如 JSON）直接作为字面内容保留，例如 `{"question":"{{input}}"}`。
双花括号不再表示字面转义。变量名由各 Node 声明：TextTemplate 的主文本叫 `primary`，
本样例叫 `input`；复制模板时应对应替换，两者共有的 `context` 保持相同语义，未知变量会报错。

使用 context 变量必须连入 context；按相同 `req_id` 合并片段，空上下文批次表示
没有参考内容。默认模板只插入 input，不隐式追加 context。
输入和上下文中的花括号保留原文，不再作为模板解析。未知占位符、无效生成参数
和非法 stop_words 在原生校验与初始化时拒绝。

固定提示文字直接写入 `prompt_template`。system 消息通过节点 `system_prompt` 参数设置，
由 `GenerateOptions` 传入模型。
需要核对绑定模型的能力时，用 `ValidateModels` 在参数解析、模型绑定之后执行一次。

TextTemplate 引用的变量必须对应已连接的输入端口；某请求没有数据时填空。
聚合输入批次存在而某请求没有结果时，仍表示合法的空上下文。

模型失败、输出数量不符或 `(req_id, sub_id)` 不符时，节点返回错误且不发布结果。
业务降级应携带可辨识的状态，不能伪装成功。

## 复用与边界

- `custom` 表示代码归属，不限制复用；领域算法不必先泛化。
- 跨节点组合使用 Pipeline 连线，不直接调用另一 Node 绕过调度。重复的纯计算可提取
  小型辅助函数，确有多个使用方后再整理共享代码。
- 平台结构、拷贝、容量和生命周期属于接入适配层，Node 使用内部值与逻辑端口。
- Node 调用声明绑定的模型能力；模型语义与厂商运行时分别由 Model / Backend 管理。
- 通用 Node、Core、Model / Backend 不依赖 custom 实现。经复用评审后可以提升到
  `common_nodes`，迁移时保留类型名和端口契约。
