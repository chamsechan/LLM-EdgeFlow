# 新业务接入：从平台结构到统一 Demo

Adapter 转换业务输入和输出。输入转换器读取完整请求。输出转换器组装完整响应。
先按 [README](../../README.md#快速开始)完成构建。以下命令在仓库根目录执行。

按以下步骤接入业务：

1. 写出完整请求和响应样例，例如 `{"query":"hello"}` → `{"translated":"你好"}`。
2. [实现缺失的转换器](#3-实现并注册转换器)。
3. 在 Pipeline 的 `io` 中选择转换器。
4. 用 Operator 测试完整业务字段。
5. 用自己的配置和数据运行 Demo。验证方法见[最小验证](#7-最小验证)。

载体是宿主传入或接收的数据结构，binding 负责读取和写入载体。
如果已有载体能表达请求和响应，复用 binding、输出池和 Demo。新类型和输出容量见第 4、6 节。
公共协议变更遵循 [CONTRIBUTING](../../CONTRIBUTING.md#3-design-and-current-contracts)。

## 输入输出以 Operator 接口为边界

完整请求与响应以 `OperatorFunc::Process` 为边界。例如输入 `{"query":"hello"}`，
由输入转换器解析对象并选择 `query`。宿主直接调用 SDK 就能获得约定的完整响应。

| 部件 | 职责 |
| --- | --- |
| Demo / 调用方 | 构造并持有输入载体，调用 SDK，复制和展示返回值 |
| binding | 校验平台指针和长度，复制数据，处理输出布局与容量 |
| InputConverter / OutputConverter | 校验业务字段、选择输入，组装并序列化完整响应 |
| Pipeline / Node | 处理内部端口数据，执行业务算法 |
| Pipeline 根 `io` | 按 `(type, name)` 选择转换器并连接内部端口 |

Catalog 的端口描述内部数据。外部格式以转换器契约为准。

## 1. 先确定要走哪条路径

先运行 `./build/alg_pipeline_tool catalog` 核对已有能力，再选择修改范围：

| 需求 | 修改范围与下一步 |
| --- | --- |
| 外部业务契约不变，只调整规则、提示词、模型或连线 | 修改 Pipeline 和必要的 `.conf`，按[运行当前方案](../../tools/pipeline_studio/README.md#运行当前方案)验证；复用已有转换器和 Demo |
| 外部业务契约不变，但已有 Node 无法完成算法 | 按[自定义 Node 入门](first_custom_node.md)实现缺失算法，再复用已有接入路径 |
| 外部载荷的字段/格式/语义改变，或需要新的平台结构 | 为新契约选择或登记 `(type, name)` 转换器；分别复用符合输入、输出语义的转换器，只新增缺失的一侧；已有载体与 ValueType 继续复用 |
| 修复已有业务的转换逻辑 | 修改对应输入/输出转换器并运行相关契约测试；仅影响该路径时，无需另建 Demo |

分别选择输入和输出转换器。只实现缺失的一侧。
同一个字符串载体可保存纯文本或完整 JSON。选择转换器时，核对字段格式和含义。
配置用 `(type, name)` 查找转换器。未注册时，工具报告 `UNKNOWN_CONVERTER`。

## 2. 用一个现有业务看清文件关系

以“输入一句文本，返回关键词匹配结果”为例。它使用现有规则节点，不需要模型权重。
先确认实际注册，再运行一次完整路径：

```bash
./build/alg_pipeline_tool catalog
./build/alg_pipeline_tool describe-node text_rule_match
./build/alg_pipeline_tool validate configs/pipeline_keyword_match_rules.json
./build/alg_pipeline_tool plan configs/pipeline_keyword_match_rules.json
./build/alg_demo --config configs/pipeline_keyword_match_rules.conf --dataset tests/fixtures/effects/keyword_inputs.txt --output-dir results/business-onboarding
```

打开 `results/business-onboarding/pipeline_keyword_match_rules/results.jsonl`。预期结果如下：

- 三条记录按输入顺序排列，`status` 均为 0。
- 第一、三条的 `output.is_hit` 为 1，类别为 `SYSTEM_INIT`。
- 第二条的 `output.is_hit` 为 0。

检查 `summary.json`，应有三条成功、零条失败。
未使用 Profile 时，结果子目录使用去掉 `.conf` 后缀的文件名。
验证新业务时，使用新业务的配置和输入。

按下表核对需要补齐的部分。表中的名称都是这个已注册样例的名称，无需逐项复制。

| 环节 | 样例文件 | 需要补齐时落实的内容 |
| --- | --- | --- |
| 本地模拟平台结构 | [Operator 数据结构](../../include/platform_mock/operator_data_types.h)、[平台交互类型](../../include/platform_mock/operator_types.h) | 已有载体不足时才新增结构，明确字段、长度和所有权；本目录只保存模拟约定，真实公司定义在授权内网接入 |
| 中立 I/O 值 | [io_values.h](../../include/adapter/io_values.h) | binding 与 Converter 之间的请求自有内容，无平台指针、长度或分配字段 |
| 内部数据边界 | [计划中的端口](../../include/core/validated_node_plan.h)、[中立结果类型](../../include/core/common_contracts.h) | 转换器逻辑端口及 `节点名.端口名` 引用与 Pipeline 产出的中立结果；已有类型可复用，外部响应由输出转换器组装 |
| 输入转换器 | [text_input.cpp](../../src/adapter/input/text_input.cpp) | 完整请求业务校验、中性数据封装及 `REGISTER_INPUT_CONVERTER` |
| 输出转换器 | [keyword_result_output.cpp](../../src/adapter/output/keyword_result_output.cpp) | 内部结果关联、完整响应组装及 `REGISTER_OUTPUT_CONVERTER` |
| 平台 binding | [operator_builtin_value_types.cpp](../../src/adapter/operator/mock/operator_builtin_value_types.cpp)、[platform_value_binding.h](../../include/adapter/operator/mock/platform_value_binding.h) | 模拟平台的类型、traits、布局读写及内存管理；其他平台使用自己的 binding 文件，复用 Converter 的中立值，见[实现与注册](operator_output_allocation.md#实现与注册) |
| Demo 载体与展示 | [text_input.cpp](../../demo/operator_shell/mock/input/text_input.cpp)、[json_output.cpp](../../demo/operator_shell/mock/output/json_output.cpp) | 复用同一宿主结构的构造与展示；新载体组合才补输入登记，新输出结构才补展示登记 |
| 构建与部署 | [Pipeline](../../configs/pipeline_keyword_match_rules.json)、[部署配置](../../configs/pipeline_keyword_match_rules.conf) | 新增 `.cpp` 自动编入；编排业务端口，配置路径和输出容量 |

配置作者在根 `io.input` / `io.output` 填写所选载体 `type` 和业务 `name`。
关键词例子分别为 `keyword_in/keyword_match` 与 `json_out/keyword_match`。
Demo 从公开 SDK 预检获得载体、业务值与必需性，再选择对应构造和展示代码。

现有业务输出复用 `json_out`：`MockOutput.text` 指向的池化 `MockTextOutput` 保存完整 JSON，
输出结构不暴露状态码。具体字段见对应输出转换器。

### 参考实现与测试

平台载体读取见 [mock binding](../../src/adapter/operator/mock/platform_value_binding.cpp)，
业务字段选择和响应组装见 [InputConverter](../../src/adapter/input/) 与
[OutputConverter](../../src/adapter/output/)。平台结构只位于 `include/platform_mock/`。
相同中立值类型可以承载不同业务语义。

[平台载体契约测试](../../tests/unit/operator/operator_shell/mock/test_platform_carrier_contracts.cpp) 执行实际 binding，
覆盖字符串、嵌套字段、带行 padding 的 RGB、PCM 和候选数组的复制及边界。
[Converter 公共机制测试](../../tests/unit/adapter/test_converter_contracts.cpp) 覆盖行关联和原子发布，
[业务 Converter 测试](../../tests/unit/adapter/test_io_converters.cpp) 核对完整字段。

## 3. 实现并注册转换器

先选择可复用的转换器。只在缺少转换器时执行以下实现步骤。
文本 JSON 业务可参照[翻译输入的 Decode](../../src/adapter/input/translate_json_input.cpp)与
[翻译输出的 Encode](../../src/adapter/output/translation_json_output.cpp)：前者解析完整对象并选择 `query`，
后者将结果序列化为含 `translated` 的完整响应。按需要完成：

1. **实现输入转换器（`src/adapter/input/`）。**
   单槽且每请求生成一个载荷时，写普通函数
   `AdapterStatus Decode(const Value& input, Payload* output)`，接收 `adapter/io_values.h`
   中的自有值，解析完整请求并校验业务要求。`DecodeInputFn` 调用 `DecodeRequestRows<Value>`，
   传入槽、类型端口和函数。框架调用 binding 读取载体，再处理循环、来源编号和发布。
   需要转交图像等自有载荷时，回调可接收 `Value&` 并移动其成员。不得移动或保存宿主内存。
   候选展开等算法使用 `ValidateDecodeRequest` / `ReadInputSlot<Value>` 显式组织，后者返回
   `std::optional<Value>`。binding 校验平台指针、长度和表示形式。
   Converter 校验业务限额，例如 rerank 段落的 64 KiB 限制。
   Operator 按输入输出 vector 的行位置关联。内部 `req_id` 为本次调用的批内行号。
   Converter 不读取宿主请求 ID 或图像帧序号。
   定义 `InputConverterDefinition`。用 `REGISTER_INPUT_CONVERTER` 注册转换器。
2. **实现输出转换器（`src/adapter/output/`）。**
   每请求一个结果时，写普通函数
   `AdapterStatus Encode(const Payload& result, Value* output)`，设置业务标量并直接赋值
   `std::string` 或自有数组。`EncodeOutputFn` 调用 `EncodeResultRows<Value>`，框架检查
   每请求恰好一个 `sub_id=0` 的结果、恢复行顺序，再通过 binding 写入内容。
   多路结果组合和排名显式使用 `ReadOutputValue` / `IndexResults`，完成校验和行对齐后调用
   `WriteOutputRows<Value>`；回调接收原始行号与 `Value*`，返回 `AdapterStatus`。
   helper 跳过未请求的可选行、调用 `WriteOutputValue` 并统计成功写入数量；业务组装仍由
   Converter 负责。`IndexResults` 用输出视图的 `count` 校验原始批内来源。
   binding 按实际输出池容量复制字符串和数组。Converter 检查业务状态并组装完整 JSON。
   定义 `OutputConverterDefinition`。用 `REGISTER_OUTPUT_CONVERTER` 注册转换器。
3. **声明选择、参数与逻辑端口。**
   每个方向的 `(type, name)` 必须唯一。每份注册只有一个宿主槽。`type` 选择宿主 binding。
   用 `ExternalInputSlot<Value>()` / `ExternalOutputSlot<Value>()` 声明中立类型，审计要求与 binding
   的 `value_type` 一致。`name` 是中性业务名。结构带业务成员时，binding 的 `services` 显式
   映射非 common 名称到平台枚举，Converter 不包含枚举。`common` 和无该成员的结构没有期望业务值。
   逻辑端口是类型化 Blackboard key，接入准备将所选端口组合为 Core 必传边界。
   每个输入发布不同端口。多个输入槽按同一批内行号配对，载体无需请求 ID 字段。
   声明参数使用普通 `Params` 与 `Parameters<Params>`，每个字段有默认值或 Required。
   无默认值的 optional 字段可省略。Create 执行校验、赋值、Prepare 和 Validate。Process 只读取共享参数。
   当前 `json_out` 直接用 `JsonOutputParameters()` 共享容量声明。其他输出的每个字符串字段使用
   `MaxBytes` 声明最小值 1 和默认尺寸，见第 6 节。

## 4. 需要新的宿主类型时

多数业务复用已注册的宿主 binding 与中立 I/O 值，载荷协议变化只需修改 Converter。
确需新的平台结构时，按[实现与注册](operator_output_allocation.md#实现与注册)登记 binding，
用 `SetInputValue<Host, Value>` / `SetOutputValue<Host, Value>` 连接所需中立值。
当前模拟平台的 traits 与布局 helper 在 `platform_value_binding.h`。真实平台使用自己的 binding
文件，核对真实头文件与内存所有权。Converter、Node、Model 和 Backend 不识别宿主结构。
新增具有相同内容语义的布局应复用中立值及 Converter。业务协议变化仍由 Converter 处理。

图像由调用方提供 RGB8 像素帧，binding 校验尺寸、步长和容量后复制为中立 `ImageFrame`。
Demo 可从文件构造像素帧，Model 负责预处理。真实 SDK 的字段与格式须在授权内网核对，
遵循[仓库边界](../../AGENTS.md#repository-guardrails)。

## 5. 统一 Demo 接入

已有载体可直接复用 Demo。准备 Pipeline、`.conf` 和数据集，按第 2 节运行即可。
`.conf` 的 `pipe_path` 相对其所在目录。模型及文件参数相对 Pipeline 目录。
详细操作见[运行当前方案](../../tools/pipeline_studio/README.md#运行当前方案)。

新增载体时：

1. 在所选壳夹具的 `demo/operator_shell/<shell>/input/` 实现 `BuildRequestsFn`，用 `REGISTER_DEMO_INPUT` 登记输入结构组合。
   组合顺序来自 SDK 的 `ResolveOperatorConfigIo` 预检。`DemoRequestBatch.storage` 持有运行期间所需缓冲区。
2. 在同一夹具的 `output/` 实现 `ShowResultFn`，用 `REGISTER_DEMO_OUTPUT` 登记输出结构。
   复制 SDK 输出结构中的结果，业务字段选择与响应组装由转换器完成。
3. 公共 runner 负责调用、输出复制和租约归还。两个目录的 `.cpp` 自动编入。
   运行后检查 `results.jsonl` 中的逐条状态及 `summary.json`。

需要保存运行预设时加入 `demo/profiles.json`。设备、批大小和池深通过 Profile 的
`chip`、`device_id`、`batch_size`、`depth` 设置。运行时控制见 [Control 入门](first_control.md)。

## 6. 输出容量

输出池深控制可同时持有的输出数量，`params.result_json_max_bytes` 控制单份完整 JSON 响应的字节容量。
默认值、硬上限和布局规则统一见[输出容量声明](operator_output_allocation.md#选择转换器与参数)，
批次限制见[宿主调用约定](operator_output_allocation.md#宿主调用与生命周期)。
普通业务不必重复填写默认容量。需要覆盖时，在所选输出项配置参数，例如：

```json
{"type":"json_out", "name":"doc_qa", "params":{"result_json_max_bytes":16383}}
```

单份响应超过实际字段容量时返回 -4。本批租约全部归还，所有输出保持未发布状态。
如果需要更大容量，修改所选输出项的 `params`。然后重新创建 handle。
容量按字节计算，不能由 token 数直接推断。
宿主借用输入、输出租约和销毁顺序见[宿主调用与生命周期](operator_output_allocation.md#宿主调用与生命周期)。

## 7. 最小验证

重新构建，再检查新业务是否进入 Catalog：

```bash
cmake --build build --target alg_sdk alg_pipeline_tool alg_demo -j 4
./build/alg_pipeline_tool catalog
```

1. 对修改后的 Pipeline 执行 `validate`。
2. 执行 `plan`，检查节点顺序。
3. 运行 Demo。
4. 核对结果的行顺序、状态和业务字段。

使用测试模型时，改用[测试工具](../../tools/pipeline_studio/README.md#工具选择) `alg_pipeline_tool_test`。

把业务断言加入[业务响应测试](../../tests/integration/operator/operator_shell/mock/test_business_responses.cpp)：
直接调用 Operator，检查完整字段、逐行对应关系和无效输入。
修改 binding 或输出容量时，还需覆盖[平台载体契约](../../tests/unit/operator/operator_shell/mock/test_platform_carrier_contracts.cpp)、
[输出发布](../../tests/unit/operator/operator_shell/mock/test_operator_publication.cpp)与[壳无关一致性套件](../../tests/contract/operator_shell/)。开发中只运行相关用例，例如：

```bash
cmake --build build --target edgeflow_test_adapter_runner -j 4
./build/edgeflow_test_adapter_runner --gtest_filter='BusinessResponsesTest.TranslateSelectsQueryFromCompleteJsonAndPairsTranslatedRows'
```

交付前执行 `./scripts/run_all_tests.sh`。真实模型效果与目标平台验收按
[效果验收指南](../VERIFIABLE_SELECTION.md)另行记录。涉及公司内部 SDK 时遵循
[仓库边界](../../AGENTS.md#repository-guardrails)，当前外部工作区只准备中立接口，
完整项目进入授权内网后才进行真实 SDK 对接和目标设备验收。
