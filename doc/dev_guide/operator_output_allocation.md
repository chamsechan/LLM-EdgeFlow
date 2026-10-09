# Operator 宿主类型、输出池与生命周期

本文写给三类读者：新增宿主类型或输出布局的作者、配置多输出与嵌套载荷的作者，以及直接调用
SDK 的宿主集成方。只复用已有类型、编写转换器登记的业务开发者，见[业务接入指南](business_onboarding.md)。

ValueType 说明“这块平台内存是什么类型、如何检查和管理”，输出池负责有界租约与复用。
普通业务复用已注册输出结构，只在需要时覆盖字符串尺寸参数。新增标准结构时，通过
`MakePooledOutputBinding<T>` 声明字段、平台上限及标量重置；框架负责分配和释放。
特殊嵌套结构才需要描述一份完整输出的布局与生命周期。框架负责创建
多少份（采用 Create 的实际池深）、租约和队列。同一个 map 键、同一个外层 C 结构，可以由不同的输出转换器登记
选择不同的嵌套 `void*` 布局：布局和布局参数写在登记的槽声明里，方案的 `io.output` 选择哪个登记，
就得到哪种布局；选择在 Create 固定，Process 使用同一份解析结果进行结果转换。

## 开发者需要实现什么

先按场景确定要新增的内容。三种场景都只新增代码或登记项，不改已有实现，也不影响没有选用
新内容的已有部署配置：

| 场景 | 新增什么 | 不需要改什么 | 隔离由谁保证 |
| --- | --- | --- | --- |
| 复用已有宿主类型 | 转换器登记 | ValueType、输出池和其他业务 | — |
| 新的平台宿主类型 | 平台结构、type traits，以及 `operator_builtin_value_types.cpp` 中的一项登记 | 其他类型及其部署配置 | 类型后缀唯一；Init 时审计全部登记，并核对槽位结构与登记结构一致 |
| 已有类型的新嵌套布局 | 自己 `.cpp` 中的命名方案，以及在槽声明中固定该方案的转换器登记 | 类型的默认实现、未选用该登记的部署 | 方案由登记固定，方案名和布局参数在注册审计时核对、解析；方案由 `io` 选择登记 |

新的平台宿主类型若只有标准 `CompanyString*` 字段和可选 `CompanyAny*`，用
`MakePooledOutputBinding<T>` 声明成员与容量即可。其他嵌套布局提供自己的普通参数结构
（包括布局枚举）、解析、单份分配/重置与载荷预算，再注册实现。
`OwnedExternalBlock` 负责已登记内存的自动释放与失败回滚。
参数类不需要继承框架基类，也不需要实现 `ToJson()`。

同一实现内可调的取值（容量、布局枚举）写在登记槽声明的 `allocator_params`（JSON 文本）中，
需要另一组取值时新增一个登记；需要独立的分配、重置或析构逻辑时，
新增命名方案，不在已有实现中加分支。

注册审计用所选布局自带的解析函数解析 `allocator_params`（未设置时为 `{}`），写错在 `Init` 报错，
早于 Create；解析结果在 Create 写入输出池规格，交给分配与转换回调。字符串值保留 JSON 引号及转义，
数组、对象和标量保持各自含义。解析函数可以选用所需的解析库，公共扩展头不包含 JSON 类型。业务实现不读取
部署文件。最终业务输出仍按注册的外层结构和嵌套布局返回。

隔离性由 [Operator 接口测试](../../tests/integration/operator/test_operator_api.cpp) 验证：
`SameOutputKeysSelectIndependentNestedAllocatorsPerHandle`（各 handle 经由不同登记独立选择方案）、
`NestedOutputConfigurationIsValidatedBeforeAllocation`（非法参数在分配前被拒绝）、
`UndeclaredLayoutParametersUseTheAllocatorDefaults`（登记未声明布局参数时使用方案默认值）。

## 选择参数

输出转换器登记的槽声明 `ExternalSlotDefinition` 固定以下内容，它们不进入方案配置：

| 字段 | 用途 |
| --- | --- |
| `type_suffix` | 外部 map key 最后一个点号后的部分，也是登记的 `type`；同时选择注册的外层 ValueType |
| `type_id` | 宿主结构名，必须等于 `type_suffix` 所登记的结构，不一致时 Init 审计报错 |
| `required` | 是否必需；`false` 的可选输出总是分配输出池，宿主每次调用都可以省略这个 key |
| `allocator` | 为该外层类型注册的分配方案标识；为空时使用类型的标准布局 |
| `allocator_params` | 由方案解释的单份布局参数（JSON 文本），注册审计时解析，例如嵌套枚举与数组容量 |
| `metadata_count` / `metadata_type_id` | 结构体带 `CompanyAny*` metadata 成员时分配的元素数和类型；结构体没有 metadata 成员时必须为 0。目前只有 `CompanyOdOutput`，生产登记都为 0，仍属待内网核对的替身字段 |

字符串字段的容量不在槽声明里：输出结构的每个字符串字段由登记的 `<字段>_max_bytes` 尺寸参数决定
（`MaxBytes("field", ...)`，默认值须在 1 到平台上限之间），方案的 `io.output` 项用 `params` 覆盖。
这样转换器与它写入的布局一定匹配，不需要在运行时核对 `spec.allocator`。

转换器声明槽位时，`ExternalInputSlot<T>(type)` / `ExternalOutputSlot<T>(type)` 令
`type_suffix = type`，输出槽使用标准布局；命名布局、metadata 或可选槽直接在返回的
`ExternalSlotDefinition` 上设置相应字段。Catalog 中的 `external_type` 是登记的宿主结构名。

同侧复用同一 `type` 时，以 `<name>.<type>` 区分宿主输出 key；每项拥有独立的输出池、
参数与布局规格，converter 仍以本项的 `type_suffix` 读取槽。
可选槽省略的行在视图中保持 `nullptr`，不压缩批次索引。编码函数跳过这些目标，
成功时 `written_count` 等于实际写入的非空目标数；任一项失败时所有租约归还且不发布输出。

以下登记来自参与编译的
[测试接入](../../tests/integration/operator/test_operator_api.cpp)和
[嵌套结构实现](../../tests/support/operator_nested_output_fixture.h)。这些类型和登记只在
测试程序中注册，用于说明扩展方式，不是生产 SDK 中可选的新业务。

```cpp
OutputConverterDefinition def;
def.type = "test_nested_out";   // 宿主 map key 后缀
def.name = "standard_pair"; // 测试专用的业务名
def.slot.type_id = "NestedOutputEnvelope";
def.slot.type_suffix = def.type;
def.slot.allocator = "test_nested_standard";
def.slot.allocator_params = R"({"kind":1,"capacity":2})";
```

方案只写 `io`；`.conf` 仅保存 `{"pipe_path":"pipeline.json"}`：

```json
{
  "io": {
    "input": [ { "type": "keyword_in", "name": "keyword_match" } ],
    "output": [
      { "type": "test_nested_out",  "name": "standard_pair" },
      { "type": "test_nested_audit_out", "name": "standard_pair" }
    ]
  }
}
```

示例中两个输出登记的 `type`（key 后缀）分别是 `test_nested_out`、`test_nested_audit_out`，调用方准备
`outputs[i]["chan.test_nested_out"]` 与 `outputs[i]["chan.test_nested_audit_out"]` 两个空 shared_ptr。
二者都指向 `NestedOutputEnvelope`，其 `void* payload` 指向下一层结构，后者的
`void* values` 再根据 `kind` 指向整数或浮点数组。把 `name` 换成另一个登记（例如使用另一种方案的
`alternate_pair`）后创建另一个 handle，map 键仍保持不变。

必需输出项省略 `params` 时使用登记的默认尺寸，布局与 metadata 总是登记固定的值；类型完全来自槽声明。
逻辑名和有效 map 后缀分别唯一；不同登记可以复用相同类型和方案，各自使用独立容量和输出池。
容量以实际载荷字节数为准，不能由 token 数
直接换算；超限检测、池预算、租约、失败回滚及全部转换成功后发布仍由框架执行。

## 实现与注册

新业务继续使用已注册宿主类型时，复用其 ValueType 与内存管理，载荷协议变化由转换器处理；
已有 DTO 的 trait 也直接复用。只有需要新宿主类型或分配布局时，才执行本节步骤。

平台宿主类型集中维护，三处一一对应：结构声明在
[`operator_data_types.h`](../../include/platform_mock/operator_data_types.h)（当前环境的模拟定义，
真实公司定义在授权内网接入），type traits 声明在 [`io_converter.h`](../../include/adapter/io_converter.h)，
ValueType 登记在 [`operator_builtin_value_types.cpp`](../../src/adapter/operator/operator_builtin_value_types.cpp)。
新增类型时三处各加一项，不改已有条目。内网接入时，宿主类型的 traits 与 ValueType 登记在这两处
逐项对照真实头文件即可；接口、枚举、所有权、线程与错误语义等其余核对见
[验收范围](../VERIFIABLE_SELECTION.md#验收范围与发布准备)。

为已有类型新增命名方案时，包含 `adapter/operator_value_type.h`，在接入层自己的 `.cpp` 中构造
`OperatorValueTypeBinding`，在注册函数中调用 `RegisterOperatorOutputAllocator(name, binding)`，
再用 `REGISTER_OPERATOR_OUTPUT_ALLOCATOR` 登记该函数。源码放在 `src/adapter/output/` 下会自动编入；
不需要包含私有 registry 或 pool 头，也不改该类型的默认登记。测试专用类型在测试源码中用
`RegisterOperatorValueType` 和 `REGISTER_OPERATOR_VALUE_TYPE` 登记。

常见输出不需要手写以下生命周期回调。例如，假设平台新增结构 `SummaryOutput`，包含
`CompanyString* summary` 和标量 `status`。先在 `io_converter.h` 中与其他平台类型一起声明 trait：

```cpp
DECLARE_EXTERNAL_TYPE_TRAITS(SummaryOutput, "SummaryOutput");
```

新输入 DTO 也需要相同的 trait 声明。结构名只在这里写一次：ValueType 登记和转换器槽位
（`ExternalInputSlot<T>` / `ExternalOutputSlot<T>`）都从 trait 取名，未声明 trait 时登记处直接编译失败。
槽位声明的结构与其后缀登记的结构不一致时，Init 审计报错并指明槽位，不会按错误的布局读写内存。

然后在 `operator_builtin_value_types.cpp` 的 `RegisterBuiltinBindings` 中登记一项：

```cpp
RegisterBinding(MakePooledOutputBinding<SummaryOutput>(
    "summary", {{"summary", &SummaryOutput::summary, 65536}},
    [](SummaryOutput& value) noexcept { value.status = 0; }));
```

容量只声明平台上限；默认尺寸属于输出转换器的 `MaxBytes("summary", ...).Default(4096)` 参数。成员声明同时用于配置校验、预算、分配和
重置；标量回调必须 `noexcept`，只重置标量，不能覆盖嵌套指针。输入对应使用
`MakeTypedInputBinding<T>`，回调直接接收 `const T&` 和 `InputLimits`。
完整可执行示例见[输出池测试](../../tests/unit/operator/test_operator_output_pool.cpp)。

特殊嵌套布局的输出方案提供以下行为：

1. `normalize_parameters`：通过 `MakeOutputParameterParser<YourParameters>(parse)`
   注册字符串解析函数。`parse` 的签名是
   `bool(const std::string&, YourParameters*, std::string*)`，只负责自己的字段、
   枚举、容量与默认值。框架在 Create 时调用一次，并包装、持有不可变结果。
   `YourParameters` 是普通 C++ 结构，无继承和反向序列化要求。
   没有该回调的方案只接受空参数文本或文本 `{}`。
2. `output_layout.compute_block_payload_bytes`：计算一份根结构及全部嵌套存储的
   载荷字节数，检查乘加溢出。框架负责乘池深并累计整个 handle 的预算。
3. `allocate_external`：通过 `spec.Parameters<ConcreteParameters>()` 读取类型化
   参数，创建一份根结构及嵌套内存，设置对应
   枚举，将根指针写入 `block->raw_struct`。`block->Own(std::make_unique<T>())`
   与 `OwnArray(std::make_unique<T[]>(n))` 同时登记释放动作；中途失败自动清理。
4. `reset_external`：在归还时清空有效内容，保留已分配的指针、容量及布局枚举，
   准备复用。该回调应无异常、无分配。
5. `destroy_external`：最终销毁单份输出，通常调用 `block->Destroy()`；释放按
   登记的逆序执行。分配器不同的内存必须登记匹配的 deleter，避免同时递归释放和
   逐项释放同一个指针。

默认实现和命名方案必须声明相同的外层类型名称；命名方案拥有自己的参数校验、布局、
预算和生命周期回调。注册必须在 Operator Init 前完成，重复标识、类型不兼容和缺失
必要回调会被拒绝。默认的 `CompanyAny` 数值类型表不会因此自动获得任意指针树能力。

例如参与编译的嵌套结构示例用如下方式登记解析：

```cpp
binding.normalize_parameters =
    MakeOutputParameterParser<NestedOutputParameters>(ParseNestedOutput);
```

`NestedOutputParameters` 只保存布局类型、容量和该测试需要的业务标记；解析后的对象
由分配、预算和转换回调通过 `spec.Parameters<NestedOutputParameters>()` 共用。
只有注册新结构的作者需要写这份解析逻辑，使用已有结构的业务无需重复实现。

配置文本留在最外层解析与工具展示边界，输出池共享不可变参数对象，不持有或重新
解析 JSON。具体参数对象应只使用析构无需分配的普通 RAII 字段；字符串字段用
`std::string` 持有内容，不保留指向解析输入的指针或 `string_view`。直接验证类型实现时，
可先调用 `NormalizeOutputParameters(binding, text, ...)`，再将解析结果交给分配与
预算回调；不在每次分配时重复解析配置。

## 转换与有效期

通过 `REGISTER_OUTPUT_CONVERTER` 注册含 `encode_fn` 回调的 `OutputConverterDefinition`，
登记的槽声明已经固定命名布局。回调从 `ExternalOutputBatchView`
按槽位读取已分配的外层结构与 `ResolvedOutputPoolSpec`，按同一份
`allocator` 和类型化参数填充载荷。转换必须保持指针与已分配布局一致，不重新读取
部署文件、不另设默认容量，也不把请求局部指针塞进输出结构。

命名布局由登记的槽声明固定，转换器与它写入的布局一定匹配；要支持另一种布局，就新增一个登记。
参照[嵌套结构夹具](../../tests/support/operator_nested_output_fixture.h)中 `ConvertNestedOutput` 的写法。

框架在全部输出转换成功后发布 map；任何一项失败都会归还已经获取的输出租约。
调用方读取时依照外部协议的枚举解释 `void*`；内存的实际清理依据已登记的所有权
记录。输出引用不延长 handle 的有效期，销毁顺序见下文
[宿主调用与生命周期](#宿主调用与生命周期)。

`alg_pipeline_tool resolve-conf` 在 `configuration.output_pools` 按槽（登记的 `type`）展示有效
方案与容量，`params` 是交给结构体解析函数的**字符串**（例如
`"{\"kind\":1,\"capacity\":8}"`），不包含该解析函数内部补齐的默认值。单输出同样通过 `output_pools` 按槽读取。
现有 Demo/Studio Profile 使用原单输出
业务；新多输出业务由其宿主调用或相应 Demo 扩展验证。

## 宿主调用与生命周期

本节写给直接调用 SDK 的宿主程序。转换器作者需要的批次上限与输出容量配置见
[业务接入指南](business_onboarding.md#6-输出容量)。

**调用前提。** `Process` 的输入、输出批次必须非空且帧数相等，单次批次不超过有效上限，
超出时直接失败，不会在门面中自动拆批。每帧按 `io` 选中的登记提供必需的输入槽和输出槽，可选槽按契约省略。
提供的输出 key 预先存在且值为 null `shared_ptr`，不能传入上一批尚未释放的输出指针。
有效 key 后缀与宿主类型可通过 `catalog --io-binding <业务名>` 查询，后缀与类型的区别见
[选择参数](#选择参数)。

**借用输入与输出租约。** `CompanyString` 用于文本，二进制使用 `CompanyBuffer`。
宿主输入是借用视图，底层字符串、数组和结构体必须保持有效直到 `Process` 返回；库不跨调用保存输入指针。
输出 `shared_ptr<void>` 持有的是当前 handle 的池租约，不延长 handle 的生命期。
需要保存结果时，在本次调用后复制到自己的 `std::string` / 值对象，再清空输出容器。
不要累积所有输出租约后在同一线程继续同步 `Process`：池满时调用会等待空闲块，
该线程也就无法返回释放旧租约。池深用于控制同时持有的输出数量，不是结果存储空间。

**销毁顺序。** 等待所有 `Process` / `Control` 返回 → 释放输出引用 → `Destroy`。
有效 handle 即使因未归还输出而在 `Destroy` 返回错误，也已被消费，不得重试或再访问
旧输出。参考 [Demo 的输出复制与释放](../../demo/output/od_output.cpp) 和
[公开 Operator 契约](../../include/edgeflow/operator/interface.h)。

**`Init`、`DeInit` 与并发。** `Init` 用于注册审计，应在创建实例前调用；审计失败返回 `-6`，
`GetOperatorLastError()` 与日志逐条列出来源（注册表或转换器审计）和原因。同一 handle 的 `Process` 与
`Control` 串行，不同 handle 可并行。`DeInit` 会清理该库实例中登记的**所有 handle**，不是单个调用方的
局部清理。调用前须停止所有实例的新调用、等待在途调用返回并释放全部输出；不支持与 Create、Process、
Control 或 Destroy 并发使用。存在未归还输出时它返回错误，但已清理的 handle 和旧输出仍失效。

**错误与诊断。** 门面自身的检查返回无效 handle `-1`、非法创建参数/配置 `-2`、非法输入或批次 `-3`、
输出槽/容量错误 `-4` 和不支持的 Control 命令 `-7`。门面之下的失败按阶段映射为公开返回码，
内部的 Pipeline、Node、Model 错误码不直接返回给宿主：

| 失败阶段 | 返回码 |
| --- | --- |
| Create：配置与部署校验、模型或 Backend 加载 | `-2`；不支持的业务 `-5`、注册冲突 `-6` 保持原类别 |
| Process：节点或模型执行失败 | `-100` |
| Control：信封、目标节点或载荷 schema 不合法 | `-2` |
| Control：没有节点声明或处理该命令 | `-7` |
| Control：节点拒绝或未能应用更新 | `-100` |
| 异常屏障捕获异常 | `-99`（`std::exception`）或 `-100`（未知异常） |

`GetOperatorLastError()` 以 `<阶段> failed with internal code <内部码>: <诊断>` 保留原始码；
执行失败的诊断还包含节点 ID 与类型，模型调用失败时包含模型操作和模型返回码。`-100` 不区分
执行失败与未知异常，需结合诊断判断故障层。公开错误码见
[`error_codes.h`](../../include/platform_mock/error_codes.h)，它是外网环境的替身，真实 SDK 的
目标码须在授权内网核验。
`Process` 失败后在当前线程读取 `GetOperatorLastError()` 并及时复制诊断；它返回的指针由库持有，
后续调用可能更新内容。成功返回不代表已完成真实模型效果或生产验收。

宿主可参考现有 [Operator runner](../../demo/common/operator_runner.h) 准备 required 输出 key，
保持槽值为空并及时复制、释放结果。
