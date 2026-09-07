# RFC 0043: 参数配置与执行行为的最小一致性整改

- **RFC 编号**：0043-config-parameter-consistency
- **创建日期**：2026-09-07
- **文档状态**：Completed
- **关联分支**：`fix/config-parameter-consistency`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow 维护团队
- **关联决策**：补充 RFC-0022、RFC-0026、RFC-0041、RFC-0042 的参数与失败契约

## 1. 问题与范围

基线 `6566006` 已复现配置编辑改变字符串、Demo Control 覆盖草稿参数、
JSON 提取丢失截断数组内容、模板缺失输入仍成功、分块生成重复尾部、规则分数溢出、
Embedding 归一化双重决策，以及后端组合配置预检遗漏。
自定义节点的 `system_prompt` 实际只是普通输入前缀，和模型层同名字段含义不同。

本次只修正上述行为，保留现有四层架构、参数读取方式、失败策略默认值和模板语法选项。
不引入参数宏、配置反射、通用规则语言、自动热更新或复杂表单系统。

## 2. 决策与权衡

- Tooling：字符串参数无损编辑，未编辑的字段保留原值及缺省状态，编辑后保存字符串，
  包括空串；删除字段使用已有 JSON 编辑页，不增加单独的存在性控件。Studio 草稿运行
  不下发 Demo 默认 Control，复用现有 CLI 开关，显式 Control 测试继续使用既有入口。
- 能力节点层 / Capability Nodes：JSON 只解析完整输入或提取完整 JSON 块，不补全
  截断数据、不从任意引号词构造结果。`extract_json_block=false` 要求整体解析成功；
  `true` 在整体解析失败后尝试完整 JSON 围栏或首个外层容器，不按首字符切换严格程度，
  也不从破损父容器内提取子节点。解析失败继续遵守现有 `failure_policy`。
  模板内置变量与动态变量遵守同一缺失策略，区分未连接/样本缺失与合法空值。
  分块在最后一块覆盖全文后停止；规则分数在共用构造路径校验有限性及 [0,1] 范围。
- 模型执行层 / Model Execution：归一化选择由 `EmbeddingOptions.normalize` 决定，
  BGE 移除重复的模型配置开关；模型继续负责归一化实现。
  BackendDefinition 增加可选纯配置校验回调，llama.cpp 将组合约束与初始化共用，
  不执行模型加载或外部 I/O。
- 流程编排层 / Orchestration：默认值归一化且字段校验通过后调用后端声明的校验，
  在统一 Validator 返回诊断；不依赖具体 Backend，也不在前端复制规则。
- 自定义节点的普通输入前缀命名为 `prompt_prefix`，模型层真正的 `system_prompt`
  保持其消息角色语义。简单字段沿用 `config.value<T>()`，同一字段默认值共享常量，
  只对实际复用的复杂逻辑提取解析函数。

## 3. 兼容与迁移

- BGE `model_config.normalize` 删除；需要不归一化的调用将 `normalize:false`
  放到 TextEmbeddingNode 的 `config`。仓库配置同步迁移，旧模型字段由 Validator 拒绝。
- PromptGuidedLlmNode 的 `config.system_prompt` 改为 `config.prompt_prefix`，仅迁移
  该节点的字段；Qwen 等模型的 `model_config.system_prompt` 不变。旧节点字段明确拒绝。
- 曾依赖截断 JSON 修补或任意引号实体提取的方案，改为产生完整 JSON；需要业务修复的
  场景以后在明确需求下单独实现，不将其隐含在通用提取开关中。
- 显式请求失败策略且引用缺失输入的模板将失败；需要空值的方案显式使用既有 `empty`
  策略或提供已连接的空上下文。合法的空上下文批次继续可用。

## 4. 验证与完成条件

扩展既有 Nodes、Engine、Catalog/Validator 和 Studio 测试，覆盖：字符串原样应用、
草稿实际参数、截断数组及引号文本、缺失与空模板输入、高 overlap 尾部、规则分数溢出及
失败 Control 保持旧值、归一化开关、默认展开后的后端组合校验、前缀迁移及旧字段拒绝。
验证受影响 Pipeline 与对应 Demo，最终执行 `./scripts/run_all_tests.sh`。
无新增依赖或测试可执行文件；不以 Mock 验证替代真实模型效果或目标硬件验收。

## 5. 实施与最终结果

- 八项范围已完成，沿用既有参数声明、类型化读取与 Control 扩展点，无新增依赖或测试
  可执行文件。自定义参数约定见[节点指南](../../src/custom_nodes/README.md#自定义参数的最小约定)，
  后端回调与归一化所有权见[开发者指南](../developer_guide.md)。
- 复审删减后，`LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh` 再次通过：格式、空白、完整默认构建及
  89/89 个 CTest 套件成功。新增回归覆盖前述边界，Studio 服务测试实际执行修改了
  `STUDIO_DRAFT` 规则的临时 Pipeline，并核对结果，证明草稿不再被默认 Control 覆盖。
- 对 `configs/` 和 `demo/fixtures/mock/` 的 24 份 Pipeline 逐一执行原生校验；当前构建
  支持的 17 份均通过 `validate` 与 `plan`，包括本次迁移的两份 ONNX/llama.cpp 文档
  问答配置。其余 7 份只报告当前未启用的 Kite/Whisper Backend 及关联模型不可用，
  不作为这些 Backend 的完整验证证据。Kite 配置中的 BGE 旧字段已同步删除。
- `entity_extract_custom_mock` 与 `doc_qa_custom_mock` 通过既有 Demo 执行，均关闭
  默认 Control；核对请求 ID、实体 JSON、分块数、意图与回答字段，执行成功。
  BGE 归一化用同一模型实例在两个节点之间切换选项的回归测试验证，避免缓存或实例
  配置覆盖调用选择。
- 验证边界：默认门禁未启用 Kite/Whisper 和独立真实模型 E2E；内部用例中有 8 项因
  Kite 未启用或 GGUF 资产缺失跳过。本次没有进行真实模型效果、目标硬件或内部 SDK
  验收，Mock 与中性执行协议测试仅证明配置和执行契约。
