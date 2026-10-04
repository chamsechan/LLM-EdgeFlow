# Operator 宿主类型、输出池与生命周期

本文写给三类读者：新增宿主类型或输出布局的作者、配置多输出与嵌套载荷的作者，以及直接调用
SDK 的宿主集成方。只复用已有类型、编写转换器与绑定的业务开发者，见[业务接入指南](business_onboarding.md)。

ValueType 说明“这块平台内存是什么类型、如何检查和管理”，输出池负责有界租约与复用。
普通业务复用已注册输出结构，只在需要时覆盖容量。新增标准结构时，通过
`MakePooledOutputBinding<T>` 声明字段、默认/最大容量及标量重置；框架负责分配和释放。
特殊嵌套结构才需要描述一份完整输出的布局与生命周期。框架负责创建
多少份（采用 Create 的实际池深）、租约和队列。同一个 map 键、同一个外层 C 结构，可以在不同
handle 的配置中选择不同的嵌套 `void*` 布局；配置在 Create 固定，Process 使用
同一份规范化配置进行结果转换。

## 开发者需要实现什么

复用已有外层结构和布局时，只编写业务转换，选择已注册的方案即可。接入新结构时，
先判断是否只是标准 `CompanyString*` 字段和可选 `CompanyAny*`：这类结构使用
`MakePooledOutputBinding<T>` 声明成员与容量即可。其他嵌套布局提供自己的普通参数结构
（包括布局枚举）、解析、单份分配/重置与载荷预算，再注册实现。
`OwnedExternalBlock` 负责已登记内存的自动释放与失败回滚。
参数类不需要继承框架基类，也不需要实现 `ToJson()`。

配置文件读取由最外层 `OperatorConfigResolver` 负责，配置提取使用独立的接入组件
`OutputConfigReader`。它在 Create 阶段运行，不是业务 Pipeline 中的 Node。
接口位于 `adapter/operator_output_config.h`：

```cpp
std::string text;
reader.Read(OutputConfigField::kParameters, &text, &error);
```

可选字段是该组件固定的枚举：`kAllocator`、`kParameters`、`kCapacities`、
`kMetadataCount`、`kMetadataTypeId`。这些枚举选择框架配置项；具体载荷的布局枚举
由结构体作者定义在自己的参数中。业务实现不传 JSON 路径，不读取整个部署文件。

JSON 读取器将选中值通过 `dump()` 转为拥有自身存储的 `std::string`；字符串值
保留 JSON 引号及转义，数组、对象和标量保持各自含义。未设置 `params` 返回文本
`{}`。注册方案只收到这一份参数文本，由创建阶段的解析函数转换为自己的参数结构。
解析函数可以选用所需的解析库；公共参数接口不包含 JSON 类型。其他配置载体可实现
同一读取接口。最终业务输出仍按注册的外层结构和嵌套布局返回。

## 选择参数

| 字段 | 用途 |
| --- | --- |
| 槽位 `slot_name` | 业务中的输出槽位，也是 `deployment.io.out_mem` 的配置键 |
| 槽位 `key_suffix` | 外部 map key 最后一个点号后的部分；`ExternalSlotDefinition` 可显式指定，为空时由 `KeySuffix()` 使用 `type_suffix`。常见槽位工厂令 `type_suffix = slot_name`；异名槽位使用完整定义 |
| 槽位 `type_suffix` | 注册定义指定的外层 ValueType；配置不再填写 `type` |
| `allocator` | 为该外层类型注册的分配方案标识；省略时使用类型的默认实现 |
| `params` | 由方案解释、校验并补齐的单份布局参数，例如嵌套枚举与数组容量 |
| `capacities` / `meta_num` / `metadata_type_id` | 方案声明的标准字符串与 CompanyAny 容量字段 |

转换器声明槽位时，`ExternalInputSlot<T>(slot)` / `ExternalOutputSlot<T>(slot)` 令
`type_suffix = slot_name`，`key_suffix` 留空并回退到 `type_suffix`，只适用于必需槽且三个名称相同的
常见约定；输入工厂的第二参数是 `value_type`，不能用来覆盖后缀。其他情况使用完整的
`ExternalSlotDefinition`。例如逻辑槽名为 `result`、已注册类型后缀为 `entity_out`、外部 key 为
`sdk.answer` 时，分别设置 `slot_name = "result"`、`type_suffix = "entity_out"`、`key_suffix = "answer"`。
`schema_version`、输出 `cardinality`、`capacity_policy` 使用 Definition 的默认值时无需赋值；
规则不同时显式填写。

以下配置来自参与编译的
[测试接入](../../tests/integration/operator/test_operator_api.cpp)和
[嵌套结构实现](../../tests/support/operator_nested_output_fixture.h)。这些类型和业务只在
测试程序中注册，用于说明扩展方式，不是生产 SDK 中可选的新业务。

下面是 Pipeline 文档的 `deployment` 部分；`.conf` 仅保存 `{"pipe_path":"pipeline.json"}`。

```json
{
  "deployment": {
    "io": {
      "io_binding": "<已注册的测试绑定>",
      "out_mem": {
        "main": {
          "allocator": "test_nested_standard",
          "params": {"kind": 1, "capacity": 8}
        },
        "audit": {
          "allocator": "test_nested_alternate",
          "params": {"kind": 2, "capacity": 16}
        }
      }
    }
  }
}
```

示例接入绑定 将两个槽位的 `key_suffix` 分别注册为 `result`、`audit`，调用方准备
`outputs[i]["chan.result"]` 与 `outputs[i]["chan.audit"]` 两个空 shared_ptr。
二者都指向 `NestedOutputEnvelope`，其 `void* payload` 指向下一层结构，后者的
`void* values` 再根据 `kind` 指向整数或浮点数组。修改 `main` 的方案或参数后创建
另一个 handle，map 键仍可保持 `chan.result`。

必需输出槽省略配置时使用已注册的默认 allocator、容量和零 metadata。只有修改默认值时
才填写 `deployment.io.out_mem`；类型完全来自槽位定义，旧 `type` 字段会被拒绝。
`required=false` 的可选输出需要显式槽配置来启用，`{}` 表示采用默认值；Process 可省略该输出。
逻辑名和有效 map 后缀分别唯一；不同槽位可以复用相同类型和方案，各自使用独立容量和输出池。
自定义 allocator 若要求布局参数，省略参数仍会报错。容量以实际载荷字节数为准，不能由 token 数
直接换算；超限检测、池预算、租约、失败回滚及全部转换成功后发布仍由框架执行。

## 实现与注册

新业务继续使用已注册宿主类型时，复用其 ValueType 与内存管理，载荷协议变化由转换器处理；
已有 DTO 的 trait 也直接复用。只有需要新宿主类型或分配布局时，才执行本节步骤。
当前环境的模拟宿主结构先在 `include/platform_mock/operator_data_types.h` 声明；真实公司定义
在授权内网接入。

包含 `adapter/operator_value_type.h`，在接入层自己的 `.cpp` 中建立
`OperatorValueTypeBinding`。外层类型首次接入时调用 `RegisterOperatorValueType`；
为该类型新增命名方案时调用 `RegisterOperatorOutputAllocator(name, binding)`。
分别通过 `REGISTER_OPERATOR_VALUE_TYPE`、`REGISTER_OPERATOR_OUTPUT_ALLOCATOR`
登记无参注册函数。源码放在 `src/adapter/input/`、`output/` 或 `biz/` 下会自动编入；
放在 `src/adapter/operator/` 等框架机制目录时，需要在 `src/adapter/CMakeLists.txt` 中登记。
不需要包含私有 registry 或 pool 头，也不需要在中央分发表增加业务判断。

常见输出不需要手写以下生命周期回调。例如，假设接入的 DTO `SummaryOutput` 包含
`CompanyString* summary` 和标量 `status` 时，先在接入层相关转换器共享的头文件中，
包含 DTO 定义和 `adapter/io_converter.h`，在首次使用 `GetSlot<SummaryOutput>` 前声明：

```cpp
namespace llm_edgeflow {
DECLARE_EXTERNAL_TYPE_TRAITS(SummaryOutput, "SummaryOutput");
}
```

新输入 DTO 也需要相同的 trait 声明。名称必须与 binding 的 `external_c_type_name`
及转换器外部槽位的 `type_id` 一致；运行时注册 binding 不会自动声明 C++ trait，
缺失或名称不一致会使输入、输出视图的 `GetSlot<T>` 返回空指针。

然后在接入层 `.cpp` 的注册函数中构造并登记 binding：

```cpp
auto binding = MakePooledOutputBinding<SummaryOutput>(
    "summary", "SummaryOutput",
    {{"summary", &SummaryOutput::summary, {4096, 65536}}},
    [](SummaryOutput& value) noexcept { value.status = 0; });
RegisterOperatorValueType(binding);
```

容量结构的顺序是默认值、最大值。成员声明同时用于配置校验、预算、分配和
重置；标量回调必须 `noexcept`，只重置标量，不能覆盖嵌套指针。输入对应使用
`MakeTypedInputBinding<T>`，回调直接接收 `const T&` 和 `ResolvedInputLimits`。
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
再由 `IoBindingDefinition` 绑定转换器与命名输出槽位。回调从 `ExternalOutputBatchView`
按槽位读取已分配的外层结构与 `ResolvedOutputPoolSpec`，按同一份
`allocator` 和类型化参数填充载荷。转换必须保持指针与已分配布局一致，不重新读取
部署文件、不另设默认容量，也不把请求局部指针塞进输出结构。

框架在全部输出转换成功后发布 map；任何一项失败都会归还已经获取的输出租约。
调用方读取时依照外部协议的枚举解释 `void*`；内存的实际清理依据已登记的所有权
记录。输出引用不延长 handle 的有效期，销毁顺序见下文
[宿主调用与生命周期](#宿主调用与生命周期)。

`alg_pipeline_tool resolve-conf` 在 `configuration.output_pools` 按逻辑槽位展示有效
方案与框架容量，`params` 是交给结构体解析函数的**字符串**（例如
`"{\"kind\":1,\"capacity\":8}"`），不包含该解析函数内部补齐的默认值。单输出同样通过 `output_pools` 按槽位读取。
现有 Demo/Studio Profile 使用原单输出
业务；新多输出业务由其宿主调用或相应 Demo 扩展验证。

## 宿主调用与生命周期

本节写给直接调用 SDK 的宿主程序。转换器作者需要的批次上限与输出容量配置见
[业务接入指南](business_onboarding.md#6-输出容量)。

**调用前提。** `Process` 的输入、输出批次必须非空且帧数相等，单次批次不超过有效上限，
超出时直接失败，不会在门面中自动拆批。每帧按接入绑定提供必需的输入槽和输出槽，可选槽按契约省略。
提供的输出 key 预先存在且值为 null `shared_ptr`，不能传入上一批尚未释放的输出指针。
有效 key 后缀与宿主类型可通过 `catalog --io-binding <binding_id>` 查询，后缀与类型的区别见
[选择参数](#选择参数)。

**借用输入与输出租约。** `CompanyString` 用于文本，二进制使用 `CompanyBuffer`。
宿主输入是借用视图，底层字符串、数组和结构体必须保持有效直到 `Process` 返回；库不跨调用保存输入指针。
输出 `shared_ptr<void>` 持有的是当前 handle 的池租约，不延长 handle 的生命期。
需要保存结果时，在本次调用后复制到自己的 `std::string` / 值对象，再清空输出容器。
不要累积所有输出租约后在同一线程继续同步 `Process`：池满时调用会等待空闲块，
该线程也就无法返回释放旧租约。池深用于控制同时持有的输出数量，不是结果存储空间。

**销毁顺序。** 等待所有 `Process` / `Control` 返回 → 释放输出引用 → `Destroy`。
有效 handle 即使因未归还输出而在 `Destroy` 返回错误，也已被消费，不得重试或再访问
旧输出。参考 [Demo 的输出复制与释放](../../demo/biz/ocr_invoice_qa_demo.cpp) 和
[公开 Operator 契约](../../include/edgeflow/operator/interface.h)。

**`Init`、`DeInit` 与并发。** `Init` 用于注册审计，应在创建实例前调用。同一 handle 的 `Process` 与
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
