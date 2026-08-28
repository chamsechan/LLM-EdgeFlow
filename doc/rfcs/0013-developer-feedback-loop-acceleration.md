# RFC 0013: 开发反馈闭环加速

- **RFC 编号**：0013-developer-feedback-loop-acceleration
- **创建日期**：2026-08-28
- **文档状态**：Completed
- **关联分支**：`feat/developer-feedback-loop-acceleration`
- **目标版本**：v4.2.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机 (Motivation & Context)

当前开发闭环同时承担全量构建、Google Test、C ABI 安全检查、多业务 Demo、CLI 与架构文档门禁。基线测量显示，冷构建主要受大型 C++ 翻译单元、重复解析 GTest/nlohmann JSON 头文件及 llama.cpp 三方源码影响；增量自验则存在测试目标重复链接、六阶段串行调度和 CI 重复执行完整测试的问题。

本 RFC 在不降低覆盖率、不改变生产 ABI 和运行时语义的前提下，引入快速构建配置、可回退高速链接器、测试 Runner 分片与预编译头、CTest 标签化统一调度以及无重复的 CI/PR 门禁，使开发过程中可以快速收敛，并在交付前继续执行完整质量门禁。

---

## 2. 设计范围与边界 (Scope & Non-Goals)

### 2.1 范围内 (In-Scope)

- [x] Fast/Sanitizer 使用 `-O1`，Release 保留标准优化等级。
- [x] 自动探测 mold/lld，显式选择失败时 fail-closed，自动模式允许系统链接器回退。
- [x] 将兼容 GTest 分片并共享预编译头，同时保留 CTest 名称、过滤器和特殊进程隔离。
- [x] 为测试增加阶段、速度和 Sanitizer 兼容性标签，统一并行调度。
- [x] 消除本地六阶段、CI 与上传流程中的重复测试执行。
- [x] 提供只创建 PR、不自动合并的标准上传模式。

### 2.2 非目标 (Non-Goals / Out-of-Scope)

- 不改变六阶段全量质量门禁的覆盖范围。
- 不修改 Layer 1 C ABI、Layer 2 Pipeline、Layer 3 Node 或 Layer 4 Engine 的业务行为。
- 不以 Fast 模式替代交付前的全量、Sanitizer 或真实模型验证。
- 不引入 Unity Build，避免测试宏、匿名状态和静态注册产生新的 ODR 风险。

---

## 3. 总体技术方案与架构设计 (Architecture & Technical Design)

### 3.1 架构分层映射 (4-Tier Mapping)

- **Layer 1 (C ABI / Platform Adapter)**：无接口或实现变化，仅继续执行 ABI 契约测试。
- **Layer 2 (Pipeline & Blackboard)**：无运行时变化，相关测试进入 core Runner。
- **Layer 3 (Business & Common Nodes)**：无运行时变化，相关测试进入 nodes Runner。
- **Layer 4 (Engines & Hardware Acceleration)**：无运行时变化；真实 llama/模型测试保持独立慢测试。
- **Tooling / Test Infrastructure**：本 RFC 的全部实现归属。

### 3.2 核心接口与数据流设计 (Interface & Data Flow)

```text
CMake configure
  ├─ LLM_EDGEFLOW_FAST_BUILD=ON -> -O1 + debug-friendly frames
  ├─ ENABLE_SANITIZERS=ON       -> -O1 + sanitizer frames
  └─ LLM_EDGEFLOW_LINKER        -> mold | lld | system

run_all_tests.sh --fast
  -> build-fast -> dev-fast CTest labels -> mock smoke

run_all_tests.sh --full (default)
  -> build -> one global parallel CTest pool
     -> tier1 core/nodes
     -> tier2 ABI/operator
     -> tier3 demo/integration
     -> tier4 CLI/tooling
```

### 3.3 测试 Runner 隔离

兼容测试按 core、nodes、adapter/operator、tooling 四类分片，每个分片共享 PCH。每个原 CTest 条目仍以独立进程运行，并用 `--gtest_filter` 选择对应源文件的测试套件。C11、Registry 冲突、Qwen/llama 和真实模型测试继续使用独立可执行文件。

---

## 4. 关键设计考量与权衡 (Design Trade-offs & Invariants)

1. **覆盖率不变**：Fast 模式只用于迭代；默认命令始终执行全量门禁。
2. **进程隔离不变**：会污染全局 Registry 的冲突测试不得并入共享进程执行。
3. **Release 语义不变**：不使用全局优化参数覆盖 CMake Release flags。
4. **可移植回退**：`auto` 找不到高速链接器时回退系统链接器；显式选择不可用必须配置失败。
5. **可诊断性**：Sanitizer 保留 frame pointer 并关闭 sibling-call optimization。
6. **向后兼容**：无参数测试脚本仍代表 full；上传脚本默认仍保持原自动合并语义，`--pr-only` 为显式新增模式。

---

## 5. 测试与质量验收计划 (Testing & Verification Plan)

- [x] CMake 配置测试：Fast、Release、Sanitizer、system、auto，以及显式
      mold/lld 缺失时的 fail-closed 行为。
- [x] 测试清单回归：77 个 CTest 条目通过，特殊 Registry/C11/外部引擎
      测试仍独立运行。
- [x] `./scripts/run_all_tests.sh --fast` 通过：74/74，热运行总耗时 4 秒，
      CTest 墙钟 1.34 秒。
- [x] `./scripts/run_all_tests.sh --full` 六阶段全量通过：77/77，统一 CTest
      调度墙钟 29.86 秒。
- [x] `./scripts/run_sanitizers.sh --fast` 通过：ASan+UBSan 74/74，CTest
      墙钟 12.27 秒。
- [x] `./scripts/format.sh --check`、LayerGuard 和文档漂移门禁通过。
- [x] 冷/热路径复测：首次 Fast 构建与测试为 183 秒；无源码变化的日常
      Fast 门禁为 4 秒。冷路径的主要剩余成本是 132 个首次编译对象。
- [ ] GitHub CI 使用 mold 执行唯一一次完整门禁并通过（PR 创建后验证）。

---

## 6. 实施路线与里程碑 (Implementation Milestones)

1. [x] 创建特性分支并冻结基线证据。
2. [x] 完成 CMake 优化等级与链接器选择。
3. [x] 完成 Runner/PCH、CTest 标签和脚本统一调度。
4. [x] 完成 CI、README 与 PR-only 上传流程。
5. [x] 完成本地全量回归与性能复测；PR 由标准上传流程创建。

---

## 7. 变更记录 (Changelog)

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-08-28 | v4.2.0 | 初始 RFC 与开发反馈闭环加速方案 | LLM-EdgeFlow Team |
| 2026-08-28 | v4.2.0 | 完成实现、本地全量与 ASan+UBSan 验证 | LLM-EdgeFlow Team |
