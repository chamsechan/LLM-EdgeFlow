# 可验证的模型、构建与业务效果选择

系统继续使用注册 Definition 和原生 `alg_pipeline_tool` 作为模型能力、Backend 协议、节点配置及端口规则的事实源。新增的工具只检查部署资产和验收证据，不实现第二个 Validator。

## 使用路径

1. Studio 的“模型”页选择“资产组合”，自动填充已有模型类型、权重路径、tokenizer 或运行配置，以及 Model/Backend 参数。也可手动配置。
2. 选择 Backend 与模型实例，再将节点绑定到该实例；按端口名称连接业务输入、节点与业务输出。
3. “检查资产与构建选择”验证**已应用到方案**的配置。Studio 固定检查当前工作区 `models/`，文件存在仅标记 `present_unverified`，点击检查后才计算散列。
4. 在目标数据集上执行效果验收。未提供匹配的效果证据时，资产/构建通过也不会产生 `ready_for_business=true`。

## 资产清单

[models/asset_manifest.json](../models/asset_manifest.json) 包含 11 个权重/sidecar 条目的 SHA-256 与 8 个现有资产组合。权重、tokenizer、Kite 运行配置与视觉 projector 均纳入检查。

- 下载命令仍为 `./scripts/fetch_real_test_models.sh --all`、`--kite`、`--whisper` 或 `--gguf-only`，精确 URL 和 SHA 统一从清单读取。
- 清单中的 Model/Backend 配置是可选择的起点；兼容性仍由当前执行文件的 Catalog 校验。
- 变更 tokenizer/运行配置路径后不会借用旧组合的校验结论，而会变为 `unregistered`。接入新资产时，补充清单中的 `artifacts`、`selections.paths` 与完整 `files`，再实际校验。
- 零字节占位模型与未注册资产不会通过选择检查。
- SHA 匹配证明文件身份；模型是否能加载、是否满足业务需求，仍需实际执行验收。

命令行示例（C ABI 语义的 model root 直接包含权重，不重复 `models/` 前缀）：

```bash
python3 tools/verify_selection.py check \
  --pipeline configs/pipeline_doc_qa.json \
  --model-root models --variant default-cpu \
  --output results/docqa-selection.json
```

报告分别给出 `configuration`、`models`、`build`、`effects` 和 `ready_for_business`。普通 `check` 的退出码表示配置/资产/构建检查；发布门禁应增加 `--require-effects`，要求业务效果也通过。

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
  --pipeline configs/pipeline_keyword_match.json \
  --tool build/variants/minimal/alg_pipeline_tool --variant minimal
```

检查会比较预期变体的 Backend 集合与**实际执行文件**的 Catalog，记录工具 SHA-256 和平台。选择 `kite-cpu` 不会把当前进程动态切换为 Kite；需构建对应产物并指定其 `--tool`、`--demo`。Kite 与 llama/Whisper 的互斥条件直接遵守已有 CMake 约束。

本次实际验证了当前完整 CPU 构建与 `minimal` 工具构建。Kite preset 的提供不等于 Kite 目标硬件或真实业务效果验收通过。

## 业务效果验收

复用现有 `alg_demo` 的业务输入转换和执行路径。验收器为选定 Pipeline 生成临时 `.conf`，从 `--conf`（默认同名 `.conf`）继承输出池配置，按 `--model-root` 生成模型路径；不会沿用原 `.conf` 中可能覆盖模型选择的 `model_paths`。

验收固定使用 CPU、device 0、batch 1，并传入新增 `--no-default-control`，保证 Demo 不用示例热更新覆盖所选规则/提示词。这个版本的验收目标是配置正确性与选定输出字段的业务效果；目标设备性能验收需要相应环境与后续测试定义。

```bash
python3 tools/verify_selection.py evaluate \
  --pipeline configs/pipeline_keyword_match.json --variant default-cpu \
  --effects tests/fixtures/effects/keyword_exact.json \
  --output results/keyword-effects.json

python3 tools/verify_selection.py check \
  --pipeline configs/pipeline_keyword_match.json --variant default-cpu \
  --effects tests/fixtures/effects/keyword_exact.json \
  --evidence results/keyword-effects.json --require-effects
```

效果规范包括业务名、相对规范文件的 dataset 路径、请求 ID、需要比较的输出 JSON Pointer 和最低通过率。参考 [关键词规范](../tests/fixtures/effects/keyword_exact.json)。其他业务可以使用已有 Demo 支持的输入数据集格式，并为相关结构化字段编写标注。

结果按 request ID 比较；重复、缺失、额外请求或非零状态都会失败。指标是 **selected_fields_exact_match**，仅表示指定数据集上所选字段的精确匹配率，不代表通用模型准确率，也不包含语义等价、召回率或复杂生成质量评测。

验收记录包含实际样本输出、模型资产散列、完整 Pipeline、构建工具、Demo/SDK 文件、部署容量、数据集及评价规范指纹。任何一个被记录输入变化，旧记录都会失效。检查会从记录重新计算指标，避免直接相信手填的汇总分数。指纹用于过期检测；这些本地记录并非经过签名的审计证明，也不覆盖目标硬件和所有系统动态依赖。

当前仓库已提供并实际运行的业务标注集只有关键词 4 条样本。其他模型组合仍需各自的数据集与验收记录，不能因为资产或构建检查通过就标记效果合格。
