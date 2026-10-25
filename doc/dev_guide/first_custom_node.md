# 第一个自定义 Node：先写逐条处理，再接入模型

所有 Node 使用同一结构。带模型的 Node 只是多声明了模型，并在处理过程中调用它。
本练习分两部分：先写一个不带模型的文本处理节点，再用同样的结构写一个调用 LLM 的节点。
两部分都使用现有 Adapter 和测试模型，无需下载权重。
先按 [README](../../README.md#快速开始)完成构建，所有命令在仓库根目录执行。

如果已有节点能满足需求，直接配置 Pipeline。如需自动生成 LLM 节点的方案和验证命令，
使用[文本 LLM Recipe](recipe_text_llm_node.md)。

## 1. 一个 Node 由哪些部分组成

| 部分 | 作用 | 是否必需 |
| --- | --- | --- |
| `Inputs` | 声明要读哪些输入 | 必需 |
| `Params` | 声明配置项 | 需要配置时 |
| `Models` | 声明要调用的模型 | 需要模型时 |
| `Run` | 业务逻辑：读取输入，按需调用模型，返回结果 | 必需 |
| `Spec` / `REGISTER_FUNCTION_NODE` | 把上面的声明登记给框架，生成 Definition | 必需 |

业务逻辑写在 `Run` 中。逻辑复杂时再拆成普通辅助函数，函数名不受框架约束。
脚手架生成的 `Transform`、`BuildPrompt`、`FormatAnswer` 只是起步写法；
Embedding 等模型节点生成时直接在 `Run` 中调用模型，没有这些函数。

框架负责模型加载、调度、数据传递和来源编号。外部请求与响应由 Adapter 转换。

## 2. 第一部分：不带模型的节点

本节写一个文本归一化节点：把“开机检查”改写成“自检”，让已有的规则节点能够命中。

### 生成

```bash
./tools/scaffold_custom_node.py my_text_normalize --write-test
```

脚手架生成 `src/custom_nodes/my_text_normalize_node.cpp` 和
`tests/unit/nodes/test_my_text_normalize_node.cpp`，并拒绝覆盖已有文件。

### 修改业务逻辑

脚手架生成的 `Run` 通过 `MapPayloads` 逐条调用辅助函数 `Transform`。在源码中找到 `Transform`，将它换成：

```cpp
std::string Transform(const std::string& text) {
  const std::string from = "开机检查";
  const std::string to = "自检";
  std::string result = text;
  for (auto pos = result.find(from); pos != std::string::npos;
       pos = result.find(from, pos + to.size())) {
    result.replace(pos, from.size(), to);
  }
  return result;
}
```

`Transform` 只处理一条文本，`MapPayloads` 保留每条结果的来源编号。
需要修改输入时，构造新文本，不要修改共享的只读输入。

在生成的测试中，把输入 `"example"` 改为 `"请做一次开机检查"`，
把输出断言改为 `"请做一次自检"`。

### 编译和测试

源码格式化需要 clang-format 18。

```bash
./scripts/format.sh
cmake --build build --target alg_sdk alg_pipeline_tool alg_pipeline_tool_test alg_demo edgeflow_test_nodes_runner -j 4
./build/edgeflow_test_nodes_runner --gtest_filter='ScaffoldGeneratedNodeTest.my_text_normalize_*'
./build/alg_pipeline_tool describe-node my_text_normalize
```

测试应通过。Catalog 应显示 `my_text_normalize`，输入输出都是 `TextBatch`，`model_dependencies` 为空。

### 接入方案并运行

```bash
mkdir -p build/first_node
```

保存以下文件为 `build/first_node/pipeline.json`：

```json
{
  "io": {"input": [{"type":"keyword_in","name":"keyword_match"}],
         "output": [{"type":"json_out","name":"keyword_match",
                     "inputs":{"matches":"matcher.matches"}}]},
  "models": [],
  "pipeline": [
    {
      "type": "my_text_normalize",
      "name": "normalize",
      "inputs": {"input": "input.sentence_text"}
    },
    {
      "type": "text_rule_match",
      "name": "matcher",
      "params": {"categories": {"SYSTEM_INIT": ["初始化", "自检"]}},
      "inputs": {"text": "normalize.output"}
    }
  ]
}
```

`"inputs":{"input":"input.sentence_text"}` 将节点的 `input` 端口连接到输入转换器。
`matcher` 通过 `normalize.output` 读取归一化结果，Validator 据此推导执行顺序。

同目录保存 `pipeline.conf`，内容为 `{"pipe_path": "pipeline.json"}`。
再创建 `input.txt`，写入两行：`请做一次开机检查` 和 `今天天气不错`。

```bash
./build/alg_pipeline_tool validate build/first_node/pipeline.json
./build/alg_pipeline_tool plan build/first_node/pipeline.json
./build/alg_demo --config build/first_node/pipeline.conf --dataset build/first_node/input.txt --output-dir build/first_node/results
```

查看 `build/first_node/results/pipeline/results.jsonl`：两条记录的 `status` 都为 0。
第一条 `is_hit` 为 1，`intent` 为 `SYSTEM_INIT`，`matched_word` 为 `自检`；第二条 `is_hit` 为 0。
原句只含“开机检查”，命中来自新节点的归一化。

## 3. 第二部分：同样的结构加一个 LLM

本节节点依次处理输入、调用模型、处理输出。生成时加上模型能力：

```bash
./tools/scaffold_custom_node.py my_business_llm -m llm --write-test
```

对照第一部分的源码，结构相同，多出的只有三处：

| 位置 | 变化 |
| --- | --- |
| `Models` | 新增 `LlmCall model;`，Spec 中用 `ModelsOf` 声明，配置里通过 `bind_model` 选择模型 |
| 辅助函数 | `Transform` 换成调用模型前的 `BuildPrompt` 和调用后的 `FormatAnswer` |
| `Run` / Spec | 整批调用一次 `Generate`；`GenerateParameters()` 从配置读取 `max_tokens` 等生成参数 |

在已有节点上手动加入这些声明，效果与 `-m llm` 生成的结果相同。

### 修改两个辅助函数

将 `BuildPrompt` 换成下面的实现，给模型输入加上任务指令：

```cpp
std::string BuildPrompt(const std::string& text) {
  return "你是实体与名词抽取助手。请返回 JSON。\n" + text;
}
```

将 `FormatAnswer` 换成下面的实现，去掉回答末尾的换行：

```cpp
std::string FormatAnswer(const std::string& text) {
  std::string answer = text;
  while (!answer.empty() && answer.back() == '\n') answer.pop_back();
  return answer;
}
```

保留短语 `实体与名词抽取助手`。测试模型据此返回后续节点需要的 JSON。
该结果用于验证接入流程。提示词效果需要真实模型验证。

生成的测试用 [`RecordingLlm`](../../tests/support/recording_models.h) 作为固定回答的模型。
把 `model->replies` 中的 `"fixture text"` 改为 `"fixture text\n"`，
输出断言仍保留 `"fixture text"`，用于检查刚添加的末尾换行清理。
然后按第一部分的命令格式化、编译，并把测试过滤器换成 `my_business_llm_*`。

### 复用已有方案和 Adapter

如果练习文件尚不存在，复制以下配置。已有同名文件时，继续编辑该文件。

```bash
cp demo/fixtures/mock/pipeline_entity_extract_custom.json demo/fixtures/mock/pipeline_first_node.json
cp demo/fixtures/mock/pipeline_entity_extract_custom.conf demo/fixtures/mock/pipeline_first_node.conf
```

编辑 `pipeline_first_node.json` 中 `name` 为 `generate_entities` 的节点，做两处修改：

- `type` 改成 `my_business_llm`。
- 将整个 `params` 对象替换为以下内容。其余生成参数使用默认值。

```json
{"bind_model": "entity_llm"}
```

保留 `inputs`、模型、JSON 解析节点和 `io` 连线。
将 `pipeline_first_node.conf` 的 `pipe_path` 改为 `pipeline_first_node.json`。

```mermaid
flowchart LR
    A[平台输入文本] --> B[已有 Adapter 解包]
    B --> C[my_business_llm: 前处理 → 模型 → 后处理]
    C --> D[已有 JSON 解析节点]
    D --> E[已有 Adapter 打包实体结果]
```

```bash
./build/alg_pipeline_tool_test validate demo/fixtures/mock/pipeline_first_node.json
./build/alg_pipeline_tool_test plan demo/fixtures/mock/pipeline_first_node.json
./build/alg_demo --config demo/fixtures/mock/pipeline_first_node.conf --dataset data/corpus_entity_extract.txt --output-dir results/first-node
```

这些配置使用测试模型，校验要用带测试注册的 `alg_pipeline_tool_test`。
`alg_pipeline_tool` 用于查看生产注册，也能发现刚编译的自定义节点。

查看 `results/first-node/pipeline_first_node/results.jsonl`：第一条记录应有 `status: 0`，
以及 `output.nouns: ["mock entity"]`。`summary.json` 应显示一条成功、
零条失败。回答来自测试模型。

替换真实模型时，按目标构建的 Catalog 更新 `models` 和资产路径，节点的 `bind_model`
引用相应 `name`。再用自己的输入与期望结果验收，不能把测试模型回答作为效果指标。

## 4. 从练习进入真实开发

在生成的测试中补充自己的业务预期，检查结果与来源，并覆盖空输入、非法输入和模型失败。
`RecordingLlm` 等替身可通过 `fail_call` 让指定调用失败。参考
[LLM Node 测试](../../tests/unit/nodes/test_llm_generate_node.cpp)。
交付前执行 `./scripts/run_all_tests.sh`，流程见 [CONTRIBUTING](../../CONTRIBUTING.md)。

| 接下来遇到的问题 | 去哪里看 |
| --- | --- |
| 端口、编号、模型绑定、Definition、并发是什么意思 | [按需参考](custom_node_concepts.md) |
| 需要配置项或运行时更新 | [Control 练习](first_control.md)，生成的节点带一个 `Params` 示例 |
| 其它模型能力 | `-m embedding`、`-m asr`、`-m ocr`、`-m rerank`，结构相同 |
| 需要多个输入或条件重试 | [多输入 starter](../../dev_support/node_authoring/starter_batch_node.cpp) |
| 需要 LLM 与 Embedding 两种能力 | [多模型 starter](../../dev_support/node_authoring/starter_multi_model_node.cpp) |
| 需要配置化模板或复杂后处理 | [复杂算法的组织与现有辅助函数](custom_node_concepts.md#复杂算法仍按普通-c-函数组织) |
| 需要接入全新的平台结构 | [业务接入指南](business_onboarding.md) |
| 需要新增模型语义或硬件后端 | [开发者扩展指南](../developer_guide.md) |

按操作命名节点，例如“识别实体”或“核对条件”。不同方案可复用同一节点。
