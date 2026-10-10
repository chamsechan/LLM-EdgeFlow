# Adapter 数据解包安全示例

四个示例位于 `tests/support/adapter_examples/`，由
[Adapter 契约测试](../../../tests/contract/abi/test_adapter_contract_security.cpp)
直接编译和执行。它们使用独立的示例结构体、DTO 和 Blackboard key，用于验证边界检查
与 COPY_IN 行为，不注册生产业务，也不是可直接连接现有 Node 的业务脚手架。
这些示例中的载体解引用和内存检查属于生产 binding 的职责；生产 Converter 使用中立值，
不复用示例的原始指针接口。

| 示例 | 验证重点 | 实现 |
| :--- | :--- | :--- |
| 平面结构 | 有界字符串读取、输入深拷贝、固定输出容量检查 | [flat_struct_adapter.h](../../../tests/support/adapter_examples/flat_struct_adapter.h) |
| 带标签联合体 | 标签枚举检查、分支载荷检查 | [tagged_union_adapter.h](../../../tests/support/adapter_examples/tagged_union_adapter.h) |
| 嵌套数组 | 数量与指针检查、乘法溢出防护、逐元素字符串复制 | [nested_array_adapter.h](../../../tests/support/adapter_examples/nested_array_adapter.h) |
| 多级指针树 | 递归深度限制、子节点数量与空指针检查、递归深拷贝 | [nested_pointer_tree_adapter.h](../../../tests/support/adapter_examples/nested_pointer_tree_adapter.h) |

以参与编译的头文件为准，文档不再复制完整实现。

新增生产业务时，参考[业务接入说明](../business_onboarding.md)和
[输入转换器](../../../src/adapter/input/)和[输出转换器](../../../src/adapter/output/)，通过 `alg_pipeline_tool catalog`
查询实际注册的业务与端口契约。
