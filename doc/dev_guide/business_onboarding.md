# 新业务接入：从平台结构到统一 Demo

本指南面向需要转换平台输入输出的 C++ 开发者。先按根目录
[README](../../README.md#快速开始)完成默认构建；以下命令都从仓库根目录执行。
公共结构或协议变更先按 [CONTRIBUTING](../../CONTRIBUTING.md)记录 RFC，再开始实现。

## 输入输出以 Operator 接口为边界

本项目业务需求中的“输入、输出”指 `OperatorFunc::Process` 边界上的完整请求和完整响应。
字段名、字段类型、序列化格式及忽略字段的约定都属于外部业务契约。
例如输入是 `{"query":"hello","src_lan":"en"}` 时，Operator 输入结构的字符串字段应
承载整个对象；不能由 Demo 先提取 `hello` 再声称完成该输入契约。

| 位置 | 负责的工作 |
| --- | --- |
| Demo / 调用方 | 读取样例、构造 Operator 载体、持有缓冲区、调用 SDK、复制或展示 SDK 返回值 |
| `InputConverter::decode_fn` | 校验外部请求、解析完整载荷、选择业务字段，转换为请求内的中性值发布至 `AlgContext` |
| Pipeline / Nodes | 对内部 typed ports 的数据执行算法；可解析模型生成的结构化内容，不承担外部协议转换 |
| `OutputConverter::encode_fn` | 从 `AlgContext` 读取中性结果，按外部契约组装序列化响应并写入已租用输出池 |
| `IoBinding` | 声明业务逻辑端口与 Pipeline Blackboard Key 的映射关系，将转换器与业务编排关联 |

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
| 外部业务契约不变，只调整规则、提示词、模型或连线 | 修改 Pipeline 和必要的 `.conf`，按[运行当前方案](../../tools/pipeline_studio/README.md#运行当前方案)验证；复用已有转换器、绑定和 Demo |
| 外部业务契约不变，但已有 Node 无法完成算法 | 按[自定义 Node 入门](first_custom_node.md)实现缺失算法，再复用已有接入路径 |
| 外部载荷的字段/格式/语义改变，或需要新的平台结构 | 实现并注册对应 `InputConverter`、`OutputConverter` 与 `IoBinding`；已有载体和 ValueType 可以复用 |
| 修复已有业务的转换逻辑 | 修改对应输入/输出转换器并运行相关契约测试；仅影响该路径时，无需另建 Demo |

“结构体布局相同”不等于“业务契约相同”：同一个 `const char*` 承载纯文本与承载完整
JSON 请求是不同的输入约定。已有 Nodes 能完成算法，也不代表转换器已支持新协议。

当前共享 SDK 的 Operator 初始化会全量审计**所有已声明业务的曝光与绑定**。新增生产业务
必须有完整的 Operator 绑定与转换器注册；
若缺少绑定，SDK 全局初始化失败。

## 2. 用一个现有业务看清文件关系

以“输入一句文本，返回关键词匹配结果”为例。它使用现有规则节点，不需要模型权重。
先确认实际注册，再运行一次完整路径：

```bash
./build/alg_pipeline_tool catalog --biz keyword_match_v1
./build/alg_pipeline_tool describe-node TextRuleMatchNode
./build/alg_pipeline_tool validate configs/pipeline_keyword_match_rules.json
./build/alg_pipeline_tool plan configs/pipeline_keyword_match_rules.json
./build/alg_demo --biz keyword_match --config configs/pipeline_keyword_match_rules.conf --dataset tests/fixtures/effects/keyword_inputs.txt --output-dir results/business-onboarding
```

查看 `results/business-onboarding/keyword_match/results.jsonl`：请求编号为 20001–20004，
四条 `status` 均为 0，前两条 `output.is_hit` 为 true 且类别为 `SYSTEM_INIT`，后两条
为 false。`summary.json` 应有四条成功、零条失败。这一步用于认识已有接入链路；
新业务仍须换成自己的配置和输入验证。

按下表阅读对应代码，再在同一位置实现新契约；表中的名称都是这个已注册样例的名称。

| 环节 | 样例文件 | 新业务要落实的内容 |
| --- | --- | --- |
| 本地模拟平台结构 | [Operator 数据结构](../../include/platform_mock/operator_data_types.h)、[平台交互类型](../../include/platform_mock/operator_types.h) | 外部输入输出字段、长度和所有权；本目录只保存当前环境的模拟约定，真实公司定义在授权内网接入 |
| 内部数据边界 | [业务 key](../../include/adapter/biz_blackboard_keys.h)、[业务 Result](../../include/adapter/biz_results.h) | ingress/egress typed key 与接入适配层持有的结果值；已有类型可复用 |
| 输入转换器 | [text_input.cpp](../../src/adapter/input/text_input.cpp) | 外部输入校验、中性数据封装及 `REGISTER_INPUT_CONVERTER` |
| 输出转换器 | [keyword_result_output.cpp](../../src/adapter/output/keyword_result_output.cpp) | 内部结果关联、输出池租约填充及 `REGISTER_OUTPUT_CONVERTER` |
| 业务绑定与曝光 | [keyword_match_bindings.cpp](../../src/adapter/biz/keyword_match_bindings.cpp) | 声明逻辑端口映射、批次上限、`REGISTER_IO_BINDING` 与 `REGISTER_BIZ_EXPOSURE` |
| Operator 类型注册 | [operator_builtin_value_types.cpp](../../src/adapter/operator/operator_builtin_value_types.cpp) | 为新宿主类型登记规范后缀、输入校验或输出分配/重置/释放 |
| Demo 数据转换 | [keyword_match_demo.cpp](../../demo/biz/keyword_match_demo.cpp) | 读数据集、构造平台输入、复制输出字段及 `REGISTER_DEMO_BIZ` |
| 构建与部署 | [接入适配层 CMake](../../src/adapter/CMakeLists.txt)、[Demo CMake](../../demo/CMakeLists.txt)、[Pipeline](../../configs/pipeline_keyword_match_rules.json)、[部署配置](../../configs/pipeline_keyword_match_rules.conf) | 登记新增 `.cpp`，编排业务端口，配置路径和输出容量 |

其中，Pipeline 的 `biz_name`（`keyword_match_v1`）、Demo 的 `--biz`（`keyword_match`）
和 Operator 槽位后缀（`keyword_in` / `keyword_out`）用途不同。
前两者的关联由 binding 的 `BizDefinition` 声明；槽位由绑定关联到已注册宿主类型。
新名字必须分别登记，不能只改 JSON 中的显示名称。

## 3. 实现并注册转换器与绑定

参照关键词或实体抽取的转换器实现完成：

1. **实现输入转换器（`src/adapter/input/`）。**
   单槽且每请求生成一个载荷时，先写普通函数
   `AdapterStatus Decode(const Host& input, Payload* output)`，只校验业务字段并复制为自持有值。
   `DecodeInputFn` 内调用 `DecodeRequestRows<Host>`，传入槽、typed 端口、批次上限与该函数；
   框架负责槽检查、循环、批内来源编号和绑定发布。Definition 使用同一批次上限。
   文本可用 `IsValidInputString` / `CopyInputString`，PCM 的范围检查和复制仍属于业务函数。
   多槽、候选展开等算法继续使用 `ValidateDecodeRequest` / `ReadInputSlot<T>` 显式组织。
   外部请求编号保存在 `raw_request_ids`，内部批次使用批内编号，输出时恢复原编号。
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
3. **实现业务绑定与曝光声明（`src/adapter/biz/`）。**
   在 `IoBindingDefinition` 中指定 `binding_id`、`biz_name`、
   绑定的 `input_converter_id` 和 `output_converter_id`，以及逻辑端口到内部 Blackboard Key 的映射。
   使用 `REGISTER_IO_BINDING` 注册绑定。
   使用 `REGISTER_BIZ_EXPOSURE` 声明业务生产暴露：`biz_name` 与 `max_batch_size`。
4. **登记构建。**
   将新增源码加入 `src/adapter/CMakeLists.txt` 的 `edgeflow_integration_objects`。

行函数返回的错误只需携带业务原因与字段路径，包装补充 converter 和样本位置。
输入行全部通过后才开始发布；输出 writer、宿主指针和池内字符串均只在同步调用期间借用，不能保存。
输出中途失败时 `written_count=0`；租用块可能已被写入，Operator 负责不发布并归还租约。
字符串按显式长度处理，包括内嵌 NUL，不通过 `c_str()` 丢失长度。

共享端口用 `MakeBlackboardKey<T>(name)` 定义一次；转换器 Definition 使用
`RequiredInputPort(port)` / `OutputPort(port)`，回调通过 `bindings.Key(port)`
读取或发布，绑定映射使用 `BindIoPort(port)`。非同名映射使用
`BindIoPort(logical_port, actual_key)`，两端的 C++ 类型必须一致。例如审核输出使用
`BindIoPort(kMatchedPolicies, kMatchedPolicy)`，不能直接绕过绑定读写实际 key。

外部必需槽的常见写法是 `ExternalInputSlot<T>(slot)` 和
`ExternalOutputSlot<T>(slot, capacity_fields)`，类型由 traits 推导。
这两个工厂令 `type_suffix = slot_name`，`key_suffix` 留空并通过 `KeySuffix()` 回退到 `type_suffix`；
仅适用于必需槽且这三个名称相同的常见约定。输入工厂的第二参数是 `value_type`，不能用来覆盖后缀。
不同后缀、可选槽或特殊布局使用完整 `ExternalSlotDefinition`，明确填写对应字段。
例如逻辑槽名为 `result`、已注册类型后缀为 `entity_out`、外部 key 为 `sdk.answer` 时，
分别设置 `slot_name = "result"`、`type_suffix = "entity_out"`、`key_suffix = "answer"`，不能直接套同名工厂。
`schema_version`、输出 `cardinality`、`capacity_policy` 使用 Definition 已有默认值时无需再赋值；不同规则显式填写。
多槽测试可直接把
`ExternalInputBatchView` / `ExternalOutputBatchView` 交给 `AdapterHarness`，包含各槽类型和池规格。
测试中直接持有字符数组时，给 `EncodeOperator` 显式提供各字段可用容量（数组大小减去结尾 NUL 的一字节），
或用 `TestOutputBatchView::SetCapacity` 描述实际存储；`CompanyString.length` 是内容长度，不能作为容量。

实现参考直接来自参与编译和测试的八个业务；公共辅助函数按适用范围使用，业务含义留在转换器中：

| 数据形态 | 输入 / 输出实现 | 保留的业务职责 |
| --- | --- | --- |
| 纯文本、结构化结果 | [实体输入](../../src/adapter/input/text_input.cpp) / [实体输出](../../src/adapter/output/structured_document_output.cpp) | 结构化结果成功条件 |
| 规则匹配 | [关键词输入](../../src/adapter/input/text_input.cpp) / [关键词输出](../../src/adapter/output/keyword_result_output.cpp) | 命中与业务状态 |
| 完整 JSON 请求响应 | [翻译输入](../../src/adapter/input/translate_json_input.cpp) / [翻译输出](../../src/adapter/output/translation_json_output.cpp) | JSON 字段与响应协议 |
| 可选字段、三路结果 | [文档输入](../../src/adapter/input/doc_query_input.cpp) / [文档输出](../../src/adapter/output/doc_answer_output.cpp) | 文档缺省、回答/意图/片段数关联 |
| 风险判定、非同名端口 | [审核输入](../../src/adapter/input/audit_input.cpp) / [审核输出](../../src/adapter/output/audit_result_output.cpp) | 风险分数/枚举与排名校验 |
| 音频、两路结果 | [音频输入](../../src/adapter/input/audio_input.cpp) / [音频输出](../../src/adapter/output/audio_result_output.cpp) | PCM 与采样率、转写/意图组合 |
| 多个外部槽 | [图像问题输入](../../src/adapter/input/image_query_input.cpp) / [票据输出](../../src/adapter/output/invoice_result_output.cpp) | frame 的请求 ID、票据与 OCR boxes |
| 候选展开与排名 | [重排输入](../../src/adapter/input/rerank_input.cpp) / [重排输出](../../src/adapter/output/rerank_result_output.cpp) | sub_id、排名和原始索引恢复 |

## 4. Operator 类型与输出池

**ValueType 说明“这块平台内存是什么类型、如何检查和管理”，输出池负责有界租约与复用。**

1. 当前环境的模拟宿主结构先在 `platform_mock/operator_data_types.h` 声明。
   新输入、输出 DTO 均须在接入层共享头中包含 DTO 定义和 `adapter/io_converter.h`，
   在 `llm_edgeflow` 命名空间声明 `DECLARE_EXTERNAL_TYPE_TRAITS(YourDto, "YourDto");`，
   并保证该特化在所有转换器首次调用 `GetSlot<YourDto>` 前可见。
   trait 名称须与 binding 的 `external_c_type_name`、外部槽位的 `type_id` 一致；
   缺失或不一致会导致 `GetSlot` 返回空指针。已有 DTO 的 trait 直接复用。
   类型实现包含 `adapter/operator_value_type.h`，通过 `RegisterOperatorValueType` 与
   `REGISTER_OPERATOR_VALUE_TYPE` 在自己的源码中登记。
   输入使用 `MakeTypedInputBinding<T>`，只需提供后缀、类型名及类型化校验函数。
   标准字符串字段与可选 metadata 使用 `MakePooledOutputBinding<T>`：声明成员、
   默认/最大容量及标量重置函数，复用内置预算、分配与释放实现；特殊布局才手写生命周期回调。
2. 同一外层类型的不同嵌套布局可以注册命名分配方案，配置通过 `allocator` 和 `params`
   选择；用 `MakeOutputParameterParser<T>` 注册自己普通参数结构的字符串解析函数。
   配置只在最外层创建阶段读取，分配实现仅创建一份完整结构，不管理池深。

`CompanyString` 用于文本，二进制使用 `CompanyBuffer`。输入借用指针不跨调用保存；
输出提取时复制所需数据，具体生命周期按
[Operator 接口](../../include/edgeflow/operator/interface.h)执行。
上述转换都留在接入适配层，Node、Model 和 Backend 无需识别宿主结构。

## 5. 统一 Demo 接入

已有契约的新方案直接沿用对应 Demo 和数据集格式，只准备 Pipeline 与指向它的 `.conf`。
这里的数据转换仅指载体构造和结果展示，外部协议的解包、
字段选择与响应组装仍在转换器；不得把原始业务请求预先拆成内部节点输入。
新增外部结构需要统一 Demo 时，按 `keyword_match_demo.cpp` 完成以下步骤：

1. 新建 `demo/biz/<biz>_demo.cpp`，从数据集读入样本，为每条样本构造宿主输入结构，
   保持字符串及数组在同步处理期间有效。
2. 使用 `RunOperatorWithExtractor<Input, Output>`，传入声明的槽位后缀；
   在 extractor 中把输出复制到本地结果值，再交给 `ResultWriter` 输出逐条记录。
   同时复制真实 `status_code`，写入样本的 `status`，不能固定填零。Process 返回成功表示
   调用完成，业务是否逐条成功还需检查 `results.jsonl` 和 `summary.json`。
3. 用 `REGISTER_DEMO_BIZ(name, title, run_function, biz_type)` 注册，`name` 与
   `BizDefinition` 的 Demo 名一致，业务类型显式给出且非 UNKNOWN；将源码加入
   `demo/CMakeLists.txt`。无需在 `demo/main.cpp` 增加业务分支。
4. 准备样例数据、Pipeline 和 `.conf`。先用显式 `--biz`、`--config`、`--dataset` 运行。
   仅需保存可重复调用的预设或加入套件时，再向 `demo/profiles.json` 添加 Profile。

`.conf` 仅作为定位文件，包含单一字段 `pipe_path`，相对 `.conf` 所在目录解析（例如在 `configs/pipeline_keyword_match_rules.conf` 中填写 `pipeline_keyword_match_rules.json`）。宿主直接调用 Operator 时，部署根为 Create 的 `model_path`；同时在 Pipeline JSON 的 `deployment` 中核对 `model_paths` 覆盖与 `io.output_allocations` 输出容量。Profile 不会自动指向新方案，详细命令见[运行当前方案](../../tools/pipeline_studio/README.md#运行当前方案)。

Demo 的 `chip`、`device_id`、`batch_size`、`depth` 只从 Profile JSON 读取；对应 CLI
选项已删除。使用 `--profiles-file <path> --profile <name>` 选择自有配置。未选 Profile
或未提供字段时使用 `cpu`、`0`、`1`、`1`。业务、配置路径、数据集等其他 CLI 覆盖仍有效。

## 6. 输出容量与生命周期

宿主输入是借用视图，底层字符串、数组和结构体必须保持有效直到 `Process` 返回。
输出 `shared_ptr<void>` 持有的是当前 handle 的池租约，不延长 handle 的生命期。
需要保存结果时，在本次调用后复制到自己的 `std::string` / 值对象，再清空输出容器。
不要累积所有输出租约后在同一线程继续同步 `Process`：池满时调用会等待空闲块，
该线程也就无法返回释放旧租约。池深用于控制同时持有的输出数量，不是结果存储空间。

销毁顺序是：等待所有 `Process` / `Control` 返回 → 释放输出引用 → `Destroy`。
有效 handle 即使因未归还输出而在 `Destroy` 返回错误，也已被消费，不得重试或再访问
旧输出。参考 [Demo 的输出复制与释放](../../demo/biz/ocr_doc_qa_demo.cpp) 和
[公开 Operator 契约](../../include/edgeflow/operator/interface.h)。

Operator 的输出路径是 `Pipeline → 内部中性值 → OutputConverter → 已租用输出池`。
Result 与请求 Context 均不跨 Process 保存。Pipeline 的 `deployment.io.output_allocations` 按逻辑
槽位分别指定类型、`allocator`、`params` 和容量。
超过输出池容量时返回 `-4`，尚未发布的输出租约全部回滚。

在运行前查看生效的池规格与配置，`depth` 应与实际宿主一致：

```bash
./build/alg_pipeline_tool resolve-conf configs/pipeline_keyword_match_rules.conf --root . --depth 1
```

输出中的 `output_pools` 包含各槽的类型、allocator、参数、metadata 和 capacities。
复用已注册载体时只覆盖必要容量；特殊结构才需要自定义分配方案。预检证明配置与预算可以准备，
无法预测任意未来模型响应的字节数。宿主可参考现有
[Operator runner](../../demo/common/operator_runner.h)准备 required 输出 key，保持槽值为空并及时复制/释放结果。

## 7. 最小验证

完成源码登记后重新构建，再检查新业务是否进入 Catalog：

```bash
cmake --build build --target alg_sdk alg_pipeline_tool alg_demo -j 4
./build/alg_pipeline_tool catalog
```

确认 Catalog 中出现新 `biz_name`，ingress/egress 类型与定义一致；随后对**本次新增或
修改的 Pipeline** 执行 `validate`、`plan`，运行对应 Demo 并核对请求 ID、状态及业务字段。
第 2 节的关键词命令是可运行参照，实际验证时替换为新业务、配置和数据集。
有意使用测试模型时按[工具选择](../../tools/pipeline_studio/README.md#校验工具选择)
构建并使用 `alg_pipeline_tool_test`。

把断言加入相应现有套件：

| 验证范围 | 必须观察到的行为 | 参考测试 |
| --- | --- | --- |
| 转换器与契约安全 | 非法指针和长度被拒绝；结果乱序仍按来源返回，重复/缺失来源与失败结果被拒绝；输出容量越界严格拦截 | [Adapter 契约测试](../../tests/contract/abi/test_adapter_contract_security.cpp)、[Operator 安全测试](../../tests/contract/abi/test_operator_safety.cpp) |
| Operator SDK | 初始化接受完整注册；在池容量内时输出完整，超池容量时无部分发布且后续请求可复用租约 | [Operator 基础测试](../../tests/integration/operator/test_operator_api.cpp)、[公开 SDK 消费者测试](../../tests/contract/abi/test_cpp_operator_sdk.cpp) |
| Pipeline / Demo | 新业务通过校验和计划，样例结果及错误路径符合预期 | [Catalog/Validator 测试](../../tests/integration/pipeline/test_pipeline_catalog_validator.cpp)、[Demo 测试](../../tests/integration/demo/test_demo_runner.cpp) |

交付前执行 `./scripts/run_all_tests.sh`。真实模型效果与目标平台验收按
[效果验收指南](../VERIFIABLE_SELECTION.md)另行记录；涉及公司内部 SDK 时遵循
[RFC-0029](../rfcs/0029-external-readiness-and-intranet-sdk-migration.md)，当前外部工作区只准备中立接口。
