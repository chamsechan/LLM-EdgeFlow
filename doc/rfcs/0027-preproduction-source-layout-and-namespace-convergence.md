# RFC 0027: 正式接入前源码布局与 C++ 命名空间收敛

- **RFC 编号**：0027-preproduction-source-layout-and-namespace-convergence
- **创建日期**：2026-09-02
- **文档状态**：Completed
- **关联分支**：`refactor/preproduction-structure-cleanup`
- **目标版本**：v10.0.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机

仓库尚未正式投入生产，但源码仍保留早期 `AlgSdkFramework` / `alg_framework`
命名。产品、配置、构建选项和 Operator 公共接口已经统一为 LLM-EdgeFlow 与
`llm_edgeflow::operator_api`，主体 C++ 类型却继续暴露另一套根命名空间。当前根
`CMakeLists.txt` 同时维护生产库、Demo、工具和测试源码清单，测试目录根部堆积数十个
不同责任的测试文件，sharded 与 individual 模式也重复维护路径和目标属性。

正式接入后再修改根命名空间会要求所有 C++ 使用方重新编译。当前预发布窗口允许在不
保留历史别名的前提下一次收敛，并同时把构建和测试所有权调整到稳定的语义边界。

预期收益：

- 产品、CMake consumer target、Operator 和主体 C++ 类型使用同一个品牌命名；
- 新增源码时在所属目录显式登记，不再扩大根 CMake 清单；
- 测试的文件位置表达被测责任，执行 tier 继续由 CTest label 独立表达；
- mock runtime、Demo fixture 与纯测试 helper 的所有权清晰，减少重复编译和路径耦合。

## 2. 设计范围与边界

### 2.1 范围内

- 将活跃生产源码、测试、工具和当前开发文档中的 `alg_framework` 原子迁移为
  `llm_edgeflow`。
- 将 CMake project 名统一为 `LLMEdgeFlow`，提供 `llm_edgeflow::sdk` target alias。
- 保留共享库输出名 `company_alg_sdk` 和 C ABI major/SOVERSION 5。
- 使用 `add_subdirectory()` 与目录内显式 `target_sources()` 管理生产、Demo、工具和
  测试构建。
- 使用标准 `BUILD_TESTING` 开关隔离 GoogleTest、CTest 和测试专用目标。
- 将重复链接的确定性 inference fixture 收敛为显式开发支持 target。
- 按 unit、integration、contract、tooling、e2e、support 和 fixtures 重组测试路径，
  将历史 stage 编号改为语义路径。
- 保持 sharded 与 individual 两种测试模式的必备契约清单和 CTest 名称不变。

### 2.2 非目标

- 不修改六个 `Alg_*` C ABI 名称、签名、异常屏障、结构体布局或错误码。
- 不修改 `llm_edgeflow::operator_api` 的类型、函数表或运行时行为。
- 不修改 Pipeline Schema、Catalog Definition、Node、Model 或 Backend 行为。
- 不通过普通 static library 拆分四层运行时；静态注册对象的链接保留需要独立设计。
- 本 RFC 不建立 ELF/macOS/Windows 动态符号白名单。当前工具和测试通过共享库消费
  主体 C++ 类型，符号隐藏需要先拆分受支持公共接口与内部链接模型。
- 不为了缩短文件机械拆分仍由同一 fixture、生命周期或集成场景共同拥有的测试。

## 3. 总体技术方案与架构设计

### 3.1 架构分层映射

- **Layer 1**：Adapter、C ABI 实现和 Operator bridge 只迁移根 C++ namespace；六个
  C ABI 继续位于全局 `extern "C"` 边界。
- **Layer 2**：Core 类型迁移根 namespace，Pipeline/Validator/Blackboard 行为不变。
- **Layer 3**：Node 实现、注册宏与 Definition 类型迁移根 namespace，注册方式不变。
- **Layer 4**：Model、Backend、中性协议和注册宏迁移根 namespace，vendor header
  边界不变。
- **Tooling/Test**：CMake 和测试目录按上述责任边界组织，不改变四层依赖方向。

### 3.2 接口与构建布局

```cpp
namespace llm_edgeflow {
// Core, Node, Model, Backend and Adapter C++ types.
}

namespace llm_edgeflow::operator_api {
// Existing supported Operator API; unchanged.
}

extern "C" {
int Alg_Init(void) noexcept;
// The other five existing Alg_* functions remain unchanged.
}
```

```cmake
add_library(alg_sdk SHARED)
add_library(llm_edgeflow::sdk ALIAS alg_sdk)
add_subdirectory(src)

# src/core/CMakeLists.txt
target_sources(alg_sdk PRIVATE pipeline.cpp pipeline_validator.cpp)
```

子目录仍显式列出源文件。禁止使用 `aux_source_directory()` 或递归 GLOB 隐式收集生产
注册对象。目录拆分只转移源码清单所有权，不额外制造运行时 library 边界。

### 3.3 测试布局

```text
tests/
  unit/{core,nodes,engine,adapter,operator,logging}/
  integration/{pipeline,runtime,demo,operator}/
  contract/{abi,architecture,catalog}/
  tooling/
  e2e/real_models/
  support/
  fixtures/{models,pipelines}/
```

CTest 的 tier、slow、integration、tooling 和 sanitizer label 是执行维度，不编码进
目录名。Demo 使用的 mock Pipeline 与确定性 runtime support 不再伪装成 test-only
文件；测试可以复用这些开发资产，但生产 `alg_sdk` 仍不得链接 fixture 注册。

## 4. 关键设计考量与不变量

1. **预发布兼容策略**：不提供 `namespace alg_framework = llm_edgeflow`。仓库外 C++
   使用方必须重新编译；尚未正式接入使这一成本可控。
2. **C ABI 稳定**：namespace 迁移不得进入 C11 头部或改变六个导出函数。SOVERSION 5
   继续表达当前 C ABI major。
3. **Operator 稳定**：现有 `llm_edgeflow::operator_api` 不重命名，因此其源码 namespace
   和类型契约保持稳定。
4. **静态注册完整性**：生产源码继续直接属于 `alg_sdk` target。测试 fixture 可使用
   OBJECT library，确保对象被显式链接到每个需要独立 Registry 的进程。
5. **历史文档不可重写**：既有 RFC 和验收报告保留原始 `alg_framework` 记录；只更新
   活跃架构文档、开发指南和示例。
6. **测试身份稳定**：CTest 名称和 required inventory 不因文件迁移变化，现有过滤器
   和隔离进程语义必须保留。

## 5. 测试与质量验收计划

- [x] C11 ABI compliance 与 C ABI safety 证明纯 C 边界未变化。
- [x] Operator、Adapter、Catalog、Registry、Pipeline、Node、Model/Backend 套件保持通过。
- [x] sharded 默认模式通过，并由 required inventory 证明必备契约未遗漏。
- [x] 单独配置 `LLM_EDGEFLOW_SHARDED_TEST_RUNNERS=OFF`，验证 individual 模式可配置并
  构建至少一个代表性目标。
- [x] `BUILD_TESTING=OFF` 可配置并构建 SDK、Demo 和工具，且不要求 GoogleTest。
- [x] 活跃源码和当前文档中不存在 `alg_framework`，本迁移 RFC 与历史 RFC/review 除外。
- [x] 最终运行一次 `./scripts/run_all_tests.sh` canonical gate；默认后端完整构建和
  85/85 CTest 通过。

## 6. 实施路线与里程碑

1. [x] 建立 RFC，冻结预发布兼容边界和目标布局。
2. [x] 原子迁移 namespace、project 名和当前文档。
3. [x] 分层 CMake，建立开发 fixture target 和 `BUILD_TESTING` 隔离。
4. [x] 迁移测试、Demo fixtures，更新所有路径与构建清单。
5. [x] 完成 focused 配置/构建验证和 canonical gate。
6. [x] 更新 Changelog，并在验证通过后将 RFC 标记为 Completed。

## 7. 变更记录

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-09-02 | v1.0.0 | 建立预发布 namespace、构建与测试布局收敛方案 | LLM-EdgeFlow Team |
| 2026-09-02 | v1.1.0 | 完成实现、双测试模式验证与 85 项 canonical CTest 验收 | LLM-EdgeFlow Team |
