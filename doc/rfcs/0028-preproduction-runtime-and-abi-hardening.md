# RFC 0028: v10.0.0 预发布运行时与 ABI 收口

- **RFC 编号**：0028-preproduction-runtime-and-abi-hardening
- **创建日期**：2026-09-02
- **文档状态**：Completed
- **关联分支**：`fix/preproduction-runtime-abi-hardening`
- **目标版本**：v10.0.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机

2026-09-02 架构审计确认了三项正式接入阻断问题：Validator 允许 Node 输出覆盖
Adapter ingress，但运行时 Blackboard 拒绝重复发布；公共 C 头中的产品版本仍为 8.0.0，
与 v10 构建不一致；共享库默认导出内部 Node、Registry 与 llama/ggml 符号。审计同时
发现 Session 资源的 `shared_ptr<void>` 未校验类型，以及 Catalog 返回的内部 vector
引用和元素指针会在后续注册时失效。

本 RFC 在不修改既有业务结构体、Pipeline Schema 和正式调用入口的前提下，关闭这些
运行时与交付风险。完成后仍需按实际部署环境另行执行真实模型、目标硬件、压力和性能
验收，不能仅凭本 RFC 宣称生产就绪。

## 2. 设计范围与边界

### 2.1 范围内

- Validator 拒绝 Node 输出与业务 ingress 的 write-once 键冲突。
- CMake 成为产品版本和 ABI 版本的单一事实源。
- 共享库只导出 6 个 `Alg_*`、3 个 `AlgBase_*` 和 3 个 Operator 入口。
- 内部工具和测试不再依赖共享库泄漏的 C++ 符号。
- Session 资源使用带运行时类型校验的 `SessionResourceKey<T>`。
- Catalog 查询返回值快照，不暴露可失效的引用或指针。

### 2.2 非目标

- 不修改 C ABI 函数、业务结构体、错误码、Operator 签名或 Pipeline Schema。
- 不在本 RFC 中完成 contracts/SPI 抽取、四层独立 target graph 或业务 BlackboardKey
  私有化。
- 不拆分 Core/Operator 大文件，不迁移 Adapter 模板，不重组重复 Node 测试。
- 不处理第三方缓存、install/export/package config 和正式发布打包策略。

## 3. 总体技术方案

### 3.1 架构分层映射

- **Layer 1**：公开函数增加统一导出宏；C ABI 和 Operator 行为不变。
- **Layer 2**：Validator 补齐 ingress 冲突规则；Session 和 Catalog 改用类型安全值语义。
- **Layer 3 / Layer 4**：运行时实现通过内部 OBJECT target 参与最终链接，不改变 Node、
  Model 或 Backend 语义和注册定义。
- **Tooling / Build**：版本头由 CMake 生成，Linux 动态符号由 version script 限制。

### 3.2 Validator write-once 规则

Node 的实际输出 key 在加入 producer 集合前必须同时检查 BizDefinition ingress 和已有
Node producer。与 ingress 冲突时沿用 `kDuplicatePortProducer`，诊断路径指向具体输出
绑定，关联生产者标识为 `$ingress`。运行时 `AlgContext::Publish` 和 `BoundOutput::Set`
继续作为防御边界。

### 3.3 版本与正式导出面

CMake 定义产品版本 10.0.0 和 ABI 版本 5.0.0，生成严格 C11 可包含的版本头，且
`SOVERSION` 从 ABI major 派生。正式动态导出面固定为：

1. `Alg_Init`、`Alg_Create`、`Alg_Process`、`Alg_Control`、`Alg_Destroy`、`Alg_DeInit`；
2. `AlgBase_setLogLevelByName`、`AlgBase_getLogLevelByName`、`AlgBase_logPrint`；
3. `Get_LLM_EDGEFLOW_OperatorTable`、`GetOperatorLastError`、
   `ValidateOperatorConfigBinding`。

GCC/Clang 默认使用 hidden visibility，并在 Linux 叠加 linker version script。内部实现
编入 OBJECT target；公共契约测试继续链接 `alg_sdk`，内部工具和测试直接消费内部对象。

### 3.4 Session 与 Catalog

`SessionResourceKey<T>` 同时携带资源名和静态类型。资源槽与 single-flight 槽保存
`std::type_index`，任何 cast 前都验证类型；同名异型访问抛出 `std::logic_error` 且不
覆盖原资源。

Catalog 不冻结。`Nodes()`、`Bizs()` 返回 vector 值，`FindNode()`、`FindBiz()` 返回
`std::optional` 值；Validator 在单次规划开头捕获稳定快照，后续注册不会使本次规划的
对象失效。

## 4. 关键不变量与兼容策略

1. 产品版本保持 v10.0.0，C ABI major 与 SOVERSION 保持 5。
2. 12 个正式入口的名称、签名、异常屏障和行为保持不变。
3. Core、Node、Model、Backend C++ 类型不属于稳定动态 ABI；外部调用方只能使用正式
   C ABI、日志 API 或 Operator API。
4. Validator 是唯一规划实现，CLI、Web 和 Pipeline 不复制 ingress 冲突规则。
5. Catalog 查询值在返回后具有独立生命周期；Session 资源不得执行未校验的 void cast。

## 5. 测试与质量验收计划

- 扩展 Validator 与 CLI 测试，证明输出覆盖 ingress 返回
  `DUPLICATE_PORT_PRODUCER`，正常 ingress 消费和现有 Pipeline 保持有效。
- 扩展 C11、日志和 Operator 契约测试，验证生成版本、ABI major、SONAME 和 12 个入口。
- Linux 使用 `nm -D --defined-only` 校验完整 allowlist，禁止内部 `llm_edgeflow` 与
  `llama/ggml` 符号泄漏。
- 扩展 Session 测试，覆盖正确/错误类型、动态 key、single-flight 与并发访问。
- 扩展 Catalog 测试，覆盖注册、排序和清理后的快照稳定性及并发查询。
- 验证登记的生产 Pipeline、Catalog 和 Demo smoke；运行 ASan/UBSan 与 TSan 专项。
- 最后运行一次 `./scripts/run_all_tests.sh`，通过后才将 RFC 状态改为 Completed。

## 6. 实施路线

1. [x] 建立 RFC，冻结 v10.0.0 / ABI 5 与 12 个正式入口。
2. [x] 修复 Validator ingress 冲突并增加回归测试。
3. [x] 统一版本头、内部链接模型和共享库导出边界。
4. [x] 收敛 Session 与 Catalog 生命周期契约。
5. [x] 更新活跃文档并完成专项和统一质量门禁。

## 7. 验收证据

- `./scripts/run_all_tests.sh`：默认 Backend 完整构建，86/86 CTest 通过；其中包括全部
  登记 Pipeline、Catalog/Validator、C11 ABI、Operator、Demo smoke、文档漂移和
  `SdkExportSurfaceTest`。
- `LLM_EDGEFLOW_SANITIZERS=address,undefined ./scripts/run_sanitizers.sh --fast`：
  82/82 sanitizer-compatible 测试通过。
- `LLM_EDGEFLOW_SANITIZERS=thread ./scripts/run_sanitizers.sh --fast`：81/81 测试通过，
  未发现数据竞争。
- `BUILD_TESTING=OFF` 可构建 SDK、Demo 与 Pipeline CLI；共享库 SONAME 为
  `libcompany_alg_sdk.so.5`，动态定义表与 12 符号 allowlist 完全一致。
- `LLM_EDGEFLOW_SHARDED_TEST_RUNNERS=OFF` 可配置并构建本 RFC 的代表目标；
  ingress 冲突、Catalog 快照、C11 ABI 与导出面聚焦测试通过。individual 模式中部分
  历史测试仍缺少各自的 dev fixture object wiring，此既有测试基础设施问题不属于本 RFC
  的运行时与对外 ABI 收口范围。
- 真实模型资产、目标硬件、容量压力和性能基线未在本地门禁中执行，仍需在具体部署环境
  完成正式接入验收。

## 8. 变更记录

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-09-02 | v1.0.0 | 建立预发布运行时与 ABI 收口方案 | LLM-EdgeFlow Team |
| 2026-09-02 | v1.1.0 | 完成实现、文档同步与默认/sanitizer 专项验收 | LLM-EdgeFlow Team |
