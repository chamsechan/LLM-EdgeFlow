# 当前分支相对 `main` 的修复后验收报告

## 1. 验收结论

**结论：通过。**

本轮已直接修复第三轮复评中剩余的 2 项 P1 和 3 项 P2，并完成定向测试、20 项 CTest 和项目六阶段全量回归。当前未发现阻塞合入的 P0/P1/P2 问题。

已确认：

- 纯 C ABI 对 Pipeline 文件打开、JSON 解析和配置语义失败均保持 `main` 的 `-3` 错误码。
- Destroy、Deinit 和 Create 回滚在外部 deallocator 抛标准或未知异常时仍会继续释放全部输出对象、Runtime 和句柄。
- Platform I/O Descriptor 会校验真实 BusinessAdapter、业务名、C DTO 类型、方向、suffix/alias 唯一性及第一阶段单输入/单输出组约束。
- 模型路径测试真实构造了多模型部分覆盖，并断言覆盖与未覆盖模型的最终绝对路径。
- `LLM_EDGEFLOW_USE_CCACHE=OFF` 会同步关闭 llama.cpp/ggml 的 `GGML_CCACHE`。

## 2. 评审范围与基线

| 项目 | 值 |
| --- | --- |
| 当前分支 | `docs/platform-operator-interface-design` |
| `main` | `a5fdf53` |
| 当前提交基线 | `278c157` |
| 本轮状态 | 基于 `278c157` 的未提交工作区修复 |
| 设计基线 | `f857662a:doc/platform_operator_interface_design.md` |
| 复验对象 | 第三轮报告中的 2 项 P1、3 项 P2及其回归影响 |

本轮除更新本文档外，实际修改了平台门面、共享 Runtime、Platform I/O Registry、第三方引擎 CMake 配置和平台 Operator 测试。

## 3. 问题关闭情况

| 原问题 | 状态 | 修复与验证 |
| --- | --- | --- |
| P1：C ABI 文件不存在时返回 `-2` | 已关闭 | `CreateFromConfigFile` 除 Registry 冲突外，将所有 `BuildFromConfigFile` 失败恢复为 `-3`；文件不存在、非法 JSON、未知节点测试均通过 |
| P1：deallocator 异常导致句柄失联和资源泄漏 | 已关闭 | 统一使用异常安全的输出池/句柄清理函数；逐项捕获回调异常并继续清理；Destroy、Deinit、多句柄、标准异常和未知异常测试均通过 |
| P2：Descriptor 不变量不完整 | 已关闭 | 新增无副作用的完整校验入口，并在实际注册前强制调用；删除可在运行期清空 Registry 的 `ResetForTesting()` |
| P2：模型路径测试未覆盖部分覆盖 | 已关闭 | 测试在独立临时目录生成双模型 Pipeline 和只覆盖一个模型的 `.conf`，直接断言两个最终路径 |
| P2：顶层 ccache 开关未控制 ggml | 已关闭 | 引入 llama.cpp 前同步设置 `GGML_CCACHE=OFF`；清除原缓存项后重新配置，确认自动为 OFF |

## 4. 关键实现说明

### 4.1 C ABI 错误码兼容

`SharedAlgorithmRuntime::CreateFromConfigFile` 继续允许 Registry 冲突返回 `-6`，其他所有 Pipeline 文件/解析/语义构建失败统一返回既有 C ABI 的 `COMPANY_ALG_ERR_INVALID_INPUT (-3)`。

平台 `.conf` 门面仍由 `CompanyConfResolver` 独立返回 `-2/-5`，因此没有破坏新平台接口的错误码设计。

### 4.2 释放回调异常安全

新增统一清理逻辑：

1. 逐个移动输出对象给 deallocator。
2. 单个回调抛异常时记录首个 `-99/-100`，继续处理剩余对象。
3. 无论回调结果如何，清空剩余 shared_ptr。
4. 使用 RAII 必然销毁 PlatformHandle 和其持有的 Runtime。
5. Deinit 先原子摘取全部活跃句柄，再逐个清理；单个句柄失败不会终止其他句柄清理。
6. Deinit 即使发现回调异常，也仍调用进程级 `GlobalDeinit()`。

测试同时统计 deallocator 调用次数和底层对象析构次数，证明不是仅调用回调，而是对象确实全部释放。

### 4.3 Descriptor 契约

`ValidateDescriptor` 现在要求：

- BizType 必须存在对应 BusinessAdapter；
- biz_name 必须与 Adapter 方法和 AdapterDescriptor 一致；
- 第一阶段必须恰好一个输入组、一个输出组；
- 输入/输出 `c_type_name` 必须与 AdapterDescriptor 的真实 DTO 类型一致；
- canonical suffix 非空、方向正确；
- canonical suffix 与 aliases 在同方向内唯一且 alias 非空。

实际 `RegisterDescriptor` 在写入 Registry 前强制执行该校验，失败会记录冲突并使 Init fail-closed。单测改用无副作用校验，不再依赖运行期重置全局 Registry。

### 4.4 模型路径与 ccache

- 模型路径测试从构建目录调用位于系统临时目录的 `.conf`，证明结果不依赖当前工作目录。
- `.conf` 只覆盖 `embed_model_v1`，未覆盖的 `llm_model_v1` 同样按 `.conf` 目录绝对化。
- CMake 仅传入 `-DLLM_EDGEFLOW_USE_CCACHE=OFF` 后，缓存中 `GGML_CCACHE:BOOL=OFF`，无需额外传入 `-DGGML_CCACHE=OFF`。

## 5. 测试记录

| 验证项 | 结果 | 说明 |
| --- | --- | --- |
| `./scripts/format.sh` | 通过 | Google C++ 格式完成 |
| Platform Operator 定向测试 | 通过 | 20/20，包含新增异常清理、C ABI 错误码、Descriptor 和路径测试 |
| `ctest --test-dir build --output-on-failure` | 通过 | 20/20 CTest，约 4.8 秒 |
| `./scripts/run_all_tests.sh` | 通过 | 六阶段 100% PASS，包含 LayerGuard、C11 ABI、Tier 1～4、7 业务 E2E 和双 CLI |
| ccache 独立关闭验证 | 通过 | 清除 `GGML_CCACHE` 缓存后，仅设置顶层 OFF，重新配置得到 `GGML_CCACHE:BOOL=OFF` |
| `git diff --check` | 通过 | 无空白错误 |

构建仍会输出已有的 FetchContent 弃用、OpenMP 未找到和部分未使用参数警告；本轮未发现它们影响构建、测试或平台 Operator 行为，故不作为验收阻塞项。

## 6. 最终意见

当前实现与 `f857662a` 设计中的 Layer 1 兼容门面、C ABI 兼容、路径确定性、命名 I/O 契约、生命周期所有权和异常屏障要求一致。第三轮报告中的全部遗留项均已关闭，可以进入提交和合入流程。
