# RFC-0059：输入输出转换独立化与接入绑定架构

- **RFC 编号**：0059-independent-adapter-input-and-output
- **创建日期**：2026-09-15
- **文档状态**：In Implementation
- **关联分支**：`refactor/adapter-io-layout-design`
- **目标版本**：投产前一次迁移 / ABI 6.0.0 / Catalog schema 4
- **负责人 / 作者**：LLM-EdgeFlow 维护者
- **代码核查基线**：`e6aa776c959aa84f5eec20bf38e73c313718e01a`
- **关联设计计划**：[doc/plans/adapter_io_layout_design_2026-09-15.md](../plans/adapter_io_layout_design_2026-09-15.md)
- **关联决策**：修订 RFC-0002、RFC-0004、RFC-0009、RFC-0028、RFC-0048、RFC-0049、RFC-0050、RFC-0053；废弃业务级 `operator/bridges/` 与 `IBizAdapter` 双向绑定；统一 C ABI 与 Operator 数据流。

## 1. 背景与目标

当前 LLM-EdgeFlow 的接入适配层 (Integration) 存在以下架构耦合问题：
1. **输入与输出转换深度耦合**：每个 `IBizAdapter` 同时负责特定外部结构的 Unpack 和 Pack，无法跨业务或跨数据结构独立复用转换逻辑。
2. **Operator 业务桥维护冗余**：`operator/bridges/` 下各业务重复构造 C ABI 影子对象或中间结构，导致转换链路冗长且易越界。
3. **缺少显式组合与中性验证**：Pipeline 只处理内部 Typed Port，但接入配置和入口选择依赖旧的 `CompanyAlgBizType` 枚举或隐式推断，不支持同一 Pipeline 挂接不同输入/输出结构。
4. **两套独立 Catalog/验证路径**：C ABI 与 Operator 在配置、验证与元数据暴露上存在碎片。

本 RFC 实施以下最终架构：
1. **输入与输出转换独立解耦**：
   - 输入转换器：读取外部输入，校验并 copy-in 发布至内部 Typed Ports。
   - 输出转换器：读取内部结果，按宿主容量与模式写入外部输出。
   - 接入绑定 (`IoBindingDefinition`)：显式关联业务契约 (`biz_name`)、输入转换器与输出转换器，声明端口映射。
2. **Operator 与 C ABI 统一数据流**：
   - 两种入口统一调用 `DecodeInput` 与 `EncodeOutput`。
   - Operator 通用入口只负责槽位解析、ValueType 校验、内存池预算/租约/发布。彻底移除业务级 `operator/bridges/`。
3. **唯一接入配置与中性计划验证**：
   - 唯一配置 `schema_version: 1`，显式指定 `io_binding`。
   - `PipelineValidator` 统一校验中性 I/O 边界（包括业务必需 ingress、额外写入非冲突、输出消费与必需 egress 完整性）。
   - Pipeline 消费稳定的 `ValidatedPipelinePlan`。
4. **一次性完整迁移，不向后兼容旧 ABI**：
   - `CompanyAlgParamCreate` 移除 `biz_type`，公开辅助接口改为 `ValidateOperatorConfigBinding(..., const char* expected_binding_id)`。
   - SDK ABI 版本升级至 6.0.0，SOVERSION 升级至 6。
   - Catalog 对外统一输出 `schema_version: 4`，新增 `validate-io` 工具命令。

## 2. 详细设计要点

完整规范与实施要求参见 [doc/plans/adapter_io_layout_design_2026-09-15.md](../plans/adapter_io_layout_design_2026-09-15.md)。

### 2.1 目录布局与职责
- `include/adapter/`：`io_converter.h`, `io_binding.h`, `converter_authoring.h`
- `src/adapter/input/`：各种输入转换器实现（如 `text_input.cpp`, `translate_json_input.cpp` 等）
- `src/adapter/output/`：各种输出转换器实现（如 `translation_json_output.cpp`, `structured_document_output.cpp` 等）
- `src/adapter/biz/`：业务契约及接入绑定定义（如 `translate_bindings.cpp` 等）
- `src/adapter/operator/`：通用 Operator 机制（ValueType、内存池、配置解析、执行生命周期管理）
- `src/adapter/`：`io_converter_registry.h/.cpp`, `io_binding_registry.h/.cpp`, `io_binding_resolver.h/.cpp`, `io_catalog.h/.cpp`, `deployment_io_config.h/.cpp`

### 2.2 错误与执行生命周期
Operator 执行时序：
1. 句柄加锁与帧数批次验证。
2. 解析输入/输出槽位。
3. 调用输入 `DecodeInput`（若校验失败直接终止，不租用输出池）。
4. 租用输出池内存块并登记 RAII guard。
5. 执行 Pipeline 计算。
6. 调用输出 `EncodeOutput` 写入已租用目标。
7. 统一发布至调用方。

## 3. 验收标准
1. 8 个既有业务（Translate, EntityExtract, KeywordMatch, DocQA, CrossRerank, ComplianceAudit, AudioAsrIntent, OcrDocQa）两套入口（C ABI 与 Operator）全部迁移至新架构。
2. 业务 bridge 彻底移除，无旧 `IBizAdapter` 与旧业务枚举选择路径。
3. 新 Catalog schema 4 正常导出且含所有业务、转换器与绑定。
4. 新工具命令 `validate-io` 支持 C ABI 与 Operator 配置静态核查。
5. `./scripts/run_all_tests.sh` 门禁全量通过。
