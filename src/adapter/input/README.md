# Input Converters（输入转换器）

本目录解析完整业务请求，校验业务字段并发布 Pipeline 内部值。平台载体读取由 binding 负责。

- 编写与注册：[业务接入](../../../doc/dev_guide/business_onboarding.md#3-实现并注册转换器)。
- 平台布局与所有权：[输出池与宿主接口](../../../doc/dev_guide/operator_output_allocation.md)。
- 一请求一载荷参考 [text_input.cpp](text_input.cpp)；候选展开参考 [rerank_input.cpp](rerank_input.cpp)。
- 完整 JSON 字段选择参考 [translate_json_input.cpp](translate_json_input.cpp)。
