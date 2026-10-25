# Output Converters（输出转换器）

本目录关联 Pipeline 结果并组装完整业务响应，binding 将中立值写入租用的输出块。
当前 JSON 输出使用 `JsonOutputValue`，容量统一由 `JsonOutputParameters()` 声明。

- 编写、注册与 `EncodeResultRows` / `WriteOutputRows`：[业务接入](../../../doc/dev_guide/business_onboarding.md#3-实现并注册转换器)。
- 容量、可选输出与失败发布规则：[输出池与宿主接口](../../../doc/dev_guide/operator_output_allocation.md)。
- 单结果参考 [keyword_result_output.cpp](keyword_result_output.cpp)；多路结果参考 [audit_result_output.cpp](audit_result_output.cpp)。
- 共享业务响应组装放在本目录，例如 [rule_match_response.cpp](rule_match_response.cpp)。
