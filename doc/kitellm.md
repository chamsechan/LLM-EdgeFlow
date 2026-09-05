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
- Catalog 的后端名仍为 `kite_llm`，支持 `text_generation`、`image_text_generation` 和 `generated_token_embedding` 协议；模型名、端口及
  参数应查询对应构建的 Catalog。关闭时不注册该后端。
- `model_path` 指向真实模型文件；可选 `backend_config.run_config_file` 是模型所在
  目录内的相对路径，由上游解析运行选项。设备 ID 通过原生
  `kiteLLM_Parameter_SetDeviceId` 传入，并非只能通过 run-config 选择。
- 当前固定 Linux 发布包按 CPU 接入。Operator / alg_demo 使用既有 `CPU` / `cpu_generic`
  平台与 `device_id=0` 即可运行正确配置的 Kite 文本生成业务；无需修改 Operator 参数。
  CUDA、NPU 等显式平台，以及 CPU 平台下大于 0 的设备 ID，均明确拒绝。
- 公共 C ABI 的 `CompanyAlgParamCreate.device_id=-1` 仍表示不另指定设备；Backend
  收到显式 -1 时映射到原生自动选择。原生非负 ID 是其 ggml 设备枚举索引，不保证等同于
  CUDA ordinal。小于 -1 拒绝，其余索引由原生加载检查。
- 显式 CPU 与 run-config 中 `model.gpu_layers > 0` 冲突时明确报错；其他上游字段和
  格式仍由 Kite 校验。本项目不添加平台 setter 或静默忽略平台要求。
- Backend 接收 Model 已格式化的 prompt，通过 tokenizer 与 token 输入保留 BOS 和
  特殊 token 语义。任务输入、输出、参数、模型和全局初始化均配对释放。
- 每请求固定 seed 仍不支持，会明确报错；需要时可在上游 run-config 中配置其 seed。
- `stop_words` 在返回结果中截断；上游 C API 没有对应 setter，因此不提前停止同步推理。

## 验证

```bash
LLM_EDGEFLOW_TEST_KITELLM_MODEL=/absolute/path/model.gguf \
  ctest --test-dir build-kite -R '^(LlamaCppBackendTest|DemoRunnerTest)$' --output-on-failure
ctest --test-dir build-kite --output-on-failure -j8
```

沿用现有 engine 测试套件名称。未设置真实模型路径时，真实模型用例会明确跳过。
该套件检查错误加载、配置路径、生成参数、固定 seed 拒绝、stop words、并发串行化和
多会话生命周期，以及原生显式设备选择、平台/运行配置冲突。DemoRunnerTest 在提供真实
模型时复用实体抽取 DAG，通过既有 Demo → Operator 路径验证非空结构化结果；解析失败
直接报错，不使用 JSON fallback。默认完整交付门禁仍为 `./scripts/run_all_tests.sh`。

已有实体抽取、文档问答等配置中的 `llama_cpp` 不会自动变成 Kite。切换时应使用
`qwen_causal_lm` + `kite_llm`，将原来的 llama.cpp `backend_config` 替换为 `{}` 或
`{"run_config_file":"run.json"}`，并保持真实模型路径正确。`random_seed` 使用 -1。
既有文档问答配置的 Embedding/Rerank 使用 ONNX；新增生成向量配置可将 Embedding 交给 Kite；图像文档识别通过下述视觉协议接入。ASR 仍未接入真实模型。

依赖设计见 [RFC-0032](rfcs/0032-kitellm-direct-github-dependency.md)；设备契约的修正与
验收见 [RFC-0033](rfcs/0033-kitellm-native-device-contract.md)。RFC-0026/0032 中的
run-config 独占设备约定是历史本地政策，已由 RFC-0033 替代。

## Kite 部署示例套件

```bash
./scripts/fetch_real_test_models.sh --kite
./build-kite/alg_demo --profiles-file demo/profiles_kite.json --suite real
# 单项运行：
./build-kite/alg_demo --profiles-file demo/profiles_kite.json --profile ocr_doc_qa_kite
```

`--profiles-file` 是通用部署配置入口；省略时保持原有 demo/profiles.json 和默认套件。
新套件包含实体抽取、问答、带精排问答、审核、图像文档问答，以及复用的关键词匹配与
ONNX 精排，以及 `doc_qa_kite_embeddings` 纯 Kite 问答。纯文本 LLM 使用 Kite；
原有混合配置的 Embedding/Rerank 使用 ONNX。所有新增结构化
解析配置均使用 fail 策略，不通过 JSON fallback 掩盖模型生成错误。

### 生成模型向量与纯 Kite 问答

Kite v0.1.0 的 `SetVerboseDataFlag(KLLM_VERBOSE_F_OUTPUT_EMBEDDING)` 与
`GetOutputEmbedding` 有实际实现，Backend 通过中性 `generated_token_embedding`
协议接入。它返回 **decode 生成 token 后** 的隐藏向量，并不返回输入全部 token 的
encoder 向量，也不是输入最后一个 token 的 prefill 向量。

`generated_text_embedding` 实现现有 `IEmbeddingModel`，由 Model 负责 prompt
prefix/suffix、last/mean 池化和请求级 L2 归一化。Backend 负责 greedy 生成、复制原生
输出及会话串行化，不承担池化或检索语义。模型配置必须指定真实 `embedding_dim`；
`max_tokens` 为 1..64（默认 1），`pooling` 可选 last（默认）/mean，`add_bos` 默认 false。
只池化实际生成的 token；首步 EOS 没有向量时明确失败，不伪造或补齐向量。

```bash
./build-kite/alg_pipeline_tool catalog
./build-kite/alg_pipeline_tool validate configs/kite/pipeline_doc_qa_embeddings.json
./build-kite/alg_pipeline_tool plan configs/kite/pipeline_doc_qa_embeddings.json
./build-kite/alg_demo --profiles-file demo/profiles_kite.json --profile doc_qa_kite_embeddings
```

该示例复用既有问答 DAG，向量与回答生成都用 Qwen2.5-0.5B GGUF + Kite，向量维度 896。
这里的向量属于生成模型特征，Catalog 明确标注为 experimental；跑通链路不代表其
检索质量等同于 BGE。原有 `doc_qa_kite` 继续使用 BGE/ONNX。更换模型、prompt、
池化方式或生成上限会改变向量空间，已有索引需要重建，检索阈值与质量需要重新评估。

现有 BGE ONNX 模型仍要求 `tensor_graph`，不能仅把 backend 名改为 Kite。后续如果
Kite 提供 encoder/prefill 向量能力，应在 Layer 4 增加符合实际输出语义的协议/Model，
保持 Node、Pipeline 和 Operator 使用能力接口。见 [RFC-0035](rfcs/0035-generated-token-embedding.md)。

### 图像文档识别

`vision_document` 实现现有 IOcrModel，要求 image_text_generation Backend 协议。
它解码 PNG/JPEG/二进制 PNM，转换 RGB 平面并按 patch_size 补白边；不裁剪内容。
输入文件上限 32 MiB，像素上限由 max_pixels 控制（默认 4194304，包含补边）。
模型 patch_size 必须与视觉投影文件匹配；随附 SmolVLM 模型使用 16。

Kite 通过原生 SetMultiModal_ChatHistory 编码图像并生成文字。run-config 必须包含
vision.mmproj，路径相对运行 JSON 所在目录，只允许其目录内的文件，不允许绝对路径
或父目录逃逸。普通 Qwen 文本 GGUF 不能替代视觉模型；必须使用匹配的 GGUF 与 mmproj。

该识别方式仅返回 combined_text；boxes 留空，Operator 检测框数量为 0。它没有真实
检测框/置信度输出，不能用于要求精确坐标的 OCR 业务。示例小模型用于功能验证，
票据转写与字段准确率需用目标数据评估。原生当前实现不支持音频输入、视频或预计算
视觉特征；不因为接口头声明或底层编译了相关模型类就宣称这些能力可用。

### 真实能力门禁

```bash
LLM_EDGEFLOW_TEST_KITELLM_MODEL="$PWD/models/qwen2.5-0.5b-instruct-q4_k_m.gguf" \
LLM_EDGEFLOW_TEST_KITELLM_VISION_MODEL="$PWD/models/SmolVLM-256M-Instruct-Q8_0.gguf" \
LLM_EDGEFLOW_TEST_KITELLM_VISION_CONFIG=kite_vision_run.json \
LLM_EDGEFLOW_TEST_KITELLM_DEMOS=1 \
  ctest --test-dir build-kite --output-on-failure -j4
```

视觉测试验证真实红色图像的推理结果、错误图像和投影配置；模型单测验证补边、RGB
排列、尺寸限制、请求溯源和失败回滚。Demo 测试运行上述独立部署套件。未设置真实
模型环境变量的跳过不算真实能力验收。设计与验收见
[RFC-0034](rfcs/0034-kitellm-capability-coverage.md)。
