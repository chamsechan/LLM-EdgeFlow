# kiteLLM 直接接入

项目从私有 GitHub 仓库 `chamsechan/kiteLLM` 的固定 `v0.1.0` Release 下载平台包，
直接包含 `kiteLLM.h` 并链接 `libkiteLLM.a`。

## 新环境构建

准备 GitHub CLI（`gh`）、项目构建工具和 OpenMP 运行时/开发包。
以下命令依次登录、检查仓库权限、构建和查询能力：

```bash
gh auth login
gh repo view chamsechan/kiteLLM
cmake --preset=kite-cpu
cmake --build build/variants/kite-cpu -j8
./build/variants/kite-cpu/alg_pipeline_tool catalog
```

CI 使用仓库 Actions secret `KITELLM_GITHUB_TOKEN`，仅在配置下载步骤将其注入为
`GH_TOKEN`，无需交互登录。凭据必须能读取 `chamsechan/kiteLLM`。当前工程的默认
`GITHUB_TOKEN` 不能代替该跨私有仓库授权。不要把 token 放在 CMake 参数、URL 或仓库文件中。
新环境需要网络和对应私有仓库的读取权限。

轮换时在仓库 Settings → Secrets and variables → Actions 更新
`KITELLM_GITHUB_TOKEN`。新建凭据只需选择 kiteLLM 仓库并授予 Contents 读取权限。

`kiteLLM Private Release & Real GGUF` 独立任务在 Linux x64 上下载校验发布包，构建
kiteLLM + ONNX，确认 Catalog 注册并运行带 `kite` 标签的 CTest。随后通过 Demo 分别执行
真实文本、生成向量问答与视觉方案。缺少或失效的凭据会使任务明确失败，结果写入验收记录。
外部 fork PR 和 Dependabot 跳过此私有任务，
其余默认 CI 保持执行。跳过状态在验收记录中保留。私有库、头文件和链接产物不上传为
公开 artifact，也不进入 Actions cache。模型缓存包含公开 GGUF、ONNX 权重和 tokenizer。

支持 Linux x86_64 与 aarch64。版本、平台包和 SHA-256 固定在
[`cmake_ext/company/KiteLlm.cmake`](../cmake_ext/company/KiteLlm.cmake)。上游 v0.1.0 对应提交
`5e58820f39919fce2046e2fd703c62601a5df59b`。修改发布包后即使 tag 不变，校验也会拒绝。

归档缓存位于 `3rdparty/kite_llm/v0.1.0/<x64|arm>/`，每次配置都校验归档 SHA-256，
再解包到当前 build 的 `_deps/kite_llm_release/`。已有完整缓存时不调用 gh、不访问网络。
如果缓存损坏，工具会报错。删除报错指定的归档，再重新配置。
工具会重新下载归档。Git 忽略这些第三方文件。

## 使用与限制

- kiteLLM 的静态归档内含 llama.cpp/ggml，版本与项目独立 llama_cpp 后端不同。
  两者互斥，依赖共享 llama.cpp/ggml 的 whisper.cpp 也不能同时启用。ONNX Runtime
  可同时启用。默认完整门禁仍验证默认后端组合，kiteLLM 使用独立构建目录专项验证。
- Catalog 的后端名为 `kite_llm`，支持 `text_generation`、`image_text_generation` 和 `generated_token_embedding` 协议。模型名、端口及
  参数应查询对应构建的 Catalog。关闭时不注册该后端。
- 模型条目 `file` 指向权重。可选 `backend.params.run_config_file` 与权重一样相对 Pipeline
  JSON 所在目录解析，必须是非空相对文件名。由上游读取运行选项。设备 ID 通过原生
  `kiteLLM_Parameter_SetDeviceId` 传入，并非只能通过 run-config 选择。
- 当前固定 Linux 发布包按 CPU 接入。Operator 使用 `ComputePlatform::kCpu`，
  alg_demo Profile 使用 `"chip": "cpu"`，并设置 `device_id=0`，即可运行正确配置的 Kite 文本生成业务。
  CUDA、NPU 等显式平台，以及 CPU 平台下大于 0 的设备 ID，均明确拒绝。
- Backend 收到显式 `device_id=-1` 时映射到原生自动选择。原生非负 ID 是其 ggml 设备枚举索引，不保证等同于
  CUDA ordinal。小于 -1 拒绝，其余索引由原生加载检查。
- 显式 CPU 与 run-config 中 `model.gpu_layers > 0` 冲突时明确报错。其他上游字段和
  格式仍由 Kite 校验。本项目不添加平台 setter 或静默忽略平台要求。
- Backend 接收 Model 已格式化的 prompt，通过 tokenizer 与 token 输入保留 BOS 和
  特殊 token 语义。任务输入、输出、参数、模型和全局初始化均配对释放。
- 固定随机种子不受支持，节点显式配置时在 Create 报错。需要时可在上游 run-config 中配置其 seed。
- `stop_words` 在返回结果中截断。上游 C API 没有对应 setter，因此不提前停止同步推理。

## 验证

```bash
(cd build/variants/kite-cpu && ctest -L kite --output-on-failure --no-tests=error)
```

该标签包含 Backend 参数与协议拒绝、Catalog 注册和共享 Demo 调用测试。不证明真实模型推理效果。
CTest 按 `Suite.Case` 发现用例，`--no-tests=error` 避免筛选为空时误报成功。
真实模型执行使用下文的[真实能力门禁](#真实能力门禁)。默认完整交付门禁仍为 `./scripts/run_all_tests.sh`。

已有实体抽取、文档问答等配置中的 `llama_cpp` 不会自动变成 Kite。切换时应使用
模型类别 `llm` + `backend.type: kite_llm`，在 `backend.params` 中填写必要参数，
例如 `{"run_config_file":"run.json"}`。权重和运行配置相对 Pipeline 目录。节点 `random_seed` 默认 -1。
文档问答配置的 Embedding/Rerank 使用 ONNX。生成向量配置可将 Embedding 交给 Kite。
图像文档识别通过下述视觉协议接入。`kite_llm` 不提供音频转写协议。真实 ASR 使用独立构建中的
`whisper_asr` + `whisper_cpp`，见[模型准备](../configs/README.md)。

## Kite 部署示例套件

```bash
./scripts/fetch_real_test_models.sh --kite
./build/variants/kite-cpu/alg_demo --profiles-file demo/profiles_kite.json --suite real
# 单项运行：
./build/variants/kite-cpu/alg_demo --profiles-file demo/profiles_kite.json --profile ocr_invoice_qa_kite
```

`--profiles-file` 是通用部署配置入口。省略时保持原有 demo/profiles.json 和默认套件。
该套件包含实体抽取、问答、带精排问答、对话合规审核、OCR 票据问答，以及复用的关注词匹配与
ONNX 精排，以及 `doc_qa_kite_embeddings` 纯 Kite 问答。纯文本 LLM 使用 Kite。
混合配置的 Embedding/Rerank 使用 ONNX。所有结构化
解析配置均使用 fail 策略，不通过 JSON fallback 掩盖模型生成错误。

### 生成模型向量与纯 Kite 问答

Kite v0.1.0 的 `SetVerboseDataFlag(KLLM_VERBOSE_F_OUTPUT_EMBEDDING)` 与
`GetOutputEmbedding` 有实际实现，Backend 通过中性 `generated_token_embedding`
协议接入。它返回 **decode 生成 token 后** 的隐藏向量，并不返回输入全部 token 的
encoder 向量，也不是输入最后一个 token 的 prefill 向量。

`generated_text_embedding` 实现现有 `IEmbeddingModel`，由 Model 负责 prompt
prefix/suffix、last/mean 池化和请求级 L2 归一化。Backend 负责 greedy 生成、复制原生
输出及会话串行化，不承担池化或检索语义。模型参数须指定真实 `embedding_dim`。`normalize` 默认 true，影响该模型的全部节点。
`max_tokens` 为 1..64（默认 1），`pooling` 可选 last（默认）/mean，`add_bos` 默认 false。
只池化实际生成的 token。首步 EOS 没有向量时明确失败，不伪造或补齐向量。

```bash
./build/variants/kite-cpu/alg_pipeline_tool catalog
./build/variants/kite-cpu/alg_pipeline_tool validate configs/pipeline_doc_qa_kite_generated_embeddings.json
./build/variants/kite-cpu/alg_pipeline_tool plan configs/pipeline_doc_qa_kite_generated_embeddings.json
./build/variants/kite-cpu/alg_demo --profiles-file demo/profiles_kite.json --profile doc_qa_kite_embeddings
```

该示例复用既有问答 DAG，向量与回答生成都用 Qwen2.5-0.5B GGUF + Kite，向量维度 896。
这里的向量属于生成模型特征，Catalog 明确标注为 experimental。跑通链路不代表其
检索质量等同于 BGE。原有 `doc_qa_kite` 继续使用 BGE/ONNX。更换模型、prompt、
池化方式或生成上限会改变向量空间，已有索引需要重建，检索阈值与质量需要重新评估。

现有 BGE ONNX 模型仍要求 `tensor_graph`，不能仅把 backend 名改为 Kite。后续如果
Kite 提供 encoder/prefill 向量能力，应在模型执行层增加符合实际输出语义的协议/Model，
保持 Node、Pipeline 和 Operator 使用能力接口。

### 图像文档识别

`vision_document` 实现现有 IOcrModel，要求 image_text_generation Backend 协议。
它接收请求自有的 RGB8 `ImageFrameBatch`，按 stride 读取像素，转换 RGB 平面并按
patch_size 补白边。不裁剪内容。像素上限由 max_pixels 控制（默认 4194304，包含补边）。
像素帧的宿主内存契约见[业务接入指南](dev_guide/business_onboarding.md#4-需要新的宿主类型时)。
PNG/JPEG/二进制 PNM 仅由 Demo 解码后构造宿主帧，Demo 样例文件上限为 32 MiB。
模型 patch_size 必须与视觉投影文件匹配。随附 SmolVLM 模型使用 16。

Kite 通过原生 SetMultiModal_ChatHistory 编码图像并生成文字。run-config 必须包含
vision.mmproj，路径相对运行 JSON 所在目录，只允许其目录内的文件，不允许绝对路径
或父目录逃逸。普通 Qwen 文本 GGUF 不能替代视觉模型。必须使用匹配的 GGUF 与 mmproj。

该模型只返回 `combined_text`。`boxes` 为空，Operator 检测框数量为 0。
模型不提供检测框或置信度，因此不能用于需要精确坐标的 OCR 业务。
示例模型用于功能验证。票据转写和字段准确率需用目标数据评估。
当前实现不支持音频输入、视频或预计算视觉特征。

### 真实能力门禁

完成 `kite-cpu` 构建并准备模型后，分别运行与 CI 相同的三条实际方案：

```bash
./scripts/fetch_real_test_models.sh --kite
./build/variants/kite-cpu/alg_demo \
  --config configs/pipeline_entity_extract_kite.conf \
  --dataset data/corpus_entity_extract.txt --output-dir results/kite-real
./build/variants/kite-cpu/alg_demo \
  --config configs/pipeline_doc_qa_kite_generated_embeddings.conf \
  --dataset data/corpus_doc_qa.txt --output-dir results/kite-real
./build/variants/kite-cpu/alg_demo \
  --config configs/pipeline_ocr_invoice_qa_kite.conf \
  --dataset data/corpus_ocr_invoice_qa_kite.txt --output-dir results/kite-real
```

1. 检查每条命令的退出码。
2. 打开 `results/kite-real/<配置文件名去掉 .conf>/`。
3. 检查 `results.jsonl` 和 `summary.json` 中的请求状态。
4. 核对业务输出。

这些运行验证真实模型执行流程。业务质量需用目标数据集和独立预期值验收。
并发和多会话行为需单独测试。
缺少模型或未执行 Demo 时，不能将单元测试通过视为真实能力验收。具体业务与设备的验收要求见
[模型、构建与效果验收](VERIFIABLE_SELECTION.md#验收范围与发布准备)。
