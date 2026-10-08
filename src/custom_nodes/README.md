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

1. 查询 `build/alg_pipeline_tool catalog --io-binding <biz_name>` 和 `describe-node`，确认已有能力。
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
  `OutputsOf` / `Produced` 声明多输出；`PortFlow` 提供数量、来源、生命周期及其配置引用。
  拆分、聚合等派生输出的正确性由算法及测试保证，声明不会自动证明这些关系。
- `ModelsOf` 中的 `Model` 根据成员类型绑定五种模型能力：`LlmCall`、`EmbeddingCall`、
  `AsrCall`、`OcrCall`、`RerankCall`。调用门面处理空批次、模型错误及保序校验。
  模型骨架要求批类型匹配能力接口；包括 OCR 的 `ImageRefBatch`。
- 同类型、保序的 compute 骨架可透传；异类型或派生输出保留明确失败的待实现入口。
  新内部批类型提供 `BlackboardTypeTraits`；平台 DTO 不进入 Node。

## 自定义参数的最小约定

在 `Parameters<Params>` 中用 `Field("name", &Params::member)` 声明成员，显式选择
`.Required()` 或 `.Default(value)`，再写范围、枚举及说明。字段同时用于预检、初始化和 Catalog；
业务函数收到普通参数结构，不再从 JSON 重复读取。跨字段检查用 `Validate`，连线约束用
`ValidateBindings`。普通字段 Control 使用 `WithControls`（payload 格式由字段声明生成，至少给出
一个受控参数），参考 [Control 练习](../../doc/dev_guide/first_control.md)。

成员类型除标量外可以是 `std::vector<T>`、`std::map<std::string, T>` 和 `nlohmann::json`（任意非 null
JSON 值）。元素是结构体时用 `.Items(Parameters<E>)` 声明元素字段；元素的诊断路径继续写键名或
下标。多个节点共享的参数组用 `.Include(&Params::member, group)` 平铺并入，LLM 节点的生成参数是
`GenerateParameters()`（[generate_parameters.h](../../include/nodes/generate_parameters.h)），
`max_tokens` 默认 128，示例见 [PromptGuidedLlmNode](prompt_guided_llm_node.cpp)。

字段说明应明确单位：TextChunk 按 Unicode 码点切分，TextTemplate 的长度是 UTF-8 字节预算，
生成的 `max_tokens` 是 token 数。类型、范围和字段组合错误应拒绝，不静默改用默认值。

### 参数复杂时，使用普通结构和 Prepare

数组、映射、任意 JSON 值用 `Field` 直接绑定到 `std::vector<T>`、`std::map<std::string, T>`、
`nlohmann::json` 成员；对象数组用 `.Items(Parameters<E>)` 声明元素字段，元素有自己的默认值、
`Prepare` 和 `Validate`。参数结构体持有自身的字符串和容器，不保存 JSON 指针。
派生状态（编译后的正则、模板记号、备用值的序列化文本等）放在不对应配置项的成员中，
由 `Prepare` 构建，再执行语义与连线校验；预检、初始化和 Control 共享同一规则。

参考 [StructuredJsonParseNode](../common_nodes/structured_json_parse_node.cpp) 和
[PromptGuidedLlmNode](prompt_guided_llm_node.cpp)。Control 用 `.WithControls({ReplaceFields(...)})`
选择要整体替换的参数，参考 [TextTemplateNode](../common_nodes/text_template_node.cpp) 和
[TextRuleMatchNode](../common_nodes/text_rule_match_node.cpp)：框架串行处理更新，更新后重新执行
`Prepare` 与校验，失败保持旧值，每次请求读取一次一致快照。

使用 context 变量必须连入 context；按相同 `req_id` 合并片段，空上下文批次表示
没有参考内容。默认模板只插入 input，不隐式追加 context。输入、上下文和
prompt_prefix 中的花括号保留原文，不再作为模板解析。未知占位符、无效生成参数
和非法 stop_words 在原生校验与初始化时拒绝。

`prompt_prefix` 是普通输入文本前缀，非空时在模板前追加一行。真正的 system 消息由模型的
`model_config.system_prompt` 设置，不能用节点前缀替代该角色。

TextTemplate 的变量只能是已连接的输入端口名（`primary`、`context`、`matches`、`document`）；
未知变量或引用未连接的输入在 `Prepare` 中拒绝。输入已连接但某个请求没有数据时，该变量渲染为空字符串。

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
