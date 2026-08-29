# RFC 0016: 构建与测试工作流收敛

- **RFC 编号**：0016-build-and-test-workflow-convergence
- **创建日期**：2026-08-29
- **文档状态**：In Implementation
- **关联分支**：`feat/build-test-workflow-optimization`
- **目标版本**：v5.1.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机 (Motivation & Context)

RFC-0013 已引入分片测试 Runner、PCH、CTest 标签、ccache 和 fast/full
门禁。RFC-0015 完成后，构建矩阵新增 ONNX Runtime 与 llama.cpp 后端，日常
反馈闭环出现以下可复现问题：

1. `--fast` 同时改变构建类型、优化等级、后端能力和测试范围，语义过载；
2. `edgeflow_dev_tests` 未构建 `VisualizerServerTest` 依赖的
   `alg_pipeline_tool_test`，干净 fast 构建只能通过 79/80 项测试；
3. ccache launcher 与用户 PATH 中的 ccache compiler wrapper 可能重复包装；
4. FetchContent URL 缺少哈希，llama.cpp 追踪浮动 `master`，破坏离线复用、
   供应链校验和可复现构建；
5. quick 与 full 使用不同构建目录，增量开发无法复用完整后端构建产物。

在 AppleClang 16、Ninja、`-j8`、关闭 ccache 且复用同一依赖源码的基准中：

| 场景 | 旧 fast | full |
| :--- | ---: | ---: |
| 干净配置 | 2.11s | 3.13s |
| 干净构建 | 47.53s | 64.80s |
| 编译任务 | 约 164 | 378 |
| 增量配置 + 无变化构建 | 1.58s | 1.85s |
| `dev-fast` 标签测试 | 0.77s / 80 项 | 0.84s / 82 项 |
| 全量测试 | 不含 slow gate | 6.71s / 84 项 |

结果表明：完整后端的干净构建成本仍需保留显式 minimal 逃生路径，但日常 quick
测试可以直接复用 full-capability 构建，无需维护第二套常用构建树。

## 2. 设计范围与边界 (Scope & Non-Goals)

### 2.1 范围内 (In-Scope)

- [ ] `--quick` 与 `--full` 共享 `build/` 和完整后端能力；无参数仍为 full。
- [ ] 旧 `--fast` 保留为 `--quick` 兼容别名，避免现有自动化立即失效。
- [ ] 新增显式 `--minimal`，在独立构建目录中关闭 ONNX Runtime/llama.cpp，
      验证可选后端关闭时的编译与测试能力。
- [ ] 修复 `edgeflow_dev_tests` 的完整依赖闭包并增加静态工作流契约测试。
- [ ] ccache 继续使用 `find_program` 跨平台发现；不写死程序或缓存路径；编译器
      已是 ccache wrapper 时跳过 launcher，避免双重包装。
- [ ] 固定所有 FetchContent URL 的 SHA256；llama.cpp 固定到确定提交；使用
      现代 FetchContent API，并允许 minimal/sanitizer 复用已获取的源码目录。
- [ ] 为所有默认测试设置有限超时，避免门禁无限挂起。
- [ ] 更新 README 开发命令、Changelog、RFC 索引与 CI 缓存键。

### 2.2 非目标 (Non-Goals / Out-of-Scope)

- 不修改 Layer 1 C ABI、Layer 2 Pipeline/Blackboard、Layer 3 Node 或 Layer 4
  Engine 的运行时行为。
- 不降低默认 full 门禁覆盖率，不将 quick/minimal 作为交付门禁替代品。
- 不引入 Unity Build，不改变测试进程隔离和 Registry 冲突测试语义。
- 不强制开发者使用仓库内 `CCACHE_DIR`，也不假设固定平台安装路径。
- 不把真实模型权重测试并入默认本地门禁。

## 3. 总体技术方案与架构设计 (Architecture & Technical Design)

### 3.1 架构分层映射 (4-Tier Mapping)

- **Layer 1 (C ABI / Platform Adapter)**：无生产变更，继续执行 C11 与异常安全门禁。
- **Layer 2 (Pipeline & Blackboard)**：无生产变更，继续执行 core/validator 测试。
- **Layer 3 (Business & Common Nodes)**：无生产变更，继续执行 nodes 与 Demo smoke。
- **Layer 4 (Engines & Hardware Acceleration)**：不改变后端接口；仅固定第三方源码
  版本并保留 enabled/disabled 双配置编译验证。
- **Tooling / Test Infrastructure**：本 RFC 的实现归属。

### 3.2 核心接口与数据流设计 (Interface & Data Flow)

```text
run_all_tests.sh [--quick|--fast]
  -> build/ (Release, ONNX Runtime ON, llama.cpp ON)
  -> build edgeflow_dev_tests
  -> ctest -L dev-fast

run_all_tests.sh [--full]  # 默认
  -> build/ (与 quick 同一配置和产物)
  -> build all
  -> ctest all labels

run_all_tests.sh --minimal
  -> build-minimal/ (Debug + O1, optional engines OFF)
  -> build edgeflow_dev_tests
  -> ctest -L dev-fast

CMake compiler cache selection
  -> find_program(ccache)
  -> resolve compiler and ccache real paths
  -> compiler already resolves to ccache ? skip launcher : set launcher
```

### 3.3 依赖获取与离线复用

所有归档 URL 必须声明 `URL_HASH SHA256=<digest>`。llama.cpp 使用确定提交归档，
禁止使用分支浮动 URL。已有合法源码树可以通过标准
`FETCHCONTENT_SOURCE_DIR_<NAME>` 覆盖，离线环境在源码齐备后可启用
`FETCHCONTENT_FULLY_DISCONNECTED=ON`。

## 4. 关键设计考量与权衡 (Design Trade-offs & Invariants)

1. **默认门禁不变**：无参数 `run_all_tests.sh` 仍等价于 `--full`。
2. **常用构建复用**：quick/full 的 CMake 能力与构建类型完全一致，切换测试范围
   不触发后端重配或大规模重编译。
3. **最小配置可验证**：minimal 明确承担后端关闭、离线和 stub 路径的兼容验证。
4. **ccache 环境自治**：项目只负责发现和 launcher 选择；缓存目录由 ccache、用户
   或 CI 环境管理。
5. **依赖可复现**：任何第三方版本变化都必须通过 URL/提交与 SHA256 的代码审查。
6. **完整目标闭包**：被 quick 标签测试引用的每个可执行文件都必须属于
   `edgeflow_dev_tests` 依赖。
7. **Fail-Closed**：依赖哈希不匹配、显式 linker 不可用或测试超时必须使门禁失败。

## 5. 测试与质量验收计划 (Testing & Verification Plan)

- [ ] CMake 脚本测试：普通编译器启用 launcher，ccache wrapper 跳过 launcher。
- [ ] 工作流契约测试：quick/full 共享 `build/`，minimal 使用隔离目录，旧 fast
      映射 quick，dev target 包含 `alg_pipeline_tool_test`。
- [ ] 干净 minimal 构建与 `dev-fast` CTest 100% 通过。
- [ ] `./scripts/run_all_tests.sh --quick` 100% 通过。
- [ ] `./scripts/run_all_tests.sh --full` 全量 CTest 100% 通过。
- [ ] `./scripts/run_sanitizers.sh --fast` 100% 通过。
- [ ] `./scripts/format.sh --check`、`git diff --check`、LayerGuard 全部通过。
- [ ] 复验 Pipeline catalog/validate/plan 与 Demo smoke，确认生产运行时无变化。

## 6. 实施路线与里程碑 (Implementation Milestones)

1. [x] 创建隔离分支并记录基线数据与 RFC。
2. [ ] 固定第三方依赖、收敛 ccache launcher 选择并补充测试。
3. [ ] 收敛 quick/full/minimal 脚本语义并修复 dev target 依赖闭包。
4. [ ] 更新 README、CI、RFC 索引与 Changelog。
5. [ ] 完成独立 review、格式化、全量和 sanitizer 门禁。
6. [ ] 标记 RFC Completed，使用标准上传脚本创建/合并变更并验证远端 main。

## 7. 变更记录 (Changelog)

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-08-29 | v5.1.0 | 初始 RFC 与构建测试工作流收敛方案 | LLM-EdgeFlow Team |
