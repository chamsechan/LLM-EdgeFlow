# LLM-EdgeFlow 框架全面审查与问题收敛报告

- **原始远程分支**：`origin/feat/framework-audit-and-review@4213cbb`
- **Rebase 目标**：`main@ac6ea4716a7ee0d3056e3acae617fbc23613bb6e`
- **整改代码提交**：`a2631ef3ba7f0785e56acbad996965bcfad4f478`
- **复验日期**：2026-08-23
- **分支合并门禁**：**PASS**
- **框架全面认证结论**：**CONDITIONAL PASS**

---

## 1. Executive Summary

原始远程实现不应直接合并。它的报告基线未覆盖实际整改提交，包含过时测试数量、不存在的 `-Werror` 声明、错误的节点数量、不可移植绝对路径，并将未验证项直接归入无条件 PASS。

分支已 rebase 到 v2.5.0 图形化工作台合并后的最新 `main`，并完成以下收敛：

- 格式门禁改为只读 `clang-format --dry-run --Werror`，CI 不再“先改文件、再只查空白错误”。
- Sanitizer 改用独立 `build-sanitizers/`，关闭用户级 ccache，动态运行所有注册 CTest 与 Smoke Profile。
- Sanitizer 集合可显式选择，默认仍为 `address,undefined`，不支持的值在 CMake 配置阶段拒绝。
- `CompanyAlgBizType` 使用 `INT32_MAX` ABI guard，纯 C11 测试以 `_Static_assert` 锁定 32 位布局。
- 撤销远程分支中由不同 clang-format 版本造成的无关对齐噪声。

未发现 P0/P1 问题。常规合并门禁已全部通过；但 ASan、LSan、TSan、真实硬件/大模型、性能基线和同 SHA 远程 CI 尚未在本次环境全部完成，因此不得将本报告解读为“全平台、100% 内存安全”证明。

## 2. 基线与环境

| 项目 | 复验值 |
| --- | --- |
| 主分支基线 | `ac6ea4716a7ee0d3056e3acae617fbc23613bb6e` |
| 整改代码 | `a2631ef3ba7f0785e56acbad996965bcfad4f478` |
| 工作树 | 整改代码提交后仅保留本报告、审查计划与 Changelog 收尾改动 |
| 操作系统 | macOS 26.6.2 (Build 25G83), arm64 |
| C/C++ 编译器 | AppleClang 16.0.0 (`clang-1600.0.26.3`) |
| 构建工具 | CMake 4.4.2, GNU Make 3.81 |
| 默认测试资产 | 23 项 CTest，9 个 Smoke Profile，11 对 `.conf/.json` |
| 节点快照 | `src/business/` + `src/common_nodes/` 共 27 个 `INode` 实现 |

## 3. 发现与整改

| 编号 | 等级 | 原始问题 | 整改 | 状态 |
| --- | :---: | --- | --- | :---: |
| AUD-001 | P2 | 报告基线指向 `25eb0f0`，但实际整改在 `4213cbb`，证据与结论未绑定同一 SHA | 记录原始分支、rebase 目标与整改提交 | CLOSED |
| AUD-002 | P2 | CI 和六阶段脚本先改写源码，再执行 `git diff --check`，无法拒绝纯格式差异 | 新增 `format.sh --check`，CI/回归脚本使用只读检查 | CLOSED |
| AUD-003 | P2 | Sanitizer 重用普通 `build/`、硬编码 21 项测试，且依赖用户级 ccache | 独立构建目录、动态 CTest、禁用 ccache、可配置 sanitizer 集合 | CLOSED |
| AUD-004 | P2 | C ABI 枚举 guard 没有编译期布局证据 | 增加纯 C11 32 位 ABI 断言 | CLOSED |
| AUD-005 | P2 | 报告声称 `-Werror`、18 个节点、21 项当前测试和无条件 ASan PASS，与代码/复验不符 | 按最新主干资产与实际输出重写报告 | CLOSED |
| AUD-006 | P3 | 报告使用 `file:///home/ubuntu/...` 绝对链接 | 改为仓库相对链接 | CLOSED |

## 4. 静态审查结论

### 4.1 Layer 1: C ABI / Platform Operator

- [`company_alg_interface.h`](../../../include/company_alg_interface.h) 保持纯 C11，公共结构未引入 STL 或第三方类型。
- 6 个 C ABI 函数仍由 [`company_c_adapter.cpp`](../../../src/adapter/company_c_adapter.cpp) 提供 `noexcept` 和顶层异常边界。
- `ALG_BIZ_TYPE_MAX_GUARD = INT32_MAX` 用于锁定枚举 ABI 宽度并使正整数非法值探针在 UBSan enum 可表示范围内；这不意味着 guard 值是合法业务类型。
- C11 测试新增 `sizeof(CompanyAlgBizType) == sizeof(int32_t)` 与 guard 值断言。

### 4.2 Layer 2 ~ Layer 4

- Pipeline Build、Catalog/Validator、DAG 与 Blackboard 的结论以当前 23 项 CTest 的覆盖边界为限，不外推为无竞态或无泄漏证明。
- 当前快照包含 27 个 `INode` 实现；`KeywordMatcherNode`、`IntentRuleNode` 等持有节点级配置状态，但未发现将单次请求数据作为跨请求状态保留的证据。
- FixedBatchExecutor 的空输入、整批、非整除、补齐剔除和底层失败传播由现有单元测试覆盖；未对真实硬件 DMA 时序做结论。

## 5. 动态复验

| 命令 | 结果 | 覆盖边界 |
| --- | --- | --- |
| `./scripts/format.sh --check` | PASS | 当前受管 C/C++ 文件符合本机 `.clang-format` |
| `cmake -S . -B build && cmake --build build -j4` | PASS | macOS arm64 默认构建 |
| `ctest --test-dir build --output-on-failure` | PASS, 23/23 | 默认注册测试；耗时 13.43s |
| `./scripts/run_all_tests.sh` | PASS | 6 阶段，含 LayerGuard、C ABI、并发、Demo、CLI 和 Studio |
| `LLM_EDGEFLOW_SANITIZERS=undefined ./scripts/run_sanitizers.sh` | PASS | UBSan 独立构建；23/23 CTest，9 个 Smoke Profile |
| `./scripts/run_sanitizers.sh` | NOT VERIFIED | 当前 AppleClang 16 ASan runtime 在 macOS 26.6.2 初始化阶段断言；最小 C 探针同样复现 |

ASan 失败发生于任何项目代码执行之前，错误为：

```text
AddressSanitizer: CHECK failed: sanitizer_malloc_mac.inc:189
"((!asan_init_is_running)) != (0)"
```

因最小空项目探针亦失败，本报告将其记为工具链/操作系统不兼容，而不是业务测试 FAIL。必须在匹配的 Xcode/LLVM 或 Linux CI 上重跑默认 `address,undefined` 后，才能将 ASan 状态更新为 PASS。

## 6. 未验证项

| 项目 | 状态 | 复验条件 |
| --- | :---: | --- |
| ASan | NOT VERIFIED | 匹配 macOS 的新版 Xcode/LLVM，或 Linux Clang/GCC |
| LeakSanitizer | NOT VERIFIED | 在支持 LSan 的平台上启用 `detect_leaks=1` |
| TSan | NOT VERIFIED | 独立 TSan 构建与并发/热更新矩阵 |
| 真实 NPU / DMA | NOT VERIFIED | 专有硬件、驱动和可观测数据 |
| 真实大模型质量 | NOT VERIFIED | 完整权重、真实 Profile 和结果断言 |
| 性能 P50/P95/P99 | NOT VERIFIED | 冻结硬件、输入和编译选项后建立基线 |
| 远程 CI | NOT VERIFIED | 推送后核对同一最终 SHA 的 Actions 结果 |

## 7. 合并与后续判定

本分支的代码、脚本和文档整改满足仓库合并门禁：无 P0/P1，已发现 P2 均有修复与本机复验证据，可合入 `main`。

“框架全面认证”仍为 `CONDITIONAL PASS`。在第 6 节未验证项关闭前，禁止声称“全平台通过”、“100% 内存安全”、“零泄漏”或“真实硬件已验证”。
