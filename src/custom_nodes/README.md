# 自定义 Node

这里存放领域算法、特定前后处理和模型调用组合，与 `src/common_nodes/` 同属能力节点层。
默认一个操作一个 `*_node.cpp`，直接放在本目录；文件名描述操作，不按项目或业务建立目录。
一个自定义 Node 可以被多个 Pipeline 使用，也可以与通用 Node 混合连线。

## 第一次开发从这里开始

| 你现在要做的事 | 推荐入口 |
| --- | --- |
| 已有能力连线组成方案 | [Pipeline Studio](../../tools/pipeline_studio/README.md) |
| 写“前处理 → LLM → 后处理” | [第一个自定义 Node](../../doc/dev_guide/first_custom_node.md)：生成、修改两个函数、编译、连线、运行 |
| 为节点增加运行时参数更新 | [第一个 Control](../../doc/dev_guide/first_control.md)：声明、更新、失败保持、通过 Demo 下发 |
| 看不懂端口、来源编号、模型绑定等术语 | [五个概念说明](../../doc/dev_guide/custom_node_concepts.md)：结合一次请求解释用途和常见错误 |
| 需要多输入、配置化模板和完整校验 | 本页下方的[完整参考样例](#完整参考样例) |
| 对接新的平台输入输出结构 | [业务接入指南](../../doc/dev_guide/business_onboarding.md) |

入门使用[轻量 C++ 模板](../../dev_support/node_authoring/starter_llm_node.cpp)。
`--kind model -m llm` 直接从它生成代码；先填写 `BuildPrompt`、`FormatAnswer`，其余
固定结构继续负责端口、模型调用和来源检查。该模板不作为新内置节点加入生产 Catalog，
生成并登记到本目录后才成为你自己的操作。

## 通用开发步骤速查

1. 查询 `build/alg_pipeline_tool catalog --biz <biz_name>` 和 `describe-node`，优先复用
   已有操作；缺失的领域逻辑放在本目录，不要求先改造成通用算法。
2. 生成骨架并登记源码（也可以手写）：

   ```bash
   # 纯处理：默认生成保留来源的文本透传，在循环中替换为领域算法
   ./scripts/scaffold_custom_node.py CustomFilterNode --kind compute --add-to-cmake --generate-test

   # Control 入门：文本前缀更新，选择尚未使用的 custom 命令 ID
   ./scripts/scaffold_custom_node.py PrefixControlNode --control-id 1001 --add-to-cmake --generate-test

   # 推荐入门：填写 BuildPrompt 和 FormatAnswer；使用 ModelBoundNode<ILlmModel>
   ./scripts/scaffold_custom_node.py DomainPromptNode --kind model -m llm --add-to-cmake --generate-test

   # 已熟悉批处理接口后：一对一保序推理
   ./scripts/scaffold_custom_node.py FastAudioNode --kind unary_inference -m asr --add-to-cmake
   ```

   `--generate-test` **打印**可编译的注册测试片段，不会自动创建或登记测试文件。
   把片段及领域断言加入现有测试套件。`--dry-run` 仅打印；已有文件默认拒绝覆盖。
3. 在生成文件中实现请求内逻辑。`TraceableItem` 的载荷为 `.data`，保留来源的写法是
   `outputs.emplace_back(item.req_id, item.sub_id, new_value)`；模型句柄通过 `model()`
   使用。请求数据留在局部变量，成员只存配置或安全共享句柄。
4. 在同文件维护 `NodeDefinition`：逻辑端口、配置字段、模型能力及并发声明与实现一致。
   模型骨架要求在 Pipeline 配置中指定 `bind_model`。生成器默认 `parallel_safe=false`，
   审查节点的共享状态后再启用；`biz_names` 通常留空以便复用。
5. 格式化、重新构建 SDK/工具和现有测试 runner，再查询 Catalog。沿用同一注册路径，
   无需修改 Core、中央列表或 Studio。没加 `--add-to-cmake` 时，将文件名加入本目录
   [CMakeLists.txt](CMakeLists.txt) 的 `target_sources`。
6. 编排 Pipeline，运行原生 `validate`、`plan` 和匹配 Demo。已有外部结构直接复用
   Adapter；新结构走[业务接入指南](../../doc/dev_guide/business_onboarding.md)。交付执行
   [CONTRIBUTING](../../CONTRIBUTING.md) 中的统一门禁。

可用节点、模型、Backend 和参数以当前构建的 Catalog 为准。脚手架只提供编译期接口
骨架，不判断模型资源是否存在，也不替代 Validator 或领域测试。

### 端口与模型骨架边界

- 端口语法：`name:Batch` 或 `name:Batch:1:1:preserve`；例如
  `chunks:TextBatch:1:N:generate_sub_id` 会完整保留 `1:N`。
- compute 同类型、1:1/preserve 默认透传；异类型或其他数量关系只生成明确报错的
  待实现入口，作者必须实现转换后才能得到业务结果。
- model 支持当前五种模型能力，批类型必须与能力接口匹配。LLM / Embedding 使用
  对应 options 的默认值；如需配置化，在 Node 中解析并声明字段。
- unary_inference 适合 LLM、Embedding、ASR、Rerank 的 1:1 批处理。OCR 的
  `ImageRefBatch` 是独立容器，当前浅层 unary 支持类不能保留该类型，请使用
  `--kind model -m ocr`。脚本会拒绝不适用的组合。
- 自定义批类型应提供 `BlackboardTypeTraits`；生成代码通过编译期检查拒绝未知类型标识。

## 自定义参数的最小约定

在节点同一文件的 `NodeDefinition.config_fields` 声明字段，在 `InitNode` 或
`InitModelNode` 使用 `config.value<T>()` 读取；不需要参数宏或修改 Core/Studio。
字段类型、默认值、范围和枚举进入 Catalog；新增字段需重新编译，已有字段改值只需
重新创建 Pipeline/实例加载配置。运行中的更新仍需显式实现 Control。

同一字段的默认值使用节点内共享常量，例如：

```cpp
constexpr double kDefaultThreshold = 0.8;
// 添加到 Definition.config_fields：
ConfigFieldDefinition{"threshold", ConfigValueKind::kNumber,
                      false, kDefaultThreshold, 0.0, 1.0};
// 在初始化中读取：
threshold_ = config.value<double>("threshold", kDefaultThreshold);
```

只有缺失字段才使用默认值；类型、范围或字段组合错误应拒绝，不能静默回退。
Validator 在物化前检查声明，初始化仍保留防御校验。复杂配置确实重复使用时，
将解析和语义检查集中为节点自己的函数，供 `validate_config`、初始化以及适用的
Control 路径复用。可直接参考 [Control 模板](../../dev_support/node_authoring/starter_control_node.cpp)
的 `PrefixConfigFields` / `ReadPrefix`，字段 `semantic` 写明用途、单位和嵌套结构，Catalog
与 Studio 会显示同一说明。普通参数保存在节点配置成员，请求数据继续通过端口传递。

涉及长度时写清单位：TextChunk 的 `chunk_size/overlap` 按 Unicode 码点计数，
TextTemplate 的 `max_length` 是 UTF-8 字节预算，生成的 `max_tokens` 是 token 数。
这些不同用途的参数不需要统一数值或名称。嵌套参数通过 JSON 配置，字段结构以节点
声明及实际校验为准，不在前端维护独立规则。

## 完整参考样例

[PromptGuidedLlmNode](prompt_guided_llm_node.cpp) 展示提示词构建、LLM 调用与代码围栏清理，
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

样例显式设置 `template_syntax: "standard"`，使用 `{{input}}` / `{{context}}`。
它与 `TextTemplateNode` 共用解析器：`{{name}}` 与兼容写法 `{name}` 都替换变量，
允许变量名两侧的空格；JSON 的普通花括号直接保留，例如
`{"question":"{{input}}"}`。双花括号不再表示字面转义。变量名仍由各 Node 声明：
TextTemplate 的主文本叫 `primary`，本样例叫 `input`；复制模板时应对应替换，
两者共有的 `context` 保持相同语义，未知变量会报错。

已有 PromptGuided 模板按以下规则迁移，避免旧模板的字面内容变成变量：

| `template_syntax` | 语义与迁移方式 |
| --- | --- |
| `standard` | 推荐用于新方案；单双括号均为变量，JSON 花括号直接书写。 |
| `auto`（默认） | 保留原先没有双花括号的模板行为；发现 `{{` 或 `}}` 就拒绝，并提示显式选择语法，不猜测含义。默认 `{input}` 无需迁移。 |
| `legacy` | 明确保留旧模板：`{input}` / `{context}` 为变量，`{{` / `}}` 为字面花括号；例如 `{{input}}` 输出字面 `{input}`。 |

旧 JSON 模板如 `{{"question":"{input}"}}` 可先设置 `legacy` 保持输出；迁移到
`standard` 时改成 `{"question":"{{input}}"}`。不要只切换模式而保留旧转义文本。
两种显式模式都由原生 Validator 与 Node 初始化使用同一条校验路径。

使用 context 变量必须连入 context；按相同 `req_id` 合并片段，空上下文批次表示
没有参考内容。默认模板只插入 input，不隐式追加 context。输入、上下文和
prompt_prefix 中的花括号保留原文，不再作为模板解析。未知占位符、无效生成参数
和非法 stop_words 在原生校验与初始化时拒绝。

`prompt_prefix` 是普通输入文本前缀，非空时在模板前追加一行；旧的节点字段
`system_prompt` 已改名并拒绝使用。模型的 `model_config.system_prompt` 仍表示真正的
system 消息，不能用节点前缀替代该角色。

TextTemplate 的 `missing_variable_policy` 也适用于内置变量。`fail` 会拒绝未连接的
引用或缺失的主输入样本；需要保留占位符或填空时显式使用 `preserve/empty`。
聚合输入批次存在而某请求没有结果时，仍表示合法的空上下文。

模型失败、输出数量不符或 `(req_id, sub_id)` 不符时，节点返回错误且不发布结果。
`fallback_text` 已删除并明确拒绝；业务降级应携带可辨识的状态，不能伪装成功。

## 复用与边界

- `custom` 表示代码归属，不限制复用；领域算法不必先泛化。
- 跨节点组合使用 Pipeline 连线，不直接调用另一 Node 绕过调度。重复的纯计算可提取
  小型辅助函数，确有多个使用方后再整理共享代码。
- 平台结构、拷贝、容量和生命周期属于接入适配层，Node 使用内部值与逻辑端口。
- Node 调用声明绑定的模型能力；模型语义与厂商运行时分别由 Model / Backend 管理。
- 通用 Node、Core、Model / Backend 不依赖 custom 实现。经复用评审后可以提升到
  `common_nodes`，迁移时保留类型名和端口契约。
