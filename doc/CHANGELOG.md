# Changelog

## 2026-09-08 方案开发者工作路径修复（RFC-0045）

- Studio 展示 Validator 的端口、相关节点和修复建议，保留节点定位；诊断文本按纯文本渲染。
- CLI `init --raw` 直接输出可保存的 Pipeline 文档，默认版本化响应保持兼容；冲突、重复和缺值选项明确失败。
- Operator 公开接口和接入教程说明输入借用、输出池租约、池满等待及 Destroy 后 handle 的失效语义，给出结果复制和释放顺序。
- OCR 与单槽 Demo 共用 Control 处理；显式文件缺失、空路径、非法 JSON 或命令会失败，OCR 提示词更新在处理样本前实际执行。
- Control schema 执行 `minimum` / `maximum` 范围；注册时拒绝无效或不支持的声明，诊断包含节点与命令，文档注解不自动修改 payload。

- Node 初始化可通过 `NodeInitContext::Fail` 返回具体原因；Pipeline 顺序与并行执行错误均附实例 ID 和类型，保留原错误码。

- Demo 默认保留 Pipeline 配置，内置规则更新改为 `--example-control` 显式启用；保留 `--no-default-control` 兼容，显式 Control 文件优先。

- Control 支持版本化 JSON 信封按 `node_id` 定向更新，裸 payload 保留广播；C ABI、Operator、Demo 共用路由，未知目标和非法信封在更新前拒绝。

- Studio 可另存 JSON、配套 `.conf` 与运行命令；草稿与效果验收共用配置生成，模型路径来自当前选择。新增 `resolve-conf` 复用原生部署解析器，展示路径来源、规范化默认值和输出池容量，避免旧配置覆盖及临时目录根路径错误。

## 2026-09-08 投产前契约一致性修复（RFC-0044）

- session Embedding 缓存以版本、长度及完整内容编码身份，保留同键并发复用与失败重试，修复嵌入 NUL 的不同语料返回相同向量的问题。
- 补齐 Operator、运行时、注册与 Backend 的异常保护；共享不可抛诊断赋值，避免错误处理中的分配失败穿透 `noexcept`。
- PipelineValidator 对业务出口复用端口流校验，可选出口缺失允许、存在时仍检查类型；ranked 出口按 Adapter 的实际聚合行为声明。
- C / Operator 共用渠道名称和音频限制：渠道名最多 256 字节，合法采样率的零长度 PCM 可使用空 buffer；metadata type ID 在窄化前检查 int32 范围。
- 顺序和并行执行共用节点错误汇总，保留返回码与本次调用诊断；TextChunk 保留父项 counts 来源，生成切片在每个请求内不重号，并检查计数容量。
- BGE / generated embedding 共用有限值与归一化规则，使用 double 累加，归一化拒绝零向量，失败不发布部分输出。
- Node / Model / Backend 共用字段定义校验；Node 防御性 Init 与预检共用字段规则，语义回调异常转为诊断。删除内部 `IBizAdapter::ValidateInput`，字段校验归入 `Unpack`；自定义 Adapter 需迁移并重新编译。

## 2026-09-07 真实实体提取示例与合并后 CI 验证修复

- 公开 llama.cpp 实体提取示例将生成预算从 64 调整为 256 token，并使用贪心解码，避免随机生成较长列表时截断 JSON；解析失败直接报错。真实 C ABI 回归覆盖公开 Profile 的长语料并检查非空 JSON 列表，继续通过 Demo 验证 Operator 路径。
- 交付脚本在合并后等待该 merge SHA 对应的 `main` push CI；PR 检查成功不再直接作为整次交付完成条件。合并后 CI 失败、取消或未注册时明确返回失败，保留已合并状态供诊断。

## 2026-09-07 参数配置与执行行为一致性整改（RFC-0043）

- Studio 保留字符串换行、空白和显式空串，未编辑的字段保持原值及缺省状态；草稿运行关闭 Demo 默认 Control，实际执行用户配置的规则和模板。
- JSON 提取保持完整外层结构，不再修补截断数组或从任意引号词构造有效结果；关闭 `extract_json_block` 时要求整段严格解析，失败继续执行配置的失败策略。模板内置变量遵守缺失策略，合法空上下文仍可使用。
- 分块在覆盖输入末尾后停止，消除高重叠参数产生的重复尾块；规则分数在预检、初始化和 Control 中统一检查有限性及 `[0,1]` 范围。
- BGE 归一化由调用选项决定，删除 `model_config.normalize`；需要关闭归一化时配置 TextEmbeddingNode 的 `config.normalize:false`。PromptGuided 的普通文本前缀改为 `prompt_prefix`，旧节点 `system_prompt` 明确拒绝，模型 system 消息字段不变。
- 后端可注册纯配置语义校验，llama.cpp 的 batch/context 组合在模型加载前拒绝，与运行时共用解析规则；自定义节点指南补充默认值共享、类型化读取和参数单位约定。

## 2026-09-07 文件浏览与契约边界修复（RFC-0042）

- `./show 配置.json` 保持默认终端显示，显式 `--web` 才启动 Web；Web 增加单文件选择，可用 `./show --web` 启动后导入 Kite 子目录等位置的方案，编辑后可另存到受控配置。
- 模型类型标注当前构建无兼容 Backend 的原因，保留不可用模型的浏览能力；不依赖模型资产或 Catalog 工具成功加载才能显示流程。
- PromptGuided 标准模式与 TextTemplate 共用模板解析；新增 `template_syntax=auto/standard/legacy`。未标注语法的旧双括号模板会明确失败，需选择标准变量语义或旧转义语义；官方 custom 示例已迁移。
- 三处路径检查复用中立词法函数，保留部署与规划职责，修复 Operator 误拒合法 `..name` 文件名的问题。
- 各层运行时目标使用受限头文件视图，去除全库 include 搜索路径的传播；共享 Core 契约清单与 LayerGuard 一致，并增加真实编译及增量头文件回归。

## 2026-09-07 Pipeline Studio 查看与编辑体验优化

- 默认使用全宽流程图浏览，编辑模式提供可折叠算子与属性面板；响应式工具栏增加独立的适应画布、100% 与缩放控制，恢复节点位置后重新适应当前画布。
- 连线改为单击选择后显式删除，增加当前文档内的撤销与重做；数据流和独立执行依赖分别以实线、虚线显示，走线避让无关节点。
- 节点、模型与 JSON 表单显示未应用状态，保留草稿并要求应用或放弃后再保存、校验或运行，避免刷新视图时覆盖输入。
- 终端 `alg_show` 只展示模型实际声明的 Backend 批处理字段，修正读取错误位置并推测默认批大小的问题。使用说明见 [Pipeline Studio](../tools/pipeline_studio/README.md)。

## 2026-09-07 Control 开发路径收敛（RFC-0041）

- Operator 新增固定结构的 `kJson` 入口，普通节点命令使用 `cmd_id + JSON`，保留既有强类型命令；Demo 增加 `--control-cmd` 和 Profile `control_cmd`。
- Pipeline 与节点复用有限 payload schema 校验；Control 失败诊断包含节点实例与原因。规则元素拒绝未知字段和缺失 pattern，原先被忽略的错误字段需要修正。
- 现有 Catalog 拒绝跨节点类型的命令 ID 冲突；共享同义命令要求双方显式 `shared_id` 且契约一致。
- 新增可编译的 Control 模板、`--control-id` 脚手架选项和[入门练习](dev_guide/first_control.md)，生成业务测试验证更新生效与失败保持。模板不加入生产 Catalog。

## 2026-09-07 上线前审查修复（RFC-0040）

- 修复 HostTensorBuffer 分配失败仍被视为成功的问题；LLM 生成参数在 Definition 预检与 Node 初始化中共用校验，非法 stop_words 在模型加载前拒绝。
- 完整门禁显式开启 BUILD_TESTING，固定默认 Backend 与 sanitizer 选项，CTest 无测试时失败；CI 验收 JSON 补齐 Whisper 结果，增加脚本行为回归。
- LayerGuard 增加按解析 include 路径的依赖矩阵与 vendor 所有权检查，覆盖公共 Node 头、Core/Engine 反向引用与局部辅助头。
- 实体抽取 biz_name 统一为 `entity_extract_v1`，model_id 为 `entity_llm`；文档问答部署统一使用 `smart_doc_qa_v1`。旧规模/Backend 专属 biz_name 不再接受，仓库配置和 Demo 同步迁移，外部配置须同步修改。
- INode/NodeBase 头文件按实际类型命名，共享 BERT tokenizer 移至 bge_common；合并 TextRerankNode 测试，统一测试替身的 TestBiz 名称及当前架构图签名。
- 选择验证工具输出 schema_version 升为 2，`ready_for_business` 改为 `ready_for_biz`；消费者需更新字段名，旧效果证据需重新生成。

## 2026-09-07 架构职责命名统一

- 统一使用接入适配层（Integration）、流程编排层（Orchestration）、能力节点层（Capability Nodes）、模型执行层（Model Execution），同步架构图、开发指南和 Agent 路由。
- 内部 CMake 对象与依赖目标改用职责名称；自定义节点脚手架和分层检查同步更新。SDK 产物、公共接口、配置格式和运行行为保持不变。
- 开发 skill 参考文件改为 `integration.md`、`orchestration.md`、`capability-nodes.md`、`model-execution.md`，历史 RFC 和验收记录保留原始名称。

## 2026-09-06 自定义 Node 渐进式入门

- 新增[动手练习](dev_guide/first_custom_node.md)与[五个概念说明](dev_guide/custom_node_concepts.md)，从一次真实运行解释端口、来源编号、模型绑定、Definition 与并发声明。
- `--kind model -m llm` 直接使用可阅读的轻量 C++ 模板，作者先填写 `BuildPrompt`、`FormatAnswer` 两个文本函数；保留既有基类、注册机制和失败检查。
- 文档中的业务函数体进入现有 Node runner 编译和执行，验证提示词、回答清理、输入快照与来源；完整 PromptGuidedLlmNode 转为按需查阅的进阶参考。
- 根目录、开发者指南和 custom Node 入口增加任务导航；教学模板只作为生成器输入和测试材料，未新增内置生产节点。

## 2026-09-06 自定义节点开发路径修复（RFC-0039）

- 修复脚手架的 C++ 接口、端口数量关系解析、模型签名和注册测试片段；生成代码进入既有 Node runner 的编译和执行，默认保守声明并发能力。
- PromptGuidedLlmNode 只替换原始模板，校验 context 连接与 stop_words；模型失败、数量或来源异常时不发布输出。删除并拒绝 `fallback_text`，模板字面花括号使用 `{{` / `}}`，上下文通过 `{context}` 显式插入。
- 新增实体抽取、文档问答两个 custom smoke Profile，复用同一节点与现有 Adapter，验证统一 Demo 输出内容及请求编号。
- Demo 类型绑定完全由必填注册描述符提供，删除中央业务名兜底；明确新平台结构仍需 Layer 1 转换。
- 更新[作者指南](../src/custom_nodes/README.md)和[实施规划](plans/solution_developer_acceptance.md)，区分工程路径、开发者试用与生产验收。

## 2026-09-06 方案开发者自定义 Node 脚手架与研发支持

- 新增 `scripts/scaffold_custom_node.py` 脚手架工具，支持一键生成 compute、model 及 unary_inference 三类合规自定义节点源码框架。
- 自动化生成类型端口契约、NodeDefinition、Layer 3 隔离头文件引用、(req_id, sub_id) 溯源保留循环与 Google Test 单元测试桩。
- 支持 `--add-to-cmake` 自动登记至 `src/custom_nodes/CMakeLists.txt`，提供内置自测与 `tests/tooling/test_scaffold_custom_node.py` 回归覆盖。
- 更新 `src/custom_nodes/README.md` 与方案开发者实施规划，闭环 S1 与 S2 阶段交付。

## 2026-09-06 自定义 Node 扩展目录

- 新增 `src/custom_nodes/` 统一存放领域算法和特定前后处理，按操作组织文件，可跨方案复用。
- 自定义与通用 Node 共用现有 Layer 3 构建、基类、Definition 和注册机制；补充接入指南，
  并将分层检查覆盖到自定义节点及其依赖边界。
- 采用 [RFC-0038](rfcs/0038-custom-node-extension-directory.md) 的目录准入规则；本次仅准备
  源码扩展入口，不新增具体算法或运行时接口。

## 2026-09-06 审计整改（阶段 5）

- 资产组合与下载脚本共用 SHA-256 清单，包含权重、tokenizer、运行配置和 projector；Studio 可选资产组合并校验已应用方案的构建/文件身份。
- 增加 default-cpu、kite-cpu、minimal 构建 preset；选择检查对比实际 Catalog 并记录执行文件指纹。
- 增加基于真实 Demo 执行、标注字段比较及指纹过期检测的业务验收工具；关闭 Demo 默认覆盖，保证评估所选 Pipeline。
- 收敛 Whisper 示例的模型根路径约定。新增资产损坏/缺失/变更、构建不匹配、实际效果与旧证据失效测试。
- [交付记录](archive/AUDIT_REMEDIATION_REPORT_2026-09-06.md) 与 [使用指南](VERIFIABLE_SELECTION.md) 说明实际验证范围和未完成的其他审计事项。

## 2026-09-06 审计整改（阶段 4）

- 七个内置 Adapter 用同一份 PackTyped 逻辑生成固定 C ABI 输出与 Operator 可变长结果，去除 Operator 中间固定字符串数组瓶颈。
- 单槽 Operator bridge 共用描述符/槽位/结果分配构造函数；新增 [业务接入说明](dev_guide/business_onboarding.md)。
- 新增长文档结果与真实 Operator 大结果、容量失败回滚/租约复用验证；统一门禁 89 项通过，公共 C11 ABI 与已有业务集成回归成功。

## 2026-09-06 审计整改（阶段 3）

- Studio 显示业务输入/输出与逐个类型端口，拖线生成实际 ports 和 depends_on；拒绝类型不兼容、成环和重复业务输出生产者，支持断线及删除节点清理。
- 新增完整模型实例表单：按协议筛选 Backend，编辑 Model/Backend 的 Catalog 字段，重命名自动更新节点引用，使用中的模型不能直接删除。
- 节点配置保留单一表单入口，画布按端口高度布局并自适应缩放。
- 统一门禁 89 项通过；Chromium 实际操作验证了模型创建、端口拖线及原生 Validator 校验，无页面异常。

## 2026-09-06 审计整改（阶段 2）

- Definition 提供无副作用的节点语义校验，复用模板/规则编译器和结构化 JSON 配置解析；切片跨字段、语料元素类型等问题在规划时拒绝。
- Node 初始化优先消费已验证计划中的配置，未连线可选端口的端到端执行语义已加入回归。
- 统一门禁通过，89 项 CTest 全部成功；资源文件、模型加载与实际输入效果仍属于运行与验收阶段。

## 2026-09-06 审计整改（阶段 1）

- 未连接的可选端口不再隐式读取默认 Blackboard 键；共享候选库需显式设置 `candidate_scope=shared`。
- Adapter 按请求编号关联完整结果，拒绝重复/越界/缺失来源；审核按请求选取首位策略。
- 结构化解析的诊断与回退不再作为成功业务结果；审核失败关闭，校验风险字段值域，规则错误状态向外传递。
- 新增空解析、跨请求检索、向量维度、乱序/重复结果和审核回退回归用例。

本文档记录 LLM-EdgeFlow 的架构里程碑与用户可感知变更。完整的设计动机、接口细节和验收证据由对应 RFC 维护；测试数量等易过时信息不在此重复记录。

仓库当前尚未发布对应 Git tag，因此以下版本号表示项目里程碑，而非可下载的正式 Release。

## Unreleased

- 新增 `audio_transcription` 中性执行协议、`whisper_asr` Model 与可选 `whisper_cpp`
  Backend，实现离线短语音识别接入；复用原有 `AsrTranscribeNode` 与既有 GGML 运行时
  目标，补充 16 kHz 单声道 float32 PCM 前置校验、UTF-8 边界保障、RAII whisper state
  生命周期管理与 `audio_asr_whisper` 真实模型 Demo 剖面，见
  [RFC-0036](rfcs/0036-whisper-asr-backend.md)。

- Kite 新增 `generated_token_embedding` 中性协议和 `generated_text_embedding` Model，
  接入原生生成 token 隐藏向量；Model 负责池化/归一化，复用现有 Embedding Node。
  提供纯 Kite 问答配置，保留 BGE/ONNX 路径；生成向量不声明与 BGE 检索质量等价，见
  [RFC-0035](rfcs/0035-generated-token-embedding.md)。

- Kite 新增中性图像文本生成协议与 vision_document OCR Model，复用原有 OCR
  Node/Operator，提供文本转写而不伪造检测框；补齐 Kite 文本、混合 ONNX 与图像
  Demo 配置、独立 profiles 文件选择及固定视觉模型下载，见
  [RFC-0034](rfcs/0034-kitellm-capability-coverage.md)。

- kiteLLM 改用原生 `SetDeviceId` 接收现有执行目标，解除 run-config 独占设备的本地限制；
  保留四层架构与 Operator 接口，使 CPU 设备 0 可通过既有 Demo/Operator 使用 Kite
  文本生成。增加平台/运行配置冲突校验及真实模型 Demo 回归，见
  [RFC-0033](rfcs/0033-kitellm-native-device-contract.md)。
- CI 新增 kiteLLM 私有发布包下载、静态链接及真实 GGUF 回归任务，通过专用 Actions
  secret 授权；结果纳入验收记录，外部 fork / Dependabot 明确跳过私有任务。
- kiteLLM 改为从 GitHub 私有 Release 下载并校验固定版本，直接链接 `kiteLLM.h` /
  `libkiteLLM.a`；移除外置 EdgeFlow C bridge 依赖。Linux x86_64/aarch64 可用，
  因内含不同版本的 llama.cpp/ggml，与独立 llama_cpp 后端互斥。
  使用与授权见 [接入说明](kitellm.md)，设计见 [RFC-0032](rfcs/0032-kitellm-direct-github-dependency.md)。
- Pipeline Studio 的 Python 服务端、Web 资源与文档收敛到统一模块目录；根目录 `show`
  保持稳定入口，原生 `alg_show` 改为忠实展示显式 `id` / `depends_on`，不再把 JSON
  数组顺序误画成串行 DAG。
- 修复 Pipeline Studio Web 工作台中已弃用的 `ensureExplicit` 残留调用导致的属性应用与节点删除未定义异常；实现基于 DAG 拓扑最长路径的自动分层布局（Topological Layering），补齐由 Catalog capability 约束的模型绑定与枚举字段下拉选择，并在 SVG 画布上提供可随草稿变更失效的校验错误节点联动高亮。
- 修复 Pipeline Studio 连续应用不同业务 Raw JSON 时 Catalog 异步响应乱序覆盖当前节点 Definition 的竞态；业务切换期间清空旧 Catalog 控件，并确保只有最新请求可更新工作台状态或报告错误。
- 删除仅用于过渡的 `core/traceable_item.h` 转发头，仓库内使用方直接依赖中立
  `contracts/traceable_item.h`。
- 四层生产源码改为独立 OBJECT target 编译，并由显式 Composition Root 聚合；LayerGuard
  同步校验源码归属、依赖方向及 Node 对轻量 `ValidatedNodePlan` 的使用。
- 业务 ingress/egress Blackboard key 从 Core 迁至 Layer 1 Adapter，Core、Node 与
  Engine 继续只持有中性值类型和逻辑端口契约。
- 保留 `3rdparty/` 持久缓存和自动归档方案，为所有固定依赖增加来源与 ABI 兼容指纹；
  缺失或不匹配的缓存不再被静默复用。
- 设计依据：[RFC-0029](rfcs/0029-external-readiness-and-intranet-sdk-migration.md)、
  [RFC-0030](rfcs/0030-compile-time-layer-boundaries.md)、
  [RFC-0031](rfcs/0031-business-blackboard-key-ownership.md)。

## 10.0.0 - 2026-09

- Validator 现在把业务 ingress 视为 write-once producer，Node 输出绑定与 ingress
  重名会在规划期以 `DUPLICATE_PORT_PRODUCER` 拒绝，不再延迟到运行时失败。
- 产品版本与 ABI 版本由 CMake 单一生成；v10.0.0 / ABI 5 共享库只导出 6 个算法 C ABI、
  3 个日志 C API 和 3 个 Operator API，内部工具和测试不再依赖泄漏的 C++ 动态符号。
- `SessionContext` 资源改用 `SessionResourceKey<T>` 并在 cast 前校验类型；
  `PipelineCatalog` 查询改为独立值/稳定快照，避免并发注册使引用和指针失效。
- 在正式接入前将主体 C++ 根命名空间从历史 `alg_framework` 原子收敛为
  `llm_edgeflow`；六个纯 C ABI、Operator API、共享库文件名与 C ABI major 保持不变。
- 生产库、Demo、开发 fixture、工具与测试改由所属目录显式管理 CMake 源码；新增
  `llm_edgeflow::sdk` target alias，并通过标准 `BUILD_TESTING` 隔离测试依赖。
- 测试树按 unit、integration、contract、tooling 与 e2e 责任重组；sharded 与
  individual 模式共用单一源码清单，历史 stage fixture 路径改为稳定语义路径。
- 设计依据：[RFC-0027](rfcs/0027-preproduction-source-layout-and-namespace-convergence.md)、
  [RFC-0028](rfcs/0028-preproduction-runtime-and-abi-hardening.md)。

## 9.0.0 - 2026-09

- 新增统一 `text_generation` Backend 协议与公共自回归采样器；同一
  `qwen_causal_lm` 可组合 llama.cpp 与条件 kiteLLM SDK，`LlmGenerateNode` 同步开放
  `top_k` 与 `repetition_penalty`；ONNX Runtime 保持强类型 `tensor_graph` 能力，Backend
  参数继续由各自 Definition 在规划期校验，kite 仅声明 SDK 原生 `run_config_file`。设计依据：
  [RFC-0026](rfcs/0026-unified-llm-generation-backends.md)。
- 引入统一 `3rdparty/` 持久化预编译归档体系，支持 llama.cpp、ONNX Runtime、GoogleTest、nlohmann/json 及 PCRE2 的本地静态库与头文件秒级复用，消除 clean build 与日常开发中的重复下载与源码重新编译耗时。

## 8.0.0 - 2026-09

- 冻结 `model_root_dir` 为直接包含 artifact 与 sidecar 的部署目录，并在 Layer 1
  将 C ABI、内存 JSON 与 Operator 路径统一解析为沙箱内绝对路径；Layer 2 不再解释
  部署根目录。
- 新增中性 `ExecutionTarget`，将 device/platform 贯通至生产 ONNX Runtime 与
  llama.cpp Backend；不支持的组合以稳定诊断 fail-closed。
- `ModelManager` 改为单一 registration 状态源；ONNX 非 Tensor I/O 在 metadata
  边界显式拒绝。
- real Profile 改用与当前 tokenizer 实现匹配的固定模型，并提供固定上游 Commit 与
  SHA-256 的 artifact/sidecar 获取脚本；mock Profile 统一声明 CPU emulator。
- 默认 CI 新增完整生产 Backend sanitizer、真实 GGUF C ABI 与 public real Profile
  job。
- 产品与共享库版本统一为 8.0.0，SONAME/C ABI major 统一为 5；内部 Biz Adapter
  descriptor 版本保持其独立的 2.0.0 契约。
- 设计依据：[RFC-0025](rfcs/0025-deployment-runtime-contract-convergence.md)。

- v7 在正式接入前删除未被外部使用的历史兼容层：Operator 后缀别名、
  `.conf` 双结构/单模型简写、隐式顺序 Pipeline 转换、Node 双字段与类型别名、
  重叠错误码，以及 Demo/CLI/Studio 兼容入口。当前契约对旧形状一律 fail-closed。
- 设计依据：[RFC-0024](rfcs/0024-pre-release-contract-cleanup.md)。
- v6 只保留 `biz_name`、`--biz` 与 Biz C++ API；Catalog 升级为 schema v2，并删除
  Business 双字段、双输出和类型/方法别名。
- Node 只保留 `Init(const NodeInitContext&)`；`AlgContext` 只保留 write-once
  `Publish/Read` 请求值契约，删除覆盖快照链与 `Set/Get/Erase/Clear` 迁移接口。
- 删除不参与 Validator 决策的 `PortDefinition::allow_override`，重复生产者继续统一
  fail-closed。
- 设计依据：[RFC-0023](rfcs/0023-v6-contract-convergence.md)。
- `TextTemplateNode` 的截断策略改为只在 UTF-8 code point 边界结束，不再产生残缺的中文或 emoji 字节序列。
- `TextRuleMatchNode` 支持原子地联合更新 categories 与 rules；任一候选无效时保留完整旧配置。
- 并行波前为每个 Node 捕获独立错误诊断，首个失败节点的错误码与消息不再被同层其他失败覆盖。
- Operator 值类型 Binding 统一持有显式 I/O 方向、输出字符串容量 Schema 和池预算；
  业务 Bridge 完整性改为按已注册 Adapter 快照审计，新增业务不再维护中央 ID 列表。
- `TextRuleMatchNode` 改用 Unicode PCRE2 语义，正确支持正/负 lookbehind 与命名捕获，
  消除 `(?<` 字符串重写导致的静默规则篡改。
- `TextChunkNode` 按 Unicode code point 执行 `chunk_size` 与 `overlap`，不再截断
  UTF-8 多字节字符；非法 UTF-8 输入统一 fail-close。
- 设计依据：[RFC-0022](rfcs/0022-text-processing-safety.md)。
- 统一 Traceable 推理批次的数量与 provenance 校验；LLM、Embedding、Rerank、OCR、ASR 对无效模型输出一致 fail-close，且无效 Embedding 结果不进入 Session 缓存。
- Layer 2 Pipeline 改为局部事务式装配，Node Init 失败不再向公开 Session
  暴露部分模型或执行资源。
- Validator 与 Blackboard 统一为 write-once 输出语义，显式 DAG 文档按严格
  object 与字段契约 fail-closed。
- 并行波前在节点异常时会等待同层已提交任务全部结束，再返回稳定错误。
- 设计依据：[RFC-0020](rfcs/0020-layer2-runtime-convergence.md)。
- Causal LM 序列改为自带执行行为和资源生命周期的中性协议，Backend 加载时
  可显式校验 Model 请求的执行协议。
- ONNX 批策略改为综合全部输入/输出 metadata；BGE Model 共用 BERT 构造期与
  输出张量基础契约，统一并发语义并拒绝非有限 Embedding 输出。
- 删除未被生产路径使用的旧 `FixedBatchExecutor` 重载，保留唯一
  `BatchPolicy + BatchSlice` 作者入口。
- 设计依据：[RFC-0021](rfcs/0021-layer4-authoring-and-protocol-convergence.md)。
- 移除不再支持的 Claude Code 与 Gemini CLI 专用 Agent 入口，并将版本演进摘要归档至 `doc/CHANGELOG.md`。
- 将 Node Definition 默认值归一化结果设为 Pipeline 运行时配置的单一事实源。
- 收敛 Operator 绑定/发布事务、Model/Backend Registry 与 BERT 模型公共机制，公共 ABI 和 Catalog 保持不变。
- 统一分片与逐测试模式的必备契约清单并补齐模式间遗漏。
- 设计依据：[RFC-0019](rfcs/0019-high-priority-layer-convergence.md)。
- 收敛请求黑板为稳定只读快照与单次发布契约，保留现有业务的兼容迁移入口。
- 同一 C ABI handle 的 Process/Control 改为串行执行，并明确 Destroy 前停流与等待契约。
- 设计依据：[RFC-0018](rfcs/0018-request-context-and-handle-concurrency-contracts.md)。
- 收敛 Agent、技能、RFC、测试与 GitHub 交付治理，消除重复门禁和架构漂移。
- GitHub 交付默认停在已验证 PR；合并需显式授权，不再回退直接推送 `main`。
- 设计依据：[RFC-0017](rfcs/0017-development-governance-convergence.md)。

## 5.1.0 - 2026-08

- 将完整构建与全部 CTest 收敛为单一质量门禁。
- 由环境管理 ccache，并固定第三方依赖版本与 SHA256 校验值。
- 补齐干净构建依赖和默认测试超时。
- 设计依据：[RFC-0016](rfcs/0016-build-and-test-workflow-convergence.md)。

## 5.0.0 - 2026-08

- 建立 Model / Backend 双注册体系和中性执行协议。
- Embedding、Rerank、LLM、OCR、ASR 改用强类型模型能力契约。
- 移除旧 Engine 路径，并隔离生产配置与确定性测试 Fixture。
- 设计依据：[RFC-0015](rfcs/0015-model-capability-backend-decoupling.md)。

## 4.3.0 - 2026-08

- 新增纯 C11 六级公共日志 API、线程安全进程级阈值和 Demo 环境变量配置。
- 设计依据：[RFC-0014](rfcs/0014-public-log-api.md)。

## 4.2.0 - 2026-08

- 收敛开发与 Sanitizer 构建模式、测试分片及标签化质量门禁。
- 标准上传脚本增加仅创建 PR 的安全交付模式。
- 设计依据：[RFC-0013](rfcs/0013-developer-feedback-loop-acceleration.md)。

## 4.1.0 - 2026-08

- 以 I/O 契约重构通用 Node，并引入显式逻辑端口绑定。
- Validator 增加端口执行契约和同波前写冲突检查。
- 设计依据：[RFC-0012](rfcs/0012-node-authoring-experience.md)。

## 4.0.0 - 2026-08

- 解耦算法 Operator 与底层计算 Platform 的概念、命名和目录结构。
- 设计依据：[RFC-0011](rfcs/0011-operator-platform-naming-unification.md)。

## 3.1.0 - 2026-08

- 将业务相关目录、类型、配置字段和 CLI 术语统一为 `biz`。
- 设计依据：[RFC-0010](rfcs/0010-business-to-biz-naming-unification.md)。

## 3.0.0 - 2026-08

- 引入纯 C11 平台值类型、命名槽位绑定和会话级输出内存池。
- Operator 配置支持模型路径与 Pipeline 配置路径解耦。
- 设计依据：[RFC-0009](rfcs/0009-company-string-and-slot-map-struct-binding.md)。

## 2.1.0 - 2026-08

- Pipeline 直接消费 `ValidatedPipelinePlan`，避免重复解析和 DAG 排序。
- 全部生产节点迁移到浅基类体系，并收敛架构图与验证门禁。
- 设计依据：[RFC-0008](rfcs/0008-architecture-contract-consolidation.md)。

## 2.0.0 - 2026-08

- 建立自描述注册、`BlackboardKey<T>` 强类型黑板和集中式 Pipeline 校验。
- 设计依据：[RFC-0008](rfcs/0008-architecture-contract-consolidation.md)。

## 1.6.0 - 2026-08

- 所有官方 Pipeline 配置迁移为显式 DAG，并移除隐式顺序兼容分支。
- 设计依据：[RFC-0007](rfcs/0007-explicit-dag-standardization-and-legacy-deprecation.md)。

## 1.5.1 - 2026-08

- 修正 Sanitizer、格式化和 C ABI 布局质量门禁，并校准审查证据。

## 1.5.0 - 2026-08

- 交付共享 Catalog/Validator 的 CLI 与可编辑 Web Pipeline Studio。
- 加入安全保存、草稿运行和工作台回归矩阵。
- 设计依据：[RFC-0006](rfcs/0006-visual-pipeline-studio.md)。

## 1.4.0 - 2026-08

- 交付参数化多业务 Demo Runner、Profile 清单和结构化结果输出。
- 设计依据：[RFC-0005](rfcs/0005-parameterized-business-demo-runner.md)。

## 1.3.0 - 2026-08

- 新增 Operator 函数表兼容门面、共享算法运行时和命名 I/O 派发。
- 设计依据：[RFC-0004](rfcs/0004-platform-operator-interface-compatibility.md)。

## 1.2.0 - 2026-08

- Pipeline 改为严格解析和一次性构建状态机，并强化注册冲突防护及结构化诊断。
- 设计依据：[RFC-0003](rfcs/0003-pipeline-dynamic-blackboard-rebaseline.md)。

## 1.1.0 - 2026-08

- 加固 Adapter 的容量、路径绑定、复杂结构和异常安全契约。
- 设计依据：[RFC-0002](rfcs/0002-c-abi-adapter-security-hardening.md)。

## 1.0.0 - 2026-08

- 建立纯 C ABI、四层架构、动态黑板、DAG 调度与定长批处理基线。
- 设计依据：[RFC-0001](rfcs/0001-four-tier-architecture-foundation.md)。
