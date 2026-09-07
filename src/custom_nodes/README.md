# 自定义 Node

这里存放领域算法、特定前后处理和模型调用组合，与 `src/common_nodes/` 同属 Layer 3。
默认一个操作一个 `*_node.cpp`，直接放在本目录；文件名描述操作，不按项目或业务建立目录。
一个自定义 Node 可以被多个 Pipeline 使用，也可以与通用 Node 混合连线。

## 最短开发路径

1. 查询 `build/alg_pipeline_tool catalog --biz <biz_name>` 和 `describe-node`，优先复用
   已有操作；缺失的领域逻辑放在本目录，不要求先改造成通用算法。
2. 生成骨架并登记源码（也可以手写）：

   ```bash
   # 纯处理：默认生成保留来源的文本透传，在循环中替换为领域算法
   ./scripts/scaffold_custom_node.py CustomFilterNode --kind compute --add-to-cmake --generate-test

   # 前处理 → 绑定模型 → 后处理；使用 ModelBoundNode<ILlmModel>
   ./scripts/scaffold_custom_node.py DomainPromptNode --kind model -m llm --add-to-cmake --generate-test

   # 一对一保序推理，基类负责空批次、错误与来源检查
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
   Adapter；新结构走[业务接入指南](../../doc/BUSINESS_ONBOARDING.md)。交付执行
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

## 可运行的复用样例

[PromptGuidedLlmNode](prompt_guided_llm_node.cpp) 展示提示词构建、LLM 调用与代码围栏清理，
是编写方式样例。纯提示词组合仍可直接复用通用 Node，无需为此开发新算法。

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

样例模板使用 `{input}` / `{context}`，字面花括号用 `{{` / `}}` 转义。使用 `{context}`
必须连入 context；按相同 `req_id` 合并片段，空上下文批次表示没有参考内容。默认模板
只插入 input，不隐式追加 context。输入、上下文和 system_prompt 中的花括号保留原文。
未知占位符、无效生成参数和非法 stop_words 在原生校验与初始化时拒绝。

模型失败、输出数量不符或 `(req_id, sub_id)` 不符时，节点返回错误且不发布结果。
`fallback_text` 已删除并明确拒绝；业务降级应携带可辨识的状态，不能伪装成功。

## 复用与边界

- `custom` 表示代码归属，不限制复用；领域算法不必先泛化。
- 跨节点组合使用 Pipeline 连线，不直接调用另一 Node 绕过调度。重复的纯计算可提取
  小型辅助函数，确有多个使用方后再整理共享代码。
- 平台结构、拷贝、容量和生命周期属于 Layer 1，Node 使用内部值与逻辑端口。
- Node 调用声明绑定的模型能力；模型语义与厂商运行时分别由 Model / Backend 管理。
- 通用 Node、Core、Model / Backend 不依赖 custom 实现。经复用评审后可以提升到
  `common_nodes`，迁移时保留类型名和端口契约。
