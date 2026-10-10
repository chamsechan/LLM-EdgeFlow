# Operator 宿主类型、输出池与生命周期

本文面向新增宿主类型、嵌套布局以及直接调用 SDK 的开发者。完整业务请求/响应由
[转换器](business_onboarding.md)负责；ValueType 声明平台结构及其内存管理，输出池管理有界租约。

## 选择转换器与参数

Pipeline 根 `io.input` / `io.output` 是至少含一项的数组。输入项只允许 `type`、`name`、可选 `params`；
输出项还通过 `inputs` 为转换器的每个必填逻辑端口指定 `节点名.端口名` 或 `input.端口名` 来源。
`type` 是宿主 map key 的后缀；`name` 对应平台 `service_type` 的业务取值。每个方向的 `(type, name)`
唯一，每份登记只有一个外部槽。`common` 是默认处理的保留名，查找时仍须显式选择，不自动回退。

```json
{
  "io": {
    "input": [{"type": "doc_in", "name": "doc_qa"}],
    "output": [{"type": "doc_out", "name": "doc_qa",
                "params": {"answer_text_max_bytes": 4095},
                "inputs": {"answer_text": "generate_answer.text",
                           "intent": "match_intent.matches",
                           "chunk_count": "chunk_docs.chunk_counts"}}]
  }
}
```

`.conf` 只包含 `{"pipe_path":"pipeline.json"}`。普通业务复用已登记的结构和转换器，仅覆盖必要参数。
每个输出字符串对应整数参数 `<field>_max_bytes`，最小值为 1；默认值只在转换器的
`Parameters<P>` / `MaxBytes` 中声明。平台 `OutputCapacityFieldConfig` 只持有最大容量。
Catalog/schema 从平台登记补齐 maximum；创建时在 `Prepare` 后读取声明成员的生效尺寸，
生成完整输出池规格，缺少尺寸或超过硬上限都会失败。翻译输出的默认容量为 8191 字节。

分配前检查池深、乘加溢出及 64 MiB 的句柄载荷预算。某一项的池计算或预算检查失败时，
`INVALID_OUTPUT_ALLOCATION` 指向 `/io/output/<i>`；全部池的合计预算超限指向 `/io/output`。

业务参数在 Create 中解析一次：校验与补默认值、赋值、Prepare、Validate。运行时通过
`options.Params<P>()` 读取不可变参数；`ParameterValues::Effective()` 反映 Prepare 后的声明成员，
未设置的 optional 不出现在结果中，未声明的派生成员也不出现在配置中。

槽声明固定 `allocator`、`allocator_params`、`metadata_count`、`metadata_type_id`；这些属性不进入
Pipeline 配置。另一种布局或固定参数需要另一份转换器登记。`allocator_params` 由布局解析函数在
`IoConverterRegistry::Audit()` 中归一化一次，Create 复用不可变结果，Process 不解析配置。
标准布局不接受专属参数。命名布局与其转换器匹配，转换器不再动态选择或核对任意部署布局。

`required=false` 的输出总是有独立池，宿主可按批次行省略该 key。保留的输出视图按原始行号索引，
不会压缩缺省行。所有输出转换成功后一起发布，任一失败均自动归还租约。

## 宿主 key 和多项配对

同一方向只有一项使用某个 `type` 时，key 可以采用任意非空前缀，例如 `camera.doc_in`。
多项复用同一种载体时，key 必须为 `name.type`，例如 `entity_extract.entity_out` 和
`translate.entity_out`。初始为空的输出指针因此也有明确对应关系。

各输入项按批内行号配对。平台登记用成员指针显式声明请求 ID 与业务成员；至少一个所选输入
必须带请求 ID，多个都带时逐行核对一致。非 common 的登记在结构带业务成员时声明期望值，
Process 在解码前核对 `service_type`，不一致整批失败，诊断包含结构名、行号与期望值。
`CompanyFrame` 带请求 ID，`CompanyString` 不带；图片和问题可以分别由两份登记发布不同逻辑端口。

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
    "summary", {{"summary", &SummaryOutput::summary, {65536}}},
    [](SummaryOutput& value) noexcept { value.status = 0; }));
```

容量结构只声明最大值。默认值在输出转换器中使用
`MaxBytes("summary", &Params::summary_max_bytes).Default(4096)` 声明。成员声明用于容量校验、预算、分配和
重置；标量回调必须 `noexcept`，只重置标量，不能覆盖嵌套指针。输入对应使用
`MakeTypedInputBinding<T>`，回调直接接收 `const T&` 和 `InputLimits`。
完整可执行示例见[输出池测试](../../tests/unit/operator/test_operator_output_pool.cpp)。

特殊嵌套布局的输出方案提供以下行为：

1. `normalize_parameters`：通过 `MakeOutputParameterParser<YourParameters>(parse)`
   注册字符串解析函数。`parse` 的签名是
   `bool(const std::string&, YourParameters*, std::string*)`，只负责自己的字段、
   枚举、容量与默认值。框架在转换器注册审计时调用一次，并包装、持有不可变结果。
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

默认实现和命名方案必须声明相同的外层类型名称。`service_type` 的成员声明及读写回调
统一来自宿主 ValueType 登记，命名方案无需重复声明；更换布局不改变转换器的业务值要求。
命名方案拥有自己的参数校验、布局、
预算和生命周期回调。注册必须在 Operator Init 前完成，重复标识、类型不兼容和缺失
必要回调会被拒绝。默认的 `CompanyAny` 数值类型表不会因此自动获得任意指针树能力。

部署计划持有所选绑定的值副本，输出池也拥有生命周期回调，后续使用无需重新查表。
回调捕获的数据应按值持有或显式共享；引用捕获的对象仍须覆盖所有计划和输出池的使用期。

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

通过 `REGISTER_OUTPUT_CONVERTER` 注册 `OutputConverterDefinition`，槽声明固定该实现写入的布局。
回调从 `ExternalOutputBatchView` 按登记 type 获取已分配外层结构与 `ResolvedOutputPoolSpec`，
按相应类型化参数填充载荷。保持指针与已分配布局一致，不重新读取部署文件、不另设默认容量，
也不把请求局部指针塞进输出结构。`OutputStringWriter` 按实际容量与显式长度写字符串。

框架在全部输出转换成功后发布 map；任何失败都会归还已获取的输出租约。调用方依照外部协议的
枚举解释 `void*`，实际释放依据登记的所有权记录。输出引用不延长 handle 的有效期。

## 宿主调用与生命周期

本节写给直接调用 SDK 的宿主程序。转换器作者需要的批次上限与输出容量配置见
[业务接入指南](business_onboarding.md#6-输出容量)。

**调用前提。** `Process` 的输入、输出批次必须非空且帧数相等，单次批次不超过有效上限，
超出时直接失败，不会在门面中自动拆批。每帧按所选转换器提供必需的输入槽和输出槽，可选槽按契约省略。
提供的输出 key 预先存在且值为 null `shared_ptr`，不能传入上一批尚未释放的输出指针。
宿主类型、业务值和必需性可通过公开 `ResolveOperatorConfigIo` 预检；它与 Create 使用同一准备链路，
不加载模型。批次有效上限为 `min(max_frame_depth, 64)`。

**借用输入与输出租约。** `CompanyString` 用于文本，二进制使用 `CompanyBuffer`。
宿主输入是借用视图，底层字符串、数组和结构体必须保持有效直到 `Process` 返回；库不跨调用保存输入指针。
输出 `shared_ptr<void>` 持有的是当前 handle 的池租约，不延长 handle 的生命期。
需要保存结果时，在本次调用后复制到自己的 `std::string` / 值对象，再清空输出容器。
不要累积所有输出租约后在同一线程继续同步 `Process`：池满时调用会等待空闲块，
该线程也就无法返回释放旧租约。池深用于控制同时持有的输出数量，不是结果存储空间。

**销毁顺序。** 等待所有 `Process` / `Control` 返回 → 释放输出引用 → `Destroy`。
有效 handle 即使因未归还输出而在 `Destroy` 返回错误，也已被消费，不得重试或再访问
旧输出。参考 [Demo 的输出复制与释放](../../demo/common/operator_runner.h) 和
[公开 Operator 契约](../../include/edgeflow/operator/interface.h)。

**`Init`、`DeInit` 与并发。** `Init` 用于注册审计，应在创建实例前调用；审计失败返回 `-6`，
`GetOperatorLastError()` 与日志逐条列出来源（注册表或 Converter 审计）和原因。同一 handle 的 `Process` 与
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
执行失败的诊断还包含节点名 与类型，模型调用失败时包含模型操作和模型返回码。`-100` 不区分
执行失败与未知异常，需结合诊断判断故障层。公开错误码见
[`error_codes.h`](../../include/platform_mock/error_codes.h)，它是外网环境的替身，真实 SDK 的
目标码须在授权内网核验。
`Process` 失败后在当前线程读取 `GetOperatorLastError()` 并及时复制诊断；它返回的指针由库持有，
后续调用可能更新内容。成功返回不代表已完成真实模型效果或生产验收。

宿主可参考现有 [Operator runner](../../demo/common/operator_runner.h) 准备 required 输出 key，
保持槽值为空并及时复制、释放结果。
