# RFC 0032: 从 GitHub 发布包直接接入 kiteLLM

- **RFC 编号**：0032-kitellm-direct-github-dependency
- **创建日期**：2026-09-05
- **文档状态**：Completed
- **关联分支**：`feat/kitellm-direct-github`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow Team

## 1. 背景与动机

用户要求直接使用 chamsechan/kiteLLM 的 `include/kiteLLM.h` 和 `libkiteLLM.a`，
并从 GitHub 获取依赖，使新环境能够重建。RFC-0026 的外置 EdgeFlow C bridge
不是该项目的公开接口，本 RFC 替代其 kiteLLM 接入决策。

已通过当前环境的 GitHub 授权读取用户指定的私有仓库和 v0.1.0 发布包；该依赖是
用户指定的 GitHub 项目，不涉及 RFC-0029 延期的公司内部 SDK。

## 2. 设计范围与边界

- Layer 4 的 KiteLlmBackend 直接调用真实 C API；保持 text_generation 协议和条件注册。
- CMake 固定 GitHub release、平台资产名称和 SHA-256；使用 gh 的环境授权下载，
  缓存归入被 Git 忽略的 3rdparty，不提交第三方源码、头、库或凭据。
- 首期消费 Linux x86_64 / aarch64 发布包；其他平台配置时明确拒绝。
- 新环境先安装 gh 并授权仓库读取权限；CI 通过具有该仓库读取权限的 GH_TOKEN 注入。
  私有依赖无法通过代码改造变为匿名可访问。
- 不修改 kiteLLM 上游，不增加 Node、Model 或公共 C ABI。

## 3. 总体技术方案

固定 v0.1.0 发布资产的 SHA-256 是依赖身份；每次配置校验缓存归档并重新解包，
使用上游导出的 CMake target 传播静态库系统依赖，并执行实际 API 编译链接探测。
ENABLE_KITELLM 默认 OFF；开启后自动下载，无 KITELLM_ROOT 本机目录依赖。

会话以 RAII 持有 Init/DeInit 和模型句柄；模型、参数、任务输入及输出分别配对释放。
同步加载确保错误在 Load 阶段返回；同一会话互斥执行。已格式化 prompt 使用
Tokenizer_Encode（显式 add_bos、parse_special=1）及 SetPromptTokens，避免二次套用模板。
生成选项映射到真实 TaskInput setters。返回文本在首个 stop word 处截断；上游没有
stop-word setter，所以这不会提前终止上游同步推理。固定请求 seed 继续明确拒绝。

## 4. 关键设计考量与权衡

- 上游静态归档包含未改名的 llama/ggml 符号，且其 llama.cpp 版本与当前项目不同。
  因此首期 ENABLE_KITELLM 与 ENABLE_LLAMACPP 互斥，并在配置阶段给出明确错误；
  不能依赖静态库链接顺序混用两个版本。未来同时启用需要另行设计符号隔离。
- 头文件只出现在具体 Backend 的实现中，不进入框架公共头。
- 输入按显式长度分词；输出通过 tokens 与显式长度解码，保留嵌入 NUL 字节。
- 失败返回框架诊断；不推断上游没有公开的 last-error 接口。
- 发布包的平台 ABI 和系统依赖由编译链接探测检查；真实硬件性能验收不在本次范围。

## 5. 测试与质量验收计划

- [x] 现有 engine 测试覆盖条件注册、错误配置、资源生命周期、生成选项、stop words。
- [x] 实际 GitHub 下载、SHA 校验、缓存复用、损坏缓存拒绝、互斥配置拒绝。
- [x] kiteLLM 非默认构建、真实 GGUF 生成及 Catalog/公共 C ABI 业务 Pipeline smoke。
- [x] `./scripts/run_all_tests.sh` 默认完整质量门禁。

## 6. 实施路线与里程碑

1. [x] 基于 GitHub 真实源码/发布头核对接口并完成设计自评。
2. [x] 完成 CMake 依赖和 Backend 替换，补充现有测试。
3. [x] 完成专项验证和默认完整质量门禁。
4. [x] 更新状态及使用文档；按后续用户授权上传 PR 并补齐私有依赖 CI。

## 7. 变更记录

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-09-05 | v1.0.0 | GitHub 固定发布包、直接 C API 与静态依赖隔离决策 | LLM-EdgeFlow Team |
| 2026-09-05 | v1.1.0 | 用户授权设置 Actions secret 和 kiteLLM 独立 CI，验收记录纳入任务结果 | LLM-EdgeFlow Team |

## 8. 验收记录

- 从 GitHub 实际下载 x64 发布包核对摘要，并由 CMake 根据当前 Linux aarch64
  环境下载 arm 发布包，完成编译链接和共享库构建。x64 只完成资产摘要核验，未运行。
- `LlamaCppBackendTest` 在真实 Qwen GGUF 下验证了错误加载、采样选项、停止词、
  固定 seed 拒绝、并发串行化、多会话独立释放和重新初始化。
- Catalog/Validator/plan 验证实体抽取 DAG；公共 C ABI 使用 device_id=-1 完成
  Init/Create/Process/Destroy/DeInit，request_id=30001、status=0，返回非空结构化实体。
- 现有 Operator/alg_demo 强制指定设备及平台，不能满足 kiteLLM 由 run-config 独占
  设备选择的既有契约；本次不扩大到 Layer 1 契约变更。默认 Demo smoke 仍由门禁运行。
- 发布包缓存逐次校验 SHA；损坏缓存、旧 KITELLM_ROOT、错误平台与两个 llama
  实现同时启用的拒绝路径纳入现有 ThirdPartyCacheMetadataTest。

- 最终默认 `./scripts/run_all_tests.sh`：88/88 CTest 通过；kiteLLM + ONNX Runtime
  专项构建（启用真实 GGUF 用例）：88/88 CTest 通过。缓存复用配置未调用 GitHub 下载。

## 9. 远程 CI 扩展

用户后续授权配置跨私有仓库凭据及远程推理验证。将已有 gh 登录凭据通过 stdin 交给
`gh secret set`，加密存储为 `KITELLM_GITHUB_TOKEN`；凭据不写文件、日志或 Git。
该操作不改变原凭据权限和有效期，后续可轮换成仅有 kiteLLM Contents 读取权限的凭据。

独立 kite-llm job 仅在非 fork、非 Dependabot 事件运行。它通过 step 级 `GH_TOKEN`
执行固定发布包下载配置，然后在无该环境变量的步骤构建及运行真实 GGUF / 全部 CTest。
私有依赖及其链接产物不进入公开 cache/artifact；只复用公开模型缓存。
默认工作流三项门禁继续执行，最终验收记录增加 kiteLLM 状态；授权场景缺失/失效 secret
必须失败，外部 fork / Dependabot 的跳过状态明确记录。实际远端运行结果以 PR checks
及对应 commit 的 acceptance artifact 为准，不预先把配置存在当作下载或推理成功。
