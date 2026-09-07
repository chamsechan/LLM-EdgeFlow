# RFC 0040: 上线前实现、验证与命名收敛

- **RFC 编号**：0040-prelaunch-audit-convergence
- **创建日期**：2026-09-07
- **文档状态**：Completed
- **关联分支**：`fix/prelaunch-audit-convergence`
- **目标版本**：v10.x
- **负责人 / 作者**：Codex

## 1. 背景与范围

落实全仓审查确认的内存失败、配置校验、质量门禁、分层保护、验收证据及命名问题。
保留 Integration → Orchestration → Capability Nodes → Model Execution 四层。
不引入新运行时能力，不改变公共 C ABI 布局或导出符号，不接触内网 SDK。

## 2. 设计与迁移

- Model Execution：HostTensorBuffer 区分零长度与分配失败；共享 BERT tokenizer
  归入 `models/bge_common/`；ElementType 字符串转换只保留一份实现。
- Capability Nodes：LlmGenerateNode 的 Definition 与初始化共用生成配置解析和语义
  校验；`INode` 头命名为 `core/node_interface.h`，实际基类放在 `nodes/node_base.h`，
  端口支持与基类实现仍保持现有语义。
- Integration：相同 I/O 契约不再按模型或 Backend 注册多个 biz_name。实体抽取统一为
  `entity_extract_v1`，文档问答统一使用现有 `smart_doc_qa_v1`。旧
  `entity_extract_0.6b_v1`、`entity_extract_llamacpp_0.6b_v1`、
  `smart_doc_qa_onnx_llamacpp_v1`、`smart_doc_qa_rerank_llm_v1` 不保留运行时别名。
  全部仓库配置、Profile 引用及测试同步迁移；实体抽取 model_id 统一为 `entity_llm`。
  Pipeline 文件名和 Profile 名继续描述部署方案，不与 biz_name 混同。
- 测试注册使用 `TestBiz*` / `test_biz_*`，开发辅助文件相应改名；选择验证工具的
  输出 schema_version 升为 2，`ready_for_business` 改为 `ready_for_biz`。这是未上线工具输出的一次迁移，旧效果
  证据需重跑；历史 RFC、Changelog 与验收归档保留原貌。
- Tooling：门禁明确开启测试、固定 Backend/ sanitizer 开关并拒绝空测试集合。
  默认本地门禁保持 ONNX + llama.cpp，Whisper 继续由独立可选构建验收。
  LayerGuard 按实际 include 路径检查层归属、公共 Node 支持头与各 Backend 的 vendor
  头边界；共享中性 Core 契约使用显式清单。CMake 的聚合 include 搜索路径不宣称是
  完整的编译器访问隔离。
- CI 证据记录所有必需任务，包括 Whisper；更新当前类图中的真实方法签名。

## 3. 不变量与验证

分配失败不得产生成功 Tensor；配置错误必须在模型加载前被原生 Validator 拒绝。
Adapter/Operator 注册保持完整，生产 SDK 不链接测试替身。目录迁移不留重复实现或
兼容头。合并 TextRerankNode 的测试到已有 Node 套件，不增加专用测试进程。

- 在现有 ModelBackendDecouplingTest 中验证零长度、正常分配和 ENOMEM。
- 在现有 Node/Validator 套件验证 stop_words 和边界参数的初始化/预检一致性。
- 扩展架构脚本契约测试，覆盖关闭测试的缓存、空 CTest、所有 CI 证据字段及
  反向/间接 include 注入。
- 验证旧 biz_name 被拒绝，新名称唯一；运行迁移后的 Mock Demo 与配置规划。
- 运行一次 `./scripts/run_all_tests.sh`；必要时按失败范围修正并重跑。

## 4. 实施状态

- [x] 设计与迁移范围自评。
- [x] 实现与针对性验证。
- [x] 完整本地门禁与结果记录。

## 5. 验证结果（2026-09-07）

- `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh` 通过：89/89 个 CTest
  项成功，包含格式、构建、架构与脚本契约、业务集成及 9 个 Mock smoke Profile。
  原独立 RerankRefineNodeTest 已并入 TextRerankNodeTest，保留原有测试用例。
- 定向 Tensor 测试 4/4、LLM/重排 Node 测试 16/16 通过；实际 CLI 对
  `stop_words: [42]` 和 `[""]` 返回配置错误，退出码为 1。
- 变更涉及的 5 个默认生产 Pipeline 和 8 个 Mock Pipeline 均通过原生
  `validate` / `plan`；Mock 使用带测试注册的工具。核对 smoke 输出的请求编号、
  成功状态、摘要计数与业务字段，custom Demo 套件另断言具体业务结果。
- 关闭所有可选 Backend、设置 `LLM_EDGEFLOW_SHARDED_TEST_RUNNERS=OFF` 的
  独立测试模式 CMake 配置与生成通过；该模式未额外执行完整编译。
- 验证边界：本地默认门禁未启用 Whisper/Kite 或真实模型专用套件；Google Test
  内部有 8 个依赖可选 Backend/模型资产的用例跳过。两个变更的 Kite Pipeline
  在默认工具中报告 `UNKNOWN_BACKEND` 及关联模型引用错误，未宣称其通过目标
  Backend 验证。未进行内网 SDK、目标硬件或真实模型效果验收。
