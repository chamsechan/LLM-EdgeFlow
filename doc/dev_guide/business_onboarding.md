# 新业务接入：从平台结构到统一 Demo

本指南写给新增或调整外部业务契约的转换器作者。新宿主类型、输出池实现，以及宿主程序如何
调用 SDK，见 [Operator 宿主类型、输出池与生命周期](operator_output_allocation.md)。
先按根目录 [README](../../README.md#快速开始)完成快速开始构建；以下命令都从仓库根目录执行。
公共结构或协议变更先按 [CONTRIBUTING](../../CONTRIBUTING.md#3-design-and-current-contracts)
明确契约与设计，再开始实现。

第一次接触 Adapter，可以先运行[无需模型的关键词样例](#2-用一个现有业务看清文件关系)，
观察完整请求与响应，再按实际缺口查阅下文。新契约不要求重新实现所有接入组件。

## 输入输出以 Operator 接口为边界

本项目业务需求中的“输入、输出”指 `OperatorFunc::Process` 边界上的完整请求和完整响应。
字段名、字段类型、序列化格式及忽略字段的约定都属于外部业务契约。
例如输入是 `{"query":"hello","src_lan":"en"}` 时，Operator 输入结构的字符串字段应
承载整个对象；不能由 Demo 先提取 `hello` 再声称完成该输入契约。

| 位置 | 负责的工作 |
| --- | --- |
| Demo / 调用方 | 读取样例、构造 Operator 载体、持有缓冲区、调用 SDK、复制或展示 SDK 返回值 |
| `InputConverterDefinition::decode_fn` | 校验外部请求、解析完整载荷、选择业务字段，转换为请求内的中性值发布至 `AlgContext` |
| Pipeline / Nodes | 对内部 typed ports 的数据执行算法；可解析模型生成的结构化内容，不承担外部协议转换 |
| `OutputConverterDefinition::encode_fn` | 从 `AlgContext` 读取中性结果，按外部契约组装序列化响应并写入已租用输出池 |
| Pipeline 根 `io` | 按方向与 `(type, name)` 选择转换器，所选 typed 端口组成 Core 明确边界 |

Demo 输出里的日志、统计和展示字段可以另行组织，但不能为 SDK 补做业务字段提取、
字段改名、响应组装或默认成功结果。宿主程序直接调用 Operator SDK 就应获得约定响应。
Catalog 的 ingress/egress 是转换器与 Pipeline 之间的内部逻辑端口，不能当成外部请求格式。
按数据形态选择下文的真实业务示例；相同载体可以承载不同协议。

## 1. 先确定要走哪条路径

先运行 `./build/alg_pipeline_tool catalog`，核对已有业务契约、操作和模型能力。

已知类型名称时可以直接查询，输出复用当前构建的 Catalog：

```bash
./build/alg_pipeline_tool describe-node TextRuleMatchNode
./build/alg_pipeline_tool describe-model qwen_causal_lm
./build/alg_pipeline_tool describe-backend llama_cpp
```

| 需求 | 修改范围与下一步 |
| --- | --- |
| 外部业务契约不变，只调整规则、提示词、模型或连线 | 修改 Pipeline 和必要的 `.conf`，按[运行当前方案](../../tools/pipeline_studio/README.md#运行当前方案)验证；复用已有转换器和 Demo |
| 外部业务契约不变，但已有 Node 无法完成算法 | 按[自定义 Node 入门](first_custom_node.md)实现缺失算法，再复用已有接入路径 |
| 外部载荷的字段/格式/语义改变，或需要新的平台结构 | 为新契约选择或登记 `(type, name)` 转换器；分别复用符合输入、输出语义的转换器，只新增缺失的一侧；已有载体与 ValueType 继续复用 |
| 修复已有业务的转换逻辑 | 修改对应输入/输出转换器并运行相关契约测试；仅影响该路径时，无需另建 Demo |

这些路径可以组合：新契约可以继续编排已有 Nodes，也可以只补充一个缺失算法。
输入和输出分别判断是否需要新转换器；仅改变输出时，输入转换器通常可以原样复用。
已有宿主载体、ValueType 和 Demo 运行代码能表达完整请求、响应及数据集格式时优先复用；
需要通过统一 Demo 运行的新载体组合还需匹配的 Demo 注册。Profile 只在需要保存运行预设时添加。

“结构体布局相同”不等于“业务契约相同”：同一个 `const char*` 承载纯文本与承载完整
JSON 请求是不同的输入约定。已有 Nodes 能完成算法，也不代表转换器已支持新协议。

Operator 初始化会审计**全部已注册的转换器**，包括未被当前方案选中的登记。
槽后缀与平台类型、业务成员、回调、typed 端口和参数声明必须一致；任一登记不合格，Init 返回 -6。
配置按 `(type, name)` 精确查找，未登记时报 `UNKNOWN_CONVERTER`，没有默认回退。

## 2. 用一个现有业务看清文件关系

以“输入一句文本，返回关键词匹配结果”为例。它使用现有规则节点，不需要模型权重。
先确认实际注册，再运行一次完整路径：

```bash
./build/alg_pipeline_tool catalog
./build/alg_pipeline_tool describe-node TextRuleMatchNode
./build/alg_pipeline_tool validate configs/pipeline_keyword_match_rules.json
./build/alg_pipeline_tool plan configs/pipeline_keyword_match_rules.json
./build/alg_demo --config configs/pipeline_keyword_match_rules.conf --dataset tests/fixtures/effects/keyword_inputs.txt --output-dir results/business-onboarding
```

查看 `results/business-onboarding/keyword_match/results.jsonl`：请求编号为 20001–20004，
四条 `status` 均为 0，前两条 `output.is_hit` 为 true 且类别为 `SYSTEM_INIT`，后两条
为 false。`summary.json` 应有四条成功、零条失败。这一步用于认识已有接入链路；
新业务仍须换成自己的配置和输入验证。

按下表核对需要补齐的部分；表中的名称都是这个已注册样例的名称，无需逐项复制。

| 环节 | 样例文件 | 需要补齐时落实的内容 |
| --- | --- | --- |
| 本地模拟平台结构 | [Operator 数据结构](../../include/platform_mock/operator_data_types.h)、[平台交互类型](../../include/platform_mock/operator_types.h) | 已有载体不足时才新增结构，明确字段、长度和所有权；本目录只保存模拟约定，真实公司定义在授权内网接入 |
| 内部数据边界 | [业务 key](../../include/adapter/biz_blackboard_keys.h)、[中性结果类型](../../include/core/common_contracts.h) | ingress/egress typed key 与 Pipeline 产出的中性结果；已有类型可复用，外部响应由输出转换器组装 |
| 输入转换器 | [text_input.cpp](../../src/adapter/input/text_input.cpp) | 外部输入校验、中性数据封装及 `REGISTER_INPUT_CONVERTER` |
| 输出转换器 | [keyword_result_output.cpp](../../src/adapter/output/keyword_result_output.cpp) | 内部结果关联、写入已分配的输出结构及 `REGISTER_OUTPUT_CONVERTER` |
| Operator 类型注册（仅新平台宿主类型） | [operator_builtin_value_types.cpp](../../src/adapter/operator/operator_builtin_value_types.cpp) | 复用已注册类型时无需改动；新平台宿主类型在此登记一项，与平台结构、type traits 一一对应；已有类型的新嵌套布局用自己文件中的命名方案，见[实现与注册](operator_output_allocation.md#实现与注册) |
| Demo 载体与展示 | [keyword_input.cpp](../../demo/input/keyword_input.cpp)、[keyword_output.cpp](../../demo/output/keyword_output.cpp) | 复用同一宿主结构的构造与展示；新载体组合才补输入登记，新输出结构才补展示登记 |
| 构建与部署 | [Pipeline](../../configs/pipeline_keyword_match_rules.json)、[部署配置](../../configs/pipeline_keyword_match_rules.conf) | 新增 `.cpp` 自动编入；编排业务端口，配置路径和输出容量 |

配置作者在根 `io.input` / `io.output` 填写所选载体 `type` 和业务 `name`。
关键词例子分别为 `keyword_in/keyword_match` 与 `keyword_out/keyword_match`。
Demo 从公开 SDK 预检获得载体、业务值与必需性，再选择对应构造和展示代码。

## 3. 实现并注册转换器

先选择可复用的输入、输出转换器，并在配置中组合；只有缺少兼容转换器时才实现对应步骤。
参照关键词或实体抽取的实现，按需要完成：

1. **实现输入转换器（`src/adapter/input/`）。**
   单槽且每请求生成一个载荷时，先写普通函数
   `AdapterStatus Decode(const Host& input, Payload* output)`，只校验业务字段并复制为自持有值。
   `DecodeInputFn` 内调用 `DecodeRequestRows<Host>`，传入槽、typed 端口与该函数；
   框架负责槽检查、循环、批内来源编号和发布。批次上限统一由 Operator 检查，为有效池深与 64 的较小值。
   文本可用 `IsValidInputString` / `CopyInputString`，PCM 的范围检查和复制仍属于业务函数。
   多槽、候选展开等算法继续使用 `ValidateDecodeRequest` / `ReadInputSlot<T>` 显式组织。
   请求编号由框架保存和恢复，内部批次使用批内编号。自己组织多槽解码或多路结果的转换器，
   在所有行校验通过后调用 `PublishRequestIds` 记录编号，编码时通过 `RequestIds` 读取；
   编号表通过解码/编码选项传递，不声明为业务端口。
   定义 `InputConverterDefinition`并使用
   `REGISTER_INPUT_CONVERTER` 注册。
2. **实现输出转换器（`src/adapter/output/`）。**
   每请求一个结果时，先写普通函数
   `AdapterStatus Encode(const Payload& result, Host* output, const OutputStringWriter& writer)`。
   函数设置业务字段，字符串用 `writer.Write(output->field, "field", value)` 写入。
   `EncodeOutputFn` 调用 `EncodeResultRows<Host>`，由框架读取绑定、检查每请求恰好一个
   `sub_id=0` 的结果、恢复顺序与外部 `request_id`、维护 `written_count`。
   多路结果组合和排名仍显式使用 `ReadOutputValue` / `IndexResults` / `WriteOutputString`。
   所有写入使用实际输出池容量，不在转换器内另填容量默认值；业务状态和 JSON 组装仍由函数负责。
   定义 `OutputConverterDefinition`并使用
   `REGISTER_OUTPUT_CONVERTER` 注册。
3. **声明选择、参数与逻辑端口。**
   每个方向的 `(type, name)` 唯一，每份登记只有一个宿主结构槽，`slot.type_suffix == type`。
   `name` 是平台业务值的中性对应，`common` 表示该载体的默认处理。平台结构带业务成员时，
   非 common 登记显式声明 `service_type`；common 和无该成员的结构不声明该值。
   逻辑端口是内部 typed Blackboard key，接入准备将所选端口组合为 Core 必传边界。
   每个输入发布不同端口；至少一个所选输入结构带请求 ID，多个都带时逐行一致。
   声明参数用普通 `Params` 与 `Parameters<Params>`，每个字段有默认值或 Required；
   无默认的 optional 可省略。Create 校验、赋值、Prepare、Validate 一次，Process 只读共享值。
   输出的每个字符串字段使用 `MaxBytes` 声明最小值 1 和默认尺寸，见第 6 节。
   新载体的请求 ID 和业务成员在 ValueType 中用成员指针显式登记，不根据类型名猜测布局。


## 4. 需要新的宿主类型时

多数业务复用已注册的宿主类型和 ValueType，载荷协议变化只需修改转换器。确需新的平台结构时，
按[实现与注册](operator_output_allocation.md#实现与注册)在平台头文件、`io_converter.h` 和
`operator_builtin_value_types.cpp` 中各新增一项；已有结构需要另一种嵌套布局时，在自己的文件中新增
命名方案，不改已有实现。宿主类型的转换都留在接入适配层，Node、Model 和 Backend 无需识别宿主结构。

## 5. 统一 Demo 接入

Demo 调用 SDK 的 `ResolveOperatorConfigIo` 做只读预检，取得有序的输入、输出载体条目；
请求构造按输入结构名组合分派，结果展示按输出结构名分派。同一宿主结构承载不同业务时共用 Demo，
例如实体抽取与翻译都使用实体输入、输出结构。业务字段校验、选择和完整响应组装仍由 SDK 的转换器负责；
Demo 从数据集构造载体并显示结果，不把完整请求预先拆成内部端口。

1. 复用已有输入构造；需要新载体组合时，在 `demo/input/` 新建 `.cpp`，
   实现 `BuildRequestsFn` 并用 `REGISTER_DEMO_INPUT` 登记。组合键按预检返回的输入顺序拼接
   结构名，例如 `CompanyFrame,CompanyString`。`DemoRequestBatch.storage` 必须持有字符串、
   数组与宿主结构，覆盖整个运行过程；请求键取自契约的 `demo.<type>`。
2. 复用已有输出展示；新输出结构在 `demo/output/` 实现 `ShowResultFn`，
   用 `REGISTER_DEMO_OUTPUT` 登记结构名。复制实际 `request_id`、`status_code` 和输出字段；
   请求信息只是可选的显示辅助，缺失时应能降级。两个目录的 `.cpp` 自动编入。
3. 公共 `RunOperatorDemo` 统一预检、创建句柄、Control、分批 Process、显示和落盘。
   每批先把输出复制为 JSON，再归还所有输出租约。多个输出项的同名字段以 `type` 加前缀；同方向重复 type 时以 `name.type` 加前缀，避免覆盖。
   Process 成功表示调用完成，逐条状态仍需检查 `results.jsonl` 和 `summary.json`。
4. 准备数据、Pipeline 和 `.conf`，用 `--config`、`--dataset` 运行；可重复调用的预设可加入
   `demo/profiles.json`。`--list` 列出 Profile 与支持的载体。只修改已有载体上的业务契约时无需新增 Demo。

运行中修改参数必须显式提供 `--control-file` 和正整数 `--control-cmd`，也可写进 Profile；
缺少任一项返回 3。规则更新示例为 `--profile keyword_match_control`，payload 来自
[`keyword_match_control.json`](../../data/keyword_match_control.json)。该 Profile 不加入 smoke，独立执行验收。
音频输入是否要求真实数据集按 Profile 的 `suite == "real"` 判断；`--suite all` 只选择 Profile 集合。
结果记录使用 Profile 名，无 Profile 时使用配置文件名的词根；不写入业务身份。

`.conf` 仅作为定位文件，包含单一字段 `pipe_path`，相对 `.conf` 所在目录解析（例如在 `configs/pipeline_keyword_match_rules.conf` 中填写 `pipeline_keyword_match_rules.json`）。宿主直接调用 Operator 时，部署根为 Create 的 `model_path`；同时核对 Pipeline JSON 中的 `models[].model_path` 与所选输出项 `params` 的容量。模型路径只配置在模型条目中，相对路径以宿主部署根为基准。Profile 不会自动指向新方案，详细命令见[运行当前方案](../../tools/pipeline_studio/README.md#运行当前方案)。

Demo 的 `chip`、`device_id`、`batch_size`、`depth` 只从 Profile JSON 读取；对应 CLI
选项已删除。使用 `--profiles-file <path> --profile <name>` 选择自有配置。未选 Profile
或未提供字段时使用 `cpu`、`0`、`1`、`1`。配置路径、数据集等其他 CLI 覆盖仍有效。

## 6. 输出容量

Process 有效批次上限为 `min(max_frame_depth, 64)`；超过时失败，不自动拆批。
池深 0 使用默认 25，池深硬上限为 1024。输出池数量与业务响应字节容量是不同限制。

每个输出字符串字段对应 `params.<field>_max_bytes`，最小值为 1，默认值只在转换器声明。
平台布局只保留硬上限；框架在 Prepare 后读取生效参数，生成全部池容量并检查总载荷预算。
例如翻译 `entity_out/translate` 自带 `entities_json_max_bytes = 8191`，不用在配置重复填写。

```json
{"type":"doc_out", "name":"doc_qa", "params":{"answer_text_max_bytes":4095}}
```

分配器、布局参数和 metadata 固定在槽声明中；另选布局需要另一个登记。
业务参数只在 Create 解析；布局参数在注册审计归一化一次，所有分配共享不可变结果。
可选输出也始终有池，宿主可以按行省略 key，其他行保持原来的索引和请求 ID。
同方向多项复用同一种载体时，使用 `name.type` 宿主 key；唯一 type 可以保留任意非空前缀。

单份响应超过实际字段容量时返回 -4；本批租约全部归还，所有输出保持未发布状态。
需要更大容量时覆盖所选项 params 并重新创建 handle。容量以实际字节数计算，不能直接从 token 数推断。
宿主借用输入、输出租约和销毁顺序见[宿主调用与生命周期](operator_output_allocation.md#宿主调用与生命周期)。

## 7. 最小验证

重新构建，再检查新业务是否进入 Catalog：

```bash
cmake --build build --target alg_sdk alg_pipeline_tool alg_demo -j 4
./build/alg_pipeline_tool catalog
```

确认 Catalog 中出现所选 `(type, name)`，平台槽、业务值、typed 端口与参数声明一致；随后对**本次新增或
修改的 Pipeline** 执行 `validate`、`plan`，运行对应 Demo 并核对请求 ID、状态及业务字段。
第 2 节的关键词命令是可运行参照，实际验证时替换为新业务、配置和数据集。
有意使用测试模型时按[工具选择](../../tools/pipeline_studio/README.md#校验工具选择)
构建并使用 `alg_pipeline_tool_test`。

把断言加入相应现有套件：

| 验证范围 | 必须观察到的行为 | 参考测试 |
| --- | --- | --- |
| 转换器与契约安全 | 非法指针和长度被拒绝；结果乱序仍按来源返回，重复/缺失来源与失败结果被拒绝；输出容量越界严格拦截 | [Adapter 契约测试](../../tests/contract/abi/test_adapter_contract_security.cpp)、[Operator 安全测试](../../tests/contract/abi/test_operator_safety.cpp) |
| Operator SDK | 初始化接受完整注册；在池容量内时输出完整，超池容量时无部分发布且后续请求可继续使用输出池 | [Operator 基础测试](../../tests/integration/operator/test_operator_api.cpp)、[公开 SDK 消费者测试](../../tests/contract/abi/test_cpp_operator_sdk.cpp) |
| Pipeline / Demo | 新业务通过校验和计划，样例结果及错误路径符合预期 | [Catalog/Validator 测试](../../tests/integration/pipeline/test_pipeline_catalog_validator.cpp)、[Demo 测试](../../tests/integration/demo/test_demo_runner.cpp) |

多槽测试可直接把 `ExternalInputBatchView` / `ExternalOutputBatchView` 交给 `AdapterHarness`，
包含各槽类型和池规格。测试中直接持有字符数组时，给 `EncodeOperator` 显式提供各字段可用容量
（数组大小减去结尾 NUL 的一字节），或用 `TestOutputBatchView::SetCapacity` 描述实际存储；
`CompanyString.length` 是内容长度，不能作为容量。

开发过程中只构建并运行相关的测试 runner，把过滤器换成实际修改的套件；
runner 的源码归属见 [tests/RuntimeTests.cmake](../../tests/RuntimeTests.cmake)：

| 修改内容 | 构建目标 | 常用 GoogleTest 过滤器 |
| --- | --- | --- |
| 转换器、协议拷贝或 Operator 绑定 | `edgeflow_test_adapter_runner` | `IoConverterRegistryTest.*` / `OperatorApiTest.*` |
| Demo 结果转换或 Pipeline 集成 | `edgeflow_test_tooling_runner` | `DemoRunnerTest.*` |

```bash
cmake --build build --target edgeflow_test_adapter_runner -j 4
./build/edgeflow_test_adapter_runner --gtest_filter='IoConverterRegistryTest.*'
```

交付前执行 `./scripts/run_all_tests.sh`。真实模型效果与目标平台验收按
[效果验收指南](../VERIFIABLE_SELECTION.md)另行记录；涉及公司内部 SDK 时遵循
[仓库边界](../../AGENTS.md#repository-guardrails)，当前外部工作区只准备中立接口，
完整项目进入授权内网后才进行真实 SDK 对接和目标设备验收。
