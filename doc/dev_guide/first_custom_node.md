# 第一个自定义 Node：从文本处理逐步加上参数和模型

本练习始终修改同一个 `my_text_normalize` 节点：先把“开机检查”改写为“自检”，
再让替换文本可配置，最后调用 LLM 整理整句话。源码文件、节点类型和输入输出端口始终不变。

| 阶段 | 要实现的行为 | 在同一份源码上增加什么 |
| --- | --- | --- |
| 文本处理 | 固定词语替换 | `Inputs`、`Run`、`Spec` |
| 普通配置 | 配置替换后的词语 | `Params` 与字段声明 |
| 调用模型 | 词语替换后，让 LLM 整理表达 | `Models` 与生成调用 |

`Run` 写处理流程，`Spec` 把输入、输出和可选的参数、模型登记给框架。
逻辑较长时可以拆成普通辅助函数；`Transform` 等函数名由你决定。
逐项处理使用 `MapPayloads`，框架保留数量、顺序和来源编号。

先按 [README](../../README.md#快速开始)完成构建，下面的命令都在仓库根目录执行。
练习使用现有 Adapter 和测试模型，无需下载权重。
已有节点能满足需求时，直接配置 Pipeline；需要一次生成 LLM 节点及配套方案时，可用
[文本 LLM Recipe](recipe_text_llm_node.md)。

## 1. 先完成文本处理

只生成一次骨架：

```bash
./tools/scaffold_custom_node.py my_text_normalize --write-test
```

得到 `src/custom_nodes/my_text_normalize_node.cpp` 和
`tests/unit/nodes/test_my_text_normalize_node.cpp`。已有文件会被保留；如果已做过练习，继续编辑它们。

在源码中把 `Transform` 替换为：

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

其余生成代码保持原样。`Run` 用 `MapPayloads` 对每条文本执行 `Transform`，
`Spec` 声明 `input` 和 `output` 都是文本批次，输入一项就返回一项。
修改文本时创建新值，不修改共享的只读输入。

在生成的测试中，将输入 `"example"` 改成 `"请做一次开机检查"`，
将 `item.data` 的期望值改成 `"请做一次自检"`。保留数量与来源编号的断言。

每完成一个阶段，都用下面的命令格式化、编译和运行这个测试：

```bash
./scripts/format.sh
cmake --build build --target edgeflow_test_nodes_runner alg_pipeline_tool -j 4
./build/edgeflow_test_nodes_runner --gtest_filter='ScaffoldGeneratedNodeTest.my_text_normalize_*'
./build/alg_pipeline_tool describe-node my_text_normalize
```

格式化需要 clang-format 18。此时测试应通过，Catalog 的输入输出均为 `TextBatch`，
`model_dependencies` 为空。

## 2. 为同一个节点增加普通配置

现在希望不同方案能选择替换后的词语。在 `Inputs` 后添加：

```cpp
struct Params {
  std::string replacement;
};
```

将 `Transform` 的签名改为
`std::string Transform(const std::string& text, const Params& params)`，
把函数体中的 `const std::string to = "自检";` 改成
`const std::string& to = params.replacement;`。其余替换逻辑保持不变。

用下面的代码替换原来的 `Run` 和 `Spec`，保留原来的注册语句：

```cpp
NodeResult<TextBatch> Run(const Inputs& inputs, const Params& params) {
  return MapPayloads(*inputs.input, [&params](const std::string& text) {
    return Transform(text, params);
  });
}

auto Spec() {
  return MakeNodeSpec(
      InputsOf<Inputs>{Required("input", &Inputs::input)},
      PreservedOutput<TextBatch>("output", "input"),
      Parameters<Params>{Field("replacement", &Params::replacement)
                             .Default("自检")
                             .Description("替换开机检查的文本")},
      &Run)
      .Description("替换操作说明中的固定词语");
}
```

框架从 Pipeline 节点的 `params` 读取 `replacement`，未填写时使用 `自检`。
这些配置在创建节点时生效。只有需要运行时更新，才添加 [Control](first_control.md)。

先运行原来的测试，确认默认值仍然生效。再把测试中 `probe.Init` 的
`nlohmann::json::object()` 改为 `nlohmann::json{{"replacement", "启动自检"}}`，
将输出期望值改为 `"请做一次启动自检"`，重新运行测试。
Catalog 现在应显示节点说明和 `replacement`，仍无模型依赖。

## 3. 在同一个节点中调用 LLM

继续编辑同一个文件。保留 `Inputs`、`Transform` 和注册语句。
给现有 `Params` 增加生成参数，并在它后面添加 `Models`：

```cpp
struct Params {
  std::string replacement;
  GenerateOptions generation;
};

struct Models {
  LlmCall model;
};
```

用下面的代码替换 `Run` 和 `Spec`。处理顺序是词语替换、构造提示词、调用模型、清理回答：

```cpp
NodeResult<TextBatch> Run(const Inputs& inputs, const Params& params,
                          const Models& models) {
  auto prompts = MapPayloads(*inputs.input, [&params](const std::string& text) {
    return "将下面的操作说明改写为简洁的规范指令，只输出改写结果：\n" +
           Transform(text, params);
  });
  auto answers = models.model.Generate(prompts, params.generation);
  if (!answers.ok()) return answers;
  return MapPayloads(answers.value(), [](const std::string& text) {
    std::string answer = text;
    while (!answer.empty() && answer.back() == '\n') answer.pop_back();
    return answer;
  });
}

auto Spec() {
  return MakeNodeSpec(
      InputsOf<Inputs>{Required("input", &Inputs::input)},
      PreservedOutput<TextBatch>("output", "input"),
      Parameters<Params>{Field("replacement", &Params::replacement)
                             .Default("自检")
                             .Description("替换开机检查的文本")}
          .Include(&Params::generation, GenerateParameters()),
      ModelsOf<Models>{Model("model", "bind_model", &Models::model)}, &Run)
      .Description("替换操作词语，并调用 LLM 整理为规范指令");
}
```

`LlmCall` 提供已经准备好的模型能力。`bind_model` 在配置中选择模型名，
`GenerateParameters()` 复用框架的生成参数声明和校验。
`replacement`、`max_tokens` 等都平铺在节点的 `params` 中，不额外嵌套 `generation`。
模型失败直接返回，框架不会发布部分结果。

### 更新已有测试

继续修改同一个测试文件。加入头文件 `tests/support/recording_models.h`，
在 `SessionContext session;` 后加入以下模型替身：

```cpp
auto model = std::make_shared<test_support::RecordingLlm>();
model->replies = {{"请执行启动自检\n"}};
ASSERT_TRUE(test_support::RegisterNodeModel(session, "fixture", model));
```

将 `probe.Init` 的参数对象改为：

```cpp
nlohmann::json{{"replacement", "启动自检"},
               {"bind_model", "fixture"},
               {"max_tokens", 32}}
```

输入仍为 `"请做一次开机检查"`，将输出期望值改为 `"请执行启动自检"`，
以确认回答末尾的换行已被清理。在原有结果断言之后补充：

```cpp
ASSERT_EQ(model->calls.size(), 1u);
ASSERT_EQ(model->calls.front().size(), 1u);
EXPECT_EQ(model->calls.front().front().data,
          "将下面的操作说明改写为简洁的规范指令，只输出改写结果：\n请做一次启动自检");
EXPECT_EQ(model->call_options.front().max_tokens, 32);
```

按第一节的命令重新编译和测试。现在测试验证了替换词、实际提示词、生成参数和回答清理；
Catalog 应显示更新后的节点说明、`bind_model`、生成参数和一个 LLM 依赖。
模型替身只提供确定的回答，业务函数无需识别测试环境。

## 4. 把这个节点接入 Pipeline

这次只验证文本进、文本出的完整调用，复用现有 `translate` 输入输出转换器：
请求是 `{"query":"..."}`，响应是 `{"translated":"..."}`。
字段名由已有外部契约决定，节点内部仍只接收和返回文本。
创建练习目录，并复制现有测试资源：

```bash
mkdir -p build/first_node/artifacts
cp demo/fixtures/mock/artifacts/neutral-llm.fixture build/first_node/artifacts/
```

保存以下内容为 `build/first_node/pipeline.json`：

```json
{
  "io": {
    "input": [{"type": "entity_in", "name": "translate"}],
    "output": [{"type": "json_out", "name": "translate",
                "inputs": {"translation": "normalize.output"}}]
  },
  "models": [{
    "type": "llm",
    "name": "normalize_llm",
    "file": "artifacts/neutral-llm.fixture",
    "backend": {"type": "test_causal_lm_backend", "params": {"fixed_batch_size": 2}}
  }],
  "pipeline": [{
    "type": "my_text_normalize",
    "name": "normalize",
    "params": {"replacement": "启动自检", "bind_model": "normalize_llm", "max_tokens": 32},
    "inputs": {"input": "input.query"}
  }]
}
```

`input.query` 来自输入转换器，`normalize.output` 交给输出转换器。
Validator 从连线推导依赖。模型的 `file` 相对 Pipeline JSON 所在目录解析。
这里使用已有测试模型，不增加 Model 或 Backend 实现。

同目录保存 `pipeline.conf`，内容为 `{"pipe_path": "pipeline.json"}`。
再创建 `input.jsonl`，写入两行完整请求：

```jsonl
{"query":"请做一次开机检查"}
{"query":"设备启动后检查状态"}
```

```bash
cmake --build build --target alg_pipeline_tool_test alg_demo -j 4
./build/alg_pipeline_tool_test validate build/first_node/pipeline.json
./build/alg_pipeline_tool_test plan build/first_node/pipeline.json
./build/alg_demo --config build/first_node/pipeline.conf --dataset build/first_node/input.jsonl --output-dir build/first_node/results
```

这些配置使用测试注册，因此校验使用 `alg_pipeline_tool_test`。
查看 `build/first_node/results/pipeline/results.jsonl`：两条记录都应为 `status: 0`，
`output.translated` 为 `[mock] generated text`；`summary.json` 应显示两条成功、零条失败。
这是接入验证；前后处理的具体结果由上一节的测试检查。
替换真实模型时，按目标构建的 Catalog 更新 `models` 与资产路径，再用业务样本验收效果。

## 5. 接下来按需求扩展

这三个阶段共用一份源码、一条注册语句和相同的端口。是否声明模型由实际算法决定。
测试跟随业务行为补充；框架初始化和模型替身的使用可查
[测试说明](../../src/custom_nodes/README.md#本地快速验证)。
交付前执行 `./scripts/run_all_tests.sh`，流程见 [CONTRIBUTING](../../CONTRIBUTING.md)。

| 遇到的需求 | 参考 |
| --- | --- |
| 更多参数类型、模型能力、端口和来源关系 | [常用写法与按需参考](custom_node_concepts.md) |
| 运行时更新已声明的参数 | [Control 练习](first_control.md) |
| 多输入、条件重试或多模型 | [多输入 starter](../../dev_support/node_authoring/starter_batch_node.cpp)、[多模型 starter](../../dev_support/node_authoring/starter_multi_model_node.cpp) |
| 配置化提示词和复杂后处理 | [复杂算法的组织](custom_node_concepts.md#复杂算法仍按普通-c-函数组织) |
| 新的外部请求或响应契约 | [业务接入指南](business_onboarding.md) |

节点按操作命名，同一个节点可以在多个方案中复用。
