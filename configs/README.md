# 示例方案配置

文件统一采用 `pipeline_<方案>_<变体>.json`，同名 `.conf` 仅保存 `pipe_path`，接入绑定与输出容量位于 Pipeline 的 `io`。
方案文件保持平铺，Pipeline Studio 可直接发现、打开并另存；测试替身方案继续位于
`demo/fixtures/mock/`。Model、Backend、权重和节点参数以文件内容与原生 Catalog 为准。

模型条目为 `type`（类别）、`name`、`file`、可选 `params` 和 `backend: {type, params?}`。
实现由类别和后端协议唯一选出，不在 JSON 中填写实现名。`params` 保存模型语义参数，
`backend.params` 保存后端资源参数；只写必要覆盖，默认值由 Definition 提供。
模型 `file` 和 `.File()` 声明的参数以 Pipeline JSON 所在目录为基准。

`default` 保留原始方案参数；`cpu` 是开源 Backend 的 CPU 演示配置（ASR 需启用 whisper.cpp）；`kite`
使用可选 Kite Backend（部分方案同时使用 ONNX Runtime）；`rules` 只运行规则节点。
这些名字不表示效果或生产验收已经通过。

JSON 字符串翻译的运行命令、输入输出与复用范围见[翻译方案](../doc/solutions/translate.md)。

需要选择可运行预设时查询 `alg_pipeline_tool catalog` 的 `profiles` 并核对资源；详细流程见
[Pipeline Studio](../tools/pipeline_studio/README.md)。

## 配置路径

宿主调用 Operator 时，Create 的 `model_path` 是部署根目录；它仅用于定位 `.conf`；模型条目的 `file` 由 Pipeline JSON 目录解析。配置预检使用同一解析规则：

| 字段 | 相对路径基准 | 目录与存在性约束 |
| --- | --- | --- |
| Create / 预检的 `model_path` | 宿主进程当前目录 | 必须是已存在的目录 |
| `cfg_file_name` | 部署根 | 必须是非空相对路径，指向根内已存在的普通配置文件（通常使用 `.conf` 后缀） |
| `.conf` 的 `pipe_path` | `.conf` 所在目录 | 可为相对或绝对路径，解析后的 Pipeline 必须存在且留在该目录内 |
| `models[].file`、`params.tokenizer_file`、`backend.params.run_config_file` | Pipeline JSON 所在目录 | 非空相对文件名，允许子目录，拒绝绝对路径、盘符、UNC、任何 `..` 分量及符号链接越界；预检允许文件尚未部署 |

目录边界同时检查路径规范化与符号链接目标，不允许通过 `..` 或符号链接逃逸，也没有
同名文件搜索回退。模型在实际创建时由相应 Backend 加载；预检成功不证明资产存在、
格式正确或可在目标设备上运行。`.conf` 只接受 `pipe_path`；Pipeline 的字段来自当前结构
声明与注册 Definition，未声明字段被拒绝，没有旧字段别名或自动迁移。

## 配置中的连接与模型

节点条目为 `type`、`name`、可选 `params` 和 `inputs` / `depends_on`。
`inputs` 将逻辑输入端口连到 `节点名.输出端口` 或 `input.逻辑端口`；输出项同样用 `inputs` 指定回包来源。
节点名不能为 `input` / `output` 或含 `.`。Validator 推导数据依赖，`depends_on` 仅补充执行顺序。
数组位置不决定执行顺序；引用不存在、类型不符或成环会被拒绝。只发布被引用的输出。
`text_embedding` 的缓存与 `vector_top_k` 的共享候选由实际输入生命周期决定，无需重复配置。

模型类别由条目 `type` 指定，所选实现来自注册 Definition。节点的模型引用（如 `params.bind_model`）
必须显式填写 `models[].name`；普通参数仍按 Definition 补齐默认值。
`max_parallel_workers` 范围为 1–64，默认 1，大于 1 时启用并行调度和相应安全检查。

## 输入输出与参数

Pipeline 根 `io.input` / `io.output` 是非空数组，每项按 `(type, name)` 选择已注册转换器。
`type` 是宿主 key 后缀，`name` 对应平台业务值；每份登记拥有一个宿主槽。所选 typed 端口组合
成传给 Core 的明确边界。Demo 从 SDK 预检查询载体与业务值，Profile 保存运行预设。

```json
{
  "io": {
    "input": [{"type":"keyword_in", "name":"keyword_match"}],
    "output": [{"type":"keyword_out", "name":"keyword_match",
                "inputs":{"matches":"match_keywords.matches"}}]
  },
  "pipeline": [{"type":"text_rule_match", "name":"match_keywords",
                "inputs":{"text":"input.sentence_text"}}]
}
```

可选 `params` 只填写必要覆盖，未写字段用代码声明的默认值。输出每个字符串有
`<field>_max_bytes` 整数参数，范围为 1 到平台硬上限，例如 `answer_text_max_bytes: 4095`。
翻译输出默认值为 8191；分配器、布局参数和 metadata 固定在登记，不由配置选择。
参数在 Create 中解析、Prepare、Validate，生效值来自 Prepare 后的声明成员。
可选输出也总是有池；宿主可按行省略 key。重复载体 type 的项使用 `name.type` 宿主 key，
唯一 type 继续接受任意非空前缀。有效 Process 批次上限为 `min(max_frame_depth, 64)`。

`validate` / `plan` 与 Operator 共用接入准备和 Core 校验入口。
本轮重构的工具升级在第 9 步完成；过渡阶段高级配置编辑与 schema 命令明确提示暂不支持。

## 手写 JSON 的补全

完成 CMake 配置后，在 VS Code 打开仓内 [edgeflow.code-workspace](../edgeflow.code-workspace)，
执行任务 **Refresh Pipeline JSON Schema**。
任务构建当前 `build/alg_pipeline_tool` 并导出本地 Schema，仓内已关联 `configs/pipeline*.json`。
其他编辑器可以关联同一文件；命令行导出入口为：

```bash
./build/alg_pipeline_tool export-schema > build/pipeline.schema.json
```

选定节点 `type` 后可查看该节点的配置字段、端口名、说明、默认值及范围；Model/Backend 配置随类型选择。
注册或构建选项变化后重新刷新。Schema 只包含所选工具构建的能力；编辑 Mock 配置时用
`alg_pipeline_tool_test export-schema` 导出到另一文件，再关联 `demo/fixtures/mock/pipeline*.json`。
默认关联不覆盖 Mock 和故意非法的测试夹具，其他构建目录或自建方案需要调整编辑器关联。

固定文档结构与原生解析器共用 Core / Integration 的声明，Node/Model/Backend 参数来自 Definition。
无需向 Pipeline 添加 `$schema`（运行时仍拒绝该字段）。Schema 的默认值仅用于提示，不改写配置。
复杂 object/array 的内部结构、模型引用、DAG、来源、并行安全及自定义参数语义仍以
`alg_pipeline_tool validate/plan` 为准；Schema 无报错不等于配置已通过预检。

## 权重与资源

方案 JSON 与所需权重、词表和 Kite run config 放在同一目录。`models[].file`、
模型的 `tokenizer_file` 和后端的 `run_config_file` 都相对 Pipeline JSON 所在目录解析；
可以包含子目录，但不能使用绝对路径或 `..` 分量。路径规范化后（包括符号链接目标）
必须留在方案目录内。预检允许资源尚未部署，模型或后端在实际 Create 时检查文件。
宿主传入的部署根用于定位 `.conf`，不改变模型文件的解析基准。

本目录中的 [asset_manifest.json](asset_manifest.json) 保存下载文件名、上游版本与 SHA-256；
下载脚本读取其中的 `artifacts`，校验哈希通过后才发布文件。权重和 tokenizer 词表保持
Git 忽略；资源清单及 [kite_text_run.json](kite_text_run.json)、
[kite_vision_run.json](kite_vision_run.json) 两份 run config 随仓库维护。

准备默认 CPU real Profile 的固定版本资源：

```bash
./scripts/fetch_real_test_models.sh --all
```

仅运行 GGUF Operator/Profile 的真实模型验收：

```bash
./scripts/fetch_real_test_models.sh --gguf-only
./scripts/run_real_model_e2e.sh
```

准备 Kite 文本、混合 ONNX/文本和图像/文档 Profile 的资源：

```bash
./scripts/fetch_real_test_models.sh --kite
```

该模式额外下载 `ggml-org/SmolVLM-256M-Instruct-GGUF` 版本
`b9e4379657e1450d04d02eec8e345667265b0a00` 的 SmolVLM-256M-Instruct Q8_0
及对应 projector，两者哈希固定在资源清单中。`kite_vision_run.json` 的 projector 路径
相对此目录。这些小模型用于功能回归；文档识别准确率需用目标数据评估。

准备 Whisper ASR 权重 `ggml-base.bin` 与 `ggml-tiny-q5_1.bin`：

```bash
./scripts/fetch_real_test_models.sh --whisper
```

构建预设、sidecar 校验和数据集验收记录见
[可验证选型](../doc/VERIFIABLE_SELECTION.md)。
