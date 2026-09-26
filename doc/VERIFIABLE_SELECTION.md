# 可验证的模型、构建与业务效果选择

系统使用注册 Definition 和原生 `alg_pipeline_tool` 作为模型能力、Backend 协议、节点配置及端口规则的事实源。选择检查工具只检查部署资产和验收证据，不实现第二个 Validator。

## 使用路径

1. Studio 的“模型”页选择“资产组合”，自动填充已有模型类型、权重路径、tokenizer 或运行配置，以及 Model/Backend 参数。也可手动配置。
2. 选择 Backend 与模型实例，再将节点绑定到该实例；按端口名称连接业务输入、节点与业务输出。
3. “检查资产与构建选择”验证**已应用到方案**的配置。Studio 固定检查当前工作区 `models/`，文件存在仅标记 `present_unverified`，点击检查后才计算散列。
4. 在目标数据集上执行效果验收。未提供匹配的效果证据时，资产/构建通过也不会产生 `ready_for_biz=true`。

## 替换模型后确认实际生效配置

先查询目标构建的 Catalog，选择兼容 Model/Backend，修改 Pipeline 中对应实例的
`model_path`、`model_config` / `backend_config`。需要改绑模型实例时，运行
`describe-node <node_type>`，按返回的 `model_dependencies` 更新 Node `config` 中的对应字段。
例如 `LlmGenerateNode` 使用 `config.bind_model` 引用 `models[].model_id`；保留模型实例 ID
只更换权重时，无需修改节点绑定。
Node 的检索数、生成预算、模板等业务参数放在 Node `config`，字段说明与默认值通过
`describe-node` 或 Studio 属性查看。未声明为 Control 的参数在重新创建 handle 后生效。

Pipeline JSON 的 `models[].model_path` 是权重路径的唯一配置来源，相对路径以宿主传入的部署根为基准。
`.conf` 仅包含 `pipe_path` 定位该 JSON。更新模型条目的路径后，查看与 Operator Create 同一解析器得到的结果：

```bash
./build/alg_pipeline_tool resolve-conf configs/pipeline_keyword_match_rules.conf --root . --depth 2
```

`--root` 是部署根目录，默认当前目录；`--depth` 与 Demo 的 batch size、depth 两者最大值
一致，省略时为 Operator 默认值 25。响应中的 `configuration.model_paths` 列出每个模型路径的
解析结果，来源统一为 `pipeline.models.model_path`；该字段是观测报告，不是配置字段。
`effective_pipeline` 包含 Node/Model/Backend 默认值，`output_pools` 按逻辑槽位给出容量。
非法部署字段、输出池容量和 Pipeline 会直接报错。此命令不加载权重，不证明业务效果。

Studio 的“另存为可运行方案”和“运行草稿”共用配置生成与原生预检，使用模型条目中已保存的
路径。检查后用生成的命令运行样本，再执行下文效果验收；JSON 校验通过不能代替这一步。

## 资产清单

[models/asset_manifest.json](../models/asset_manifest.json) 包含 11 个权重/sidecar 条目的 SHA-256 与 8 个现有资产组合。权重、tokenizer、Kite 运行配置与视觉 projector 均纳入检查。

- 下载命令为 `./scripts/fetch_real_test_models.sh --all`、`--kite`、`--whisper` 或 `--gguf-only`，精确 URL 和 SHA 统一从清单读取。
- 清单中的 Model/Backend 配置是可选择的起点；兼容性仍由当前执行文件的 Catalog 校验。
- 清单路径相对资产目录；Pipeline 的 `models[].model_path` 相对宿主部署根；tokenizer、运行配置等 sidecar 路径相对实际模型所在目录解析。
- 变更 tokenizer/运行配置路径后不会借用旧组合的校验结论，而会变为 `unregistered`。接入新资产时，补充清单中的 `artifacts`、`selections.paths` 与完整 `files`，再实际校验。
- 零字节占位模型与未注册资产不会通过选择检查。
- SHA 匹配证明文件身份；模型是否能加载、是否满足业务需求，仍需实际执行验收。

资产检查示例（先准备上文模型资产）：

```bash
cmake --preset default-cpu
cmake --build --preset default-cpu --target alg_pipeline_tool alg_demo
python3 tools/verify_selection.py check \
  --pipeline configs/pipeline_doc_qa_default.json \
  --tool build/variants/default-cpu/alg_pipeline_tool \
  --model-root models --pipeline-root . --variant default-cpu \
  --output results/docqa-selection.json
```

`--model-root` 指定资产清单路径的基准目录，默认仓库的 `models/`；`--pipeline-root`
指定模型条目路径的宿主根，默认仓库根目录。调整资产目录不会覆盖 Pipeline 中已保存的模型路径。

`default-cpu` preset 包含 whisper.cpp；日常完整门禁的 `build/` 默认关闭它，不能直接作为
该 preset 的匹配产物。`--variant` 只校验 Backend 集合，不选择可执行文件；使用 preset
时需显式传入对应目录的 `--tool`，执行效果验收时还需指定同目录的 `--demo`。

报告使用 `schema_version=2`；报告分别给出 `configuration`、`models`、`build`、`effects` 和 `ready_for_biz`。普通 `check` 的退出码表示配置/资产/构建检查；发布门禁应增加 `--require-effects`，要求业务效果也通过。

## 构建变体

[CMakePresets.json](../CMakePresets.json) 提供三个互相隔离的构建目录；使用 preset 需要支持 CMake Presets 的 CMake 版本以及 Ninja。

| 变体 | 启用 Backend | 目的 |
| --- | --- | --- |
| `default-cpu` | ONNX Runtime、llama.cpp、whisper.cpp | 常规文本、向量和语音能力 |
| `kite-cpu` | ONNX Runtime、Kite | Kite 文本、视觉及生成向量能力 |
| `minimal` | 无推理 Backend | 规则、模板及纯数据处理方案 |

```bash
cmake --preset minimal
cmake --build --preset minimal --target alg_pipeline_tool
python3 tools/verify_selection.py check \
  --pipeline configs/pipeline_keyword_match_rules.json \
  --tool build/variants/minimal/alg_pipeline_tool --variant minimal
```

检查会比较预期变体的 Backend 集合与**实际执行文件**的 Catalog，记录工具 SHA-256 和平台。选择 `kite-cpu` 不会把当前进程动态切换为 Kite；需构建对应产物并指定其 `--tool`、`--demo`。Kite 与 llama/Whisper 的互斥条件直接遵守已有 CMake 约束。

构建变体检查不等于目标硬件或真实业务效果验收通过；这些结论需要对应环境的执行记录。

### Whisper 音频与运行限制

`whisper_asr` + `whisper_cpp` 接收整段单声道 float32 PCM，采样率必须为 16 kHz，
采样值必须有限且位于 `[-1, 1]`。调用方负责声道格式和重采样；当前路径不提供流式转写。
非空音频至少为 100 ms，最长时长由 `max_audio_seconds` 配置限制；合法采样率的空音频返回空文本。
转写文本同时受 Model 的 `max_output_bytes` 与 Operator 输出槽容量约束，超限报错。
具体输入校验见 [Whisper Model](../src/engine/models/whisper_asr/whisper_asr_model.cpp)，
配置字段及默认值查询目标构建的 `describe-model whisper_asr`。

当前 [Whisper Backend](../src/engine/backends/whisper_cpp/whisper_cpp_backend.cpp) 支持 CPU，
设备 ID 为 0 或省略。启用 `ENABLE_WHISPERCPP` 还需启用 `ENABLE_LLAMACPP` 复用 GGML，
并关闭 `ENABLE_KITELLM`；约束由 [CMake 模块](../cmake_ext/WhisperCpp.cmake)执行。
权重准备见[模型资产说明](../models/README.md)。默认构建通过、测试替身通过或单条真实音频通过，
均不能代替目标语料的识别效果和目标设备性能验收。

## 业务效果验收

复用现有 `alg_demo` 的样例读取、宿主载体构造和 SDK 执行路径；业务请求的解包与响应
组装仍由 Adapter 完成，见[输入输出边界](dev_guide/business_onboarding.md#输入输出以-operator-接口为边界)。
验收器在 `--pipeline-root` 指定的宿主根内生成临时 Pipeline 和 `.conf`，保留所选 Pipeline 的
`models[].model_path`，并从 Pipeline JSON（或 `--conf` 定位的原 JSON）继承 `deployment.io`
输出池配置。`--model-root` 仅用于资产清单校验，不改写模型条目中的路径。

验收固定使用 CPU、device 0、batch 1；Demo 默认使用所选规则/提示词。这个版本的验收目标是配置正确性与选定输出字段的业务效果；目标设备性能验收需要相应环境与后续测试定义。

```bash
python3 tools/verify_selection.py evaluate \
  --pipeline configs/pipeline_keyword_match_rules.json --variant default-cpu \
  --tool build/variants/default-cpu/alg_pipeline_tool \
  --demo build/variants/default-cpu/alg_demo \
  --effects tests/fixtures/effects/keyword_exact.json \
  --output results/keyword-effects.json

python3 tools/verify_selection.py check \
  --pipeline configs/pipeline_keyword_match_rules.json --variant default-cpu \
  --tool build/variants/default-cpu/alg_pipeline_tool \
  --demo build/variants/default-cpu/alg_demo \
  --effects tests/fixtures/effects/keyword_exact.json \
  --evidence results/keyword-effects.json --require-effects
```

效果规范包括业务名、相对规范文件的 dataset 路径、请求 ID、需要比较的输出 JSON Pointer 和最低通过率。参考 [关键词规范](../tests/fixtures/effects/keyword_exact.json)。其他业务可以使用已有 Demo 支持的输入数据集格式，并为相关结构化字段编写标注。

结果按 request ID 比较；重复、缺失、额外请求或非零状态都会失败。指标是 **selected_fields_exact_match**，仅表示指定数据集上所选字段的精确匹配率，不代表通用模型准确率，也不包含语义等价、召回率或复杂生成质量评测。

验收记录包含实际样本输出、模型资产散列、完整 Pipeline、构建工具、Demo/SDK 文件、部署容量、数据集及评价规范指纹。任何一个被记录输入变化，旧记录都会失效。检查会从记录重新计算指标，避免直接相信手填的汇总分数。指纹用于过期检测；这些本地记录并非经过签名的审计证明，也不覆盖目标硬件和所有系统动态依赖。

当前仓库已提供并实际运行的业务标注集只有关键词 4 条样本。其他模型组合仍需各自的数据集与验收记录，不能因为资产或构建检查通过就标记效果合格。

## 验收范围与发布准备

项目尚未正式发布。工程测试、可编译教程和自动化代理走查已有覆盖，真实新开发者独立完成任务的
体验验收尚无完成记录，不能据此宣称上手效率已验证。开展试用时，选择实际需要的编排、Node
扩展、Control 或接入任务，记录代码与环境基线、完成耗时、求助原因、修改范围和结果；未执行的
任务标为未覆盖。

每个拟上线方案应预先确定数据集、效果指标、输出容量、延迟、吞吐、内存与稳定性要求，
使用匹配的模型、配置和构建产物验收。目标设备还需独立验证宿主接入、资源生命周期、并发、
长时间运行及故障恢复。记录通过项、失败样本和未覆盖范围；本页的效果检查工具不代替这些专项验证。

公司内部 SDK 的真实契约与目标硬件尚未在此外部环境验收。完整项目进入授权内网，且获得真实
SDK、工具链、设备和模型的合规访问路径后，才能核对接口、枚举、结构布局、所有权、线程与错误
语义，并开展实际集成和部署验收。外部工作区只准备中立接入边界，不请求、推断、复制或提交
内部头文件、库、模型、配置或凭据；[平台模拟定义](../include/platform_mock/README.md)不证明内部兼容性。
