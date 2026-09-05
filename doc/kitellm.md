# kiteLLM 直接接入

项目从私有 GitHub 仓库 `chamsechan/kiteLLM` 的固定 `v0.1.0` Release 下载平台包，
直接包含 `kiteLLM.h` 并链接 `libkiteLLM.a`。不再需要 `kitellm_edgeflow_adapter.h`
或另一个本地 kiteLLM 源码目录。

## 新环境构建

安装 GitHub CLI（`gh`）和项目构建工具，以及发布库需要的 OpenMP 运行时/开发包。
使用对该仓库有读取权限的账号登录：

```bash
gh auth login
gh repo view chamsechan/kiteLLM
cmake -S . -B build-kite -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_KITELLM=ON -DENABLE_LLAMACPP=OFF -DENABLE_ONNXRUNTIME=ON
cmake --build build-kite -j8
./build-kite/alg_pipeline_tool catalog
```

CI 使用仓库 Actions secret `KITELLM_GITHUB_TOKEN`，仅在配置下载步骤将其注入为
`GH_TOKEN`，无需交互登录。凭据必须能读取 `chamsechan/kiteLLM`；当前工程的默认
`GITHUB_TOKEN` 不能代替该跨私有仓库授权。不要把 token 放在 CMake 参数、URL 或仓库文件中。
新环境仍需网络和授权；GitHub 自动下载消除了对旧机器目录的依赖，不会绕过私有仓库权限。

轮换时在仓库 Settings → Secrets and variables → Actions 更新
`KITELLM_GITHUB_TOKEN`。新建凭据只需选择 kiteLLM 仓库并授予 Contents 读取权限。

`kiteLLM Private Release & Real GGUF` 独立任务在 Linux x64 上下载校验发布包，构建
kiteLLM + ONNX，确认 Catalog 注册并运行包含真实 GGUF 的完整 CTest。缺少或失效的
凭据会使任务明确失败，结果写入验收记录。外部 fork PR 和 Dependabot 跳过此私有任务，
其余默认 CI 保持执行；跳过状态在验收记录中保留。私有库、头文件和链接产物不上传为
公开 artifact，也不进入 Actions cache，只有公开 GGUF 模型使用缓存。

支持 Linux x86_64 与 aarch64；版本、平台包和 SHA-256 固定在
[`cmake/KiteLlm.cmake`](../cmake/KiteLlm.cmake)。上游 v0.1.0 对应提交
`5e58820f39919fce2046e2fd703c62601a5df59b`。修改发布包后即使 tag 不变，校验也会拒绝。

归档缓存位于 `3rdparty/kite_llm/v0.1.0/<x64|arm>/`，每次配置都校验归档 SHA-256，
再解包到当前 build 的 `_deps/kite_llm_release/`。已有完整缓存时不调用 gh、不访问网络。
缓存损坏会明确报错；删除报错指向的归档后重新配置即可下载。第三方文件均被 Git 忽略。
旧构建目录如设置过 `KITELLM_ROOT`，用 `cmake -UKITELLM_ROOT ...` 清除。

## 使用与限制

- kiteLLM 的静态归档内含 llama.cpp/ggml，版本与项目独立 llama_cpp 后端不同；
  两者在本次接入中互斥。ONNX Runtime 可同时启用。默认完整门禁仍验证默认后端组合，
  kiteLLM 使用独立构建目录专项验证。
- Catalog 的后端名仍为 `kite_llm`，支持 `text_generation` 协议；模型名、端口及
  参数应查询对应构建的 Catalog。关闭时不注册该后端。
- `model_path` 指向真实模型文件；可选 `backend_config.run_config_file` 是模型所在
  目录内的相对路径。设备选择和上游加载选项由该文件负责。
- 直接使用公共 C ABI 时，以 `CompanyAlgParamCreate.device_id=-1` 表示不另指定设备。
  现有 Operator / alg_demo 强制指定设备和平台，尚不能运行 kiteLLM 配置；本次保持该
  既有契约，真实业务链路通过公共 C ABI 验证，默认 Demo smoke 仍由完整门禁验证。
- Backend 接收 Model 已格式化的 prompt，通过 tokenizer 与 token 输入保留 BOS 和
  特殊 token 语义。任务输入、输出、参数、模型和全局初始化均配对释放。
- 每请求固定 seed 仍不支持，会明确报错；需要时可在上游 run-config 中配置其 seed。
- `stop_words` 在返回结果中截断；上游 C API 没有对应 setter，因此不提前停止同步推理。

## 验证

```bash
LLM_EDGEFLOW_TEST_KITELLM_MODEL=/absolute/path/model.gguf \
  ctest --test-dir build-kite -R '^LlamaCppBackendTest$' --output-on-failure
ctest --test-dir build-kite --output-on-failure -j8
```

沿用现有 engine 测试套件名称。未设置真实模型路径时，真实模型用例会明确跳过。
该套件检查错误加载、配置路径、生成参数、固定 seed 拒绝、stop words、并发串行化和
多会话生命周期。默认完整交付门禁仍为 `./scripts/run_all_tests.sh`。

设计与验收记录见 [RFC-0032](rfcs/0032-kitellm-direct-github-dependency.md)。
