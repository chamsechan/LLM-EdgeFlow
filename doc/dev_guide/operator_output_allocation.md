# Operator 多输出与嵌套载荷分配

业务开发者通过注册的实现描述**一份完整输出如何创建、重置和释放**。框架负责创建
多少份（默认 25）、租约和队列。同一个 map 键、同一个外层 C 结构，可以在不同
handle 的配置中选择不同的嵌套 `void*` 布局；配置在 Create 固定，Process 使用
同一份规范化配置进行结果转换。

## 开发者需要实现什么

复用已有外层结构和布局时，只编写业务转换，选择已注册的方案即可。接入新结构时，
定义自己的普通参数结构（包括布局枚举），提供参数解析、单份结构的分配/重置与
载荷预算，再注册实现。`OwnedExternalBlock` 负责已登记内存的自动释放与失败回滚。
参数类不需要继承框架基类，也不需要实现 `ToJson()`。

配置文件读取由最外层 `OperatorConfigResolver` 负责，配置提取使用独立的接入组件
`OutputConfigReader`。它在 Create 阶段运行，不是业务 Pipeline 中的 Node。
接口位于 `adapter/operator_output_config.h`：

```cpp
std::string text;
reader.Read(OutputConfigField::kParameters, &text, &error);
```

可选字段是该组件固定的枚举：`kType`、`kAllocator`、`kParameters`、`kCapacities`、
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
| bridge `logical_name` | 业务中的输出槽位，也是 `data.outputs` 的配置键 |
| bridge `key_suffix` | 外部 map key 最后一个点号后的部分；省略时沿用 `type_suffix` |
| `type` | 已注册的外层 ValueType，必须匹配槽位的 `type_suffix` |
| `allocator` | 为该外层类型注册的分配方案标识；省略时使用类型的默认实现 |
| `params` | 由方案解释、校验并补齐的单份布局参数，例如嵌套枚举与数组容量 |
| `capacities` / `meta_num` / `metadata_type_id` | 方案声明的标准字符串与 CompanyAny 容量字段 |

以下配置来自参与编译的
[测试接入](../../tests/integration/operator/test_operator_api.cpp)和
[嵌套结构实现](../../tests/support/operator_nested_output_fixture.h)。这些类型和业务只在
测试程序中注册，用于说明扩展方式，不是生产 SDK 中可选的新业务。

```json
{
  "data": {
    "pipe_path": "pipeline.json",
    "outputs": {
      "main": {
        "type": "test_nested_out",
        "allocator": "test_nested_standard",
        "params": {"kind": 1, "capacity": 8}
      },
      "audit": {
        "type": "test_nested_out",
        "allocator": "test_nested_alternate",
        "params": {"kind": 2, "capacity": 16}
      }
    }
  }
}
```

示例 bridge 将两个槽位的 `key_suffix` 分别注册为 `result`、`audit`，调用方准备
`outputs[i]["chan.result"]` 与 `outputs[i]["chan.audit"]` 两个空 shared_ptr。
二者都指向 `NestedOutputEnvelope`，其 `void* payload` 指向下一层结构，后者的
`void* values` 再根据 `kind` 指向整数或浮点数组。修改 `main` 的方案或参数后创建
另一个 handle，map 键仍可保持 `chan.result`。

每个声明的输出槽位都需要配置，包括 `required=false` 的可选输出；可选是指 Process
可以省略该输出 map 项。逻辑名和有效 map 后缀分别唯一；不同槽位可以复用相同类型
和方案，各自使用独立容量和输出池。原单输出 `data.mem_que` 继续可用，也接受
`allocator` / `params`，但不能与 `data.outputs` 同时出现。

## 实现与注册

包含 `adapter/operator_value_type.h`，在接入层自己的 `.cpp` 中建立
`OperatorValueTypeBinding`。外层类型首次接入时调用 `RegisterOperatorValueType`；
为该类型新增命名方案时调用 `RegisterOperatorOutputAllocator(name, binding)`。
分别通过 `REGISTER_OPERATOR_VALUE_TYPE`、`REGISTER_OPERATOR_OUTPUT_ALLOCATOR`
登记无参注册函数，并把源码加入接入层构建目标。不需要包含私有 registry 或 pool 头，
也不需要在中央分发表增加业务判断。

每个输出方案提供以下行为：

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

在 `OperatorBizSlot::convert_output` 注册该槽位的业务结果转换函数。多输出 bridge
每个槽位都要提供转换；已有单输出仍可使用 `convert_sample_output`。
回调接收内部结果、已分配的外层结构和对应的 `ResolvedOutputPoolSpec`，按同一份
`allocator` 和类型化参数填充载荷。转换必须保持指针与已分配布局一致，不重新读取
部署文件、不另设默认容量，也不把请求局部指针塞进输出结构。

框架在全部输出转换成功后发布 map；任何一项失败都会归还已经获取的输出租约。
调用方读取时依照外部协议的枚举解释 `void*`；内存的实际清理依据已登记的所有权
记录。输出引用不延长 handle 的有效期，销毁顺序沿用
[输出容量与生命周期](business_onboarding.md#输出容量)。

`alg_pipeline_tool resolve-conf` 在 `configuration.output_pools` 按逻辑槽位展示有效
方案与框架容量，`params` 是交给结构体解析函数的**字符串**（例如
`"{\"kind\":1,\"capacity\":8}"`），不包含该解析函数内部补齐的默认值。单输出还保留
相同内容的 `output_pool`。现有 Demo/Studio Profile 使用原单输出
业务；新多输出业务由其宿主调用或相应 Demo 扩展验证。公开 C ABI 的输出契约不受
Operator 方案选择影响；若新增 C ABI 动态输出，应另外定义完整的缓冲区所有权契约。
