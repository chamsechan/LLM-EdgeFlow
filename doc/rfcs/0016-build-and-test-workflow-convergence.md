# RFC 0016: 构建与测试工作流收敛

- **RFC 编号**：0016-build-and-test-workflow-convergence
- **创建日期**：2026-08-29
- **文档状态**：Completed
- **关联分支**：`feat/build-test-workflow-optimization`、`refactor/build-workflow-simplification`
- **目标版本**：v5.1.0

## 1. 背景与动机

RFC-0015 引入 ONNX Runtime 与 llama.cpp 后，旧 `--fast` 同时改变构建类型、
后端能力、构建目录和测试范围，形成了第二套日常构建语义。实测结果如下：

| 场景 | 旧 fast | full |
| :--- | ---: | ---: |
| 干净配置 | 2.11s | 3.13s |
| 干净构建 | 47.53s | 64.80s |
| 增量配置 + 无变化构建 | 1.58s | 1.85s |
| 标签测试 | 0.77s / 80 项 | 0.84s / 82 项 |

17 秒的首次构建收益不足以抵消双构建树和双配置语义的维护成本。同时发现：

1. 干净 `edgeflow_dev_tests` 缺少 `alg_pipeline_tool_test`，测试目标闭包不完整；
2. 第三方归档缺少 SHA256，llama.cpp 跟踪浮动分支；
3. 项目显式设置 ccache launcher，与环境 compiler wrapper 职责重叠。

## 2. 范围与非目标

### 2.1 范围内

- `run_all_tests.sh` 只保留一个完整后端、完整 CTest 门禁。
- 修复 `edgeflow_dev_tests` 的目标依赖闭包。
- 为默认测试设置有限超时。
- 固定 FetchContent 版本和 SHA256，并使用 `FetchContent_MakeAvailable`。
- ccache 由 compiler wrapper 或标准 CMake launcher 管理。

### 2.2 非目标

- 不修改 C ABI、Pipeline、Node 或 Engine 运行时行为。
- 不在项目中管理 ccache 安装位置、缓存目录或 CI 缓存实现。
- 不为构建脚本增加通过 grep 检查源码文本的契约测试。
- 不维护 quick/minimal 等收益有限的第二套门禁。

## 3. 架构与数据流

### 3.1 四层映射

- **Layer 1**：无生产变更，继续执行 C11 与异常安全测试。
- **Layer 2**：无生产变更，继续执行 Pipeline/Validator 测试。
- **Layer 3**：无生产变更，继续执行 Node 与 Demo smoke。
- **Layer 4**：不改变后端接口，仅固定第三方源码版本。
- **Tooling**：本 RFC 的实现归属。

### 3.2 唯一门禁

```text
run_all_tests.sh
  -> build/ (Release, ONNX Runtime ON, llama.cpp ON)
  -> build all targets
  -> ctest all labels
```

可选后端或特殊 sanitizer 组合仍可直接通过 CMake 选项验证，不进入主测试脚本。

### 3.3 ccache 边界

项目不调用 `find_program(ccache)`，也不设置 `CCACHE_DIR`：

- compiler 已是 ccache wrapper 时，编译自然经过 ccache；
- CI 或其他环境可设置 `CMAKE_C_COMPILER_LAUNCHER` 和
  `CMAKE_CXX_COMPILER_LAUNCHER`；
- ggml 自身的二次 ccache 探测关闭，避免重复策略。

## 4. 设计不变量

1. 只有一个交付门禁和一个常用构建目录。
2. 门禁始终构建完整后端并运行全部 CTest。
3. 测试引用的二进制必须属于相应构建目标依赖闭包。
4. 所有远端归档声明固定版本与 SHA256。
5. ccache 与依赖下载缓存由环境负责，项目只描述标准接入点。
6. 哈希不匹配、测试失败或测试超时必须使门禁失败。

## 5. 验收结果

- [x] 干净最小后端构建验证通过（82/82，方案收敛前基线）。
- [x] 完整后端构建与全部 CTest 通过。
- [x] `edgeflow_dev_tests` 包含 `alg_pipeline_tool_test`。
- [x] nlohmann/json、GoogleTest、ONNX Runtime、llama.cpp 均固定 SHA256。
- [x] UBSan 82/82 通过。
- [x] Pipeline catalog/validate/plan 与 Demo smoke 通过。
- [x] 格式化、LayerGuard、`git diff --check` 通过。

本机 macOS 26.6.2 搭配 Apple Clang 16/macOS 15 SDK 时，独立最小 ASan 探针
在进入 `main()` 前触发 `asan_init_is_running`。该问题属于宿主工具链兼容性，
不作为项目代码失败记录。

## 6. 实施结果

1. [x] 记录 fast/full 基线并创建 RFC。
2. [x] 修复测试目标闭包和默认超时。
3. [x] 固定第三方依赖版本与哈希。
4. [x] 将测试入口收敛为单一完整门禁。
5. [x] 删除 ccache 检测封装、源码文本契约测试和内部 FetchContent 缓存策略。
6. [x] 更新 README、CI 与 Changelog。

## 7. 变更记录

| 日期 | 版本 | 变更 |
| :--- | :--- | :--- |
| 2026-08-29 | v5.1.0 | 完成构建与测试工作流收敛 |
| 2026-08-29 | v5.1.0 | 精简为单一门禁和环境自治的 ccache 策略 |
