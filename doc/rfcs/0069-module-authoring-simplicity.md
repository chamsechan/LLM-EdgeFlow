# RFC 0069：以普通函数降低模块开发负担

- **RFC 编号**：0069-module-authoring-simplicity
- **创建日期**：2026-09-22
- **文档状态**：Completed
- **关联分支**：`refactor/module-authoring-simplicity`
- **目标版本**：投产前
- **负责人 / 作者**：LLM-EdgeFlow maintainers
- **基线**：`e03d088`
- **关联决策**：延续 RFC-0059/0060 的独立转换与 Operator 边界、RFC-0065 的单一实现、
  RFC-0068 的 Node 作者契约；将 RFC-0066 中暂未实施的作者包装作如下有限演进。

## 1. 问题与范围

用户要求优先降低单模块作者必须理解的框架知识，并避免过度设计。普通 Node 已有
函数和 Spec；Converter 仍直接处理 Context、绑定、批次和输出槽，逐项 Model 仍手写
批次切片。复杂 Node 的初始状态和 Control 重建路径不够一致，测试和教程也存在失配。

本次完成可独立验证的有限改造：教程与生成测试、普通请求转换、逐项模型调用包装、
复杂状态构建及缓存失败适配。直接修复审查已复现的输出 NUL 截断和排名节点查询关联。
不重写 Core、厂商 Backend、复杂分配器，不新增注册表、配置语言或通用反射。

## 2. 决策与权衡

### 接入适配层 / Integration

- 在现有 converter 作者头提供类型化的普通请求行/结果行包装。作者函数接收宿主值和
  普通字段，输出写入由同步借用的 writer 管理；框架管理循环、来源、绑定和发布。
- 保留独立 InputConverterDefinition / OutputConverterDefinition、原始函数指针回调和
  IoBinding。回调只需调用行辅助函数，不新增声明类型或捕获式注册，不恢复旧 BizAdapter。
- 先迁移适用的单槽、每请求一行的转换；多槽和展开/汇聚保留其显式算法，不用业务开关
  强塞进单行包装。完整 JSON、字段限制、PCM 所有权、业务状态仍在 Integration。
- 内部字符串统一通过 `std::string_view` 按长度写入，支持内嵌 NUL，不保留 `const char*`
  兼容重载；空值使用空 view 表达。容量取实际池
  声明，失败不能发布 Operator 输出，不变更池租约和销毁关系。

### 能力节点层 / Capability Nodes

- 保留现有 Spec。普通字段继续复用 Field；复杂配置/更新复用普通的候选状态构建与校验函数，
  框架原有快照控制发布。初始配置与补丁不必同形，不从编译状态反向生成 JSON。
- SessionResources 适配返回 NodeResult 的资源工厂，共享失败信息且失败不缓存；
  Node 作者只使用 `GetOrCreateResult`，不保留旧 `GetOrCreateResource` 转发入口。
  缓存身份和模型版本仍显式由算法提供，底层复用 Core 唯一的 single-flight 实现。
- VectorTopK 和 TextRerank 的 queries 路线限定每请求一个 query，遇重复请求的 query
  明确失败；Rerank 候选缺对应 query 也失败。pairs 和 shared candidates 语义保留。
  不增加全局 Batch 唯一性约束；该检查是这两个算法的前提。

### 模型执行层 / Model Execution

- 为逐项推理提供有限的函数包装，复用 IModel、ModelRuntimeFactory 和 FixedBatchExecutor。
  作者保留既有能力接口、模型资源准备与逐项语义；框架处理循环、来源和异常回滚，
  不新增模型基类或自动身份声明。
- 不把 Tensor Model 改为逐项执行，不扩建通用多阶段模型 DSL。Qwen 的来源相关 seed、
  Whisper 的整批输入预校验、各模型错误码与协议要求保持。
- 配置和资源检查按当前 owning layer 保留，不让 Engine 依赖 Node 的参数或结果类型。

### Tooling

- Control 教程使用当前 Pipeline deployment、相对 conf 目录的 pipe_path，并纳入原生验证。
- 生成的普通模块行为测试统一使用 NodeHarness；重复 Run 重复发布输入，避免输入被耗尽。
  框架契约测试仍可直接操作计划。生成提示指向实际存在的业务函数。

## 3. 兼容与迁移

Operator API、平台布局、Pipeline 格式、业务标识、注册与池所有权保持。源码作者辅助接口
随仓内调用同步迁移；不添加第二套同义运行时。错误来源保留转换器/字段/样本信息。
已迁移模块删除旧循环、重复构建与异常适配；不存在按版本选择的新旧执行分支。
保留的 `FixedBatchExecutor::Execute` 是逐项包装和 Tensor 批处理共用的底层执行器；
多槽 Converter 使用的读写辅助函数也是行包装的底层能力，不是旧版本兼容实现。
两项正确性修复的行为变化为：普通字符串不再静默截断；非法多查询关联不再静默成功。
每阶段可整体回退对应 helper、消费者和测试，不能只回退其中一侧。

## 4. 验证与完成条件

- 教程提取与生成测试进入现有 runner；非文本重复执行、Control 成功/失败保持行为。
- Converter 用现有业务验证绑定重命名、重复外部 ID、输入拷贝、结果重排、错误诊断、
  容量及 NUL；完整 Operator 用例验证输出发布和租约复用。
- 模型包装验证空批、错误输出清理、标准/非标准异常、数量与来源；现有模型 suites
  验证 seed、BOS、输出处理、原始错误码、Whisper 全批预检和 BGE 不受影响。
- 节点测试验证重复 query/缺 query 拒绝，pairs/shared 正常；缓存失败并发与重试、
  初始配置和 Control 构建失败保持旧状态。
- 独立审查作者接口、所有权与错误传播；最后按 CONTRIBUTING 执行一次 canonical gate。
  Mock/源码验证不宣称真实模型效果、硬件验收或真实新手试用成功。

## 5. 实施与最终结果

1. 教程、测试入口与两项正确性修复，聚焦验证后接受。
2. Converter 普通请求作者包装及真实消费者迁移，聚焦验证后接受。
3. 逐项模型、复杂状态与缓存适配，聚焦验证后接受。
4. 更新现行指南，完成独立审查与最终门禁。

- 阶段 1：接受。Node 聚焦 65 项、Adapter 聚焦 21 项通过；脚手架 Python 21 项通过。
  教程 JSON 经生成 fixture 调用原生部署解析与 IoBindingResolver 验证。独立静态审查通过。
- 阶段 2：接受。Adapter runner 全部 223 项通过，包含新增行函数契约测试与实际 Operator
  路径；独立审查通过。迁移 4 个输入注册（文本实体/关键词、翻译 JSON、PCM）和 3 个输出。
  新测试源码中的真实 NUL 字节已修为 C++ 转义文本，后续统一构建再次验证。
- 阶段 3：`ExecuteItems` 迁移 Qwen、Whisper、VisionDocument、GeneratedTextEmbedding；
  BGE 保留 Tensor 批次。TextEmbedding 使用返回 NodeResult 的缓存工厂；模板与规则共享
  候选构建函数。CorpusSource 保留原 parser：现有 Field 不能表达其“可缺省但无声明默认值”
  的契约，本次不为一个字段扩建参数接口。
- 静态审查发现同一批次切片内诊断可能残留，已改为每次逐项调用前清空并增加回归测试。
- 阶段 3：接受。最终增量构建后，Node 聚焦 161 项、Model/Batch 聚焦 74 项、Adapter
  全量 223 项通过，无失败或跳过；覆盖完整失败信息、缓存重试与并发、Control 旧状态、
  模型预检及固定 Tensor 回归。
- 阶段 4：现行接入、Node、Model、源码布局指南与技能引用同步；独立审查已关闭，
  无剩余阻断。按 CONTRIBUTING 将完整最终 diff（含本状态）交给唯一验收命令
  `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh`，完成状态以该门禁通过确认。
- 迁移收口：4 个适用输入注册、3 个适用输出注册与 4 个逐项 Model 全部使用公共辅助层。
  删除无人调用的 Node 缓存转发入口和两个字符串兼容重载；剩余多端口/多槽、排名组合
  各自只有一份实现。Core 缓存及批次执行器继续作为唯一底层实现供上层复用。
- 验证范围为默认构建、源码契约和中性测试夹具；不声明真实模型效果、目标硬件或内部 SDK
  验收，也不以代码行数替代真实开发者试用。复杂多槽、展开/汇聚、Tensor 与 Backend
  资源管理仍保留其必要的显式契约。
