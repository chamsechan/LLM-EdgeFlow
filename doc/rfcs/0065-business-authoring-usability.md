# RFC 0065: 业务开发入口与诊断的定向简化

- **RFC 编号**：0065-business-authoring-usability
- **创建日期**：2026-09-21
- **文档状态**：Completed
- **关联分支**：`refactor/business-authoring-usability`
- **目标版本**：投产前
- **负责人 / 作者**：LLM-EdgeFlow maintainers

## 1. 问题与范围

基线 `63b006d` 的代码审查发现可选输入槽压缩帧索引、缺少内部结果被报为容量不足、
Backend 原因在 Model 返回时丢失，以及已存在的简单编写能力未被默认入口使用。
用户要求修复并尊重现有目录，避免无消费者的框架和无收益开发。

## 2. 决策与边界

- Integration 可选槽为每个输入帧保留位置；缺失使用空共享指针，必填槽仍拒绝缺失。
  内部 context/结果缺失使用现有 InvalidInput 分类，容量不足继续使用 BufferTooSmall。
- 将内置类型化输入和池化输出构造函数用于源码扩展接口；模板留在 `include/adapter/`，
  非模板分配、预算与重置实现留在 `src/adapter/operator/`。所有内置消费者使用同一实现，
  不新增目录层级，不改变池租约、预算、释放顺序与输出容量契约。
- Adapter 使用现有类型化 key/port helper 消除可推导的类型字符串；保持输入和输出转换器
  独立，保留显式业务映射。测试夹具扩展到命名槽与输出池规格，不复制运行时校验规则。
- Node 脚手架默认按已实现的能力选择函数式模板；Control 和 Embedding 复用现有 Spec。
  手写生命周期入口保留给超出该范围的节点。生产代码只采用有实际简化收益的既有 helper；
  本次不新增多输出 DSL、不重做 Core/Blackboard、不统一成万能配置或数据框架。
- Model 五种能力方法增加可选的调用方诊断字符串输出，与 Backend 现行风格一致。
  FixedBatchExecutor 补充失败诊断，Model 传递 Backend 原因，Node 保留原因到请求错误。
  诊断属于当前调用或其同一次资源创建，不存入共享 Model 状态；不增加另一个 Result 类型体系。
- SessionContext 的同一次资源创建向所有等待者传播同一异常，完成后移除 flight；失败不入缓存，
  后续调用可重试。TextEmbeddingNode 在该边界内携带模型错误码与原因，防止等待请求丢诊断。

## 3. 兼容与迁移

Operator 函数表、平台 DTO、Pipeline/Catalog schema 不变。内部结果缺失的转换器返回码
从误用的 -4 更正为 -3，更新对应契约测试。Model 源码扩展实现统一迁移诊断参数；原调用点
可以省略参数，不保留旧虚函数转发链。`TraceableUnaryInferenceNode::InferBatch` 的实现也增加
诊断输出参数，仓内示例、生成器和 override 一并迁移。源码扩展需重新编译，不承诺内部 C++ 动态 ABI。
本次延续 RFC-0064 的单一实现原则，不恢复其删除的无消费者 Adapter 包装。

## 4. 验证与完成条件

1. Adapter 聚焦测试覆盖可选槽前/中/后缺失、required 拒绝、输出缺数据与真实容量不足，
   多槽夹具和常见输出分配/重置/失败回滚；现有业务输入输出测试保持通过。
2. 脚手架测试和生成 C++ 的行为测试验证默认选择、显式高级入口、Control、Embedding。
3. 模型与 Node 测试覆盖 Backend 原因传播、空批次、后续批次失败全量清空，确保诊断属于
   当前调用；并发资源创建失败传播及重试；独立复核跨层诊断和输出内存辅助实现。
4. 更新活跃指南与 Changelog，执行 CONTRIBUTING 的唯一全量门禁。

## 5. 实施与最终结果

- Adapter、Node、Model/Core 已完成上述修改；现有目录划分、Operator 函数表和 DTO 保持不变。
- 聚焦验证：413 项 C++ 测试、20 项脚手架 Python 测试通过；生成的 Node 及行为夹具参与 C++ 编译和测试。
- 当前生产 Catalog 保持 12 Nodes、6 Models、2 Backends、8 组业务/输入转换器/输出转换器/绑定。
  10 份使用当前可用 Backend 的 Pipeline 均通过原生 validate 与 plan。
  其余 7 份配置依赖当前构建未启用的 kite_llm 或 whisper_cpp；未宣称真实模型或硬件验收。
- Authoring benchmark probe 通过 C++ 语法和类型检查；本次不作新的性能结论。
- 独立复核覆盖输出所有权、类型映射、诊断迁移与 single-flight 并发语义，无剩余阻断缺陷。
- 最终统一门禁：`LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh`。本状态依 CONTRIBUTING
  在最终差异中准备，以该命令成功确认；失败时恢复实施状态并修正。
- 各块均可按本分支文件差异回退；不重置无关修改。
