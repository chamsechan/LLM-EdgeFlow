# 示例方案配置

文件统一采用 `pipeline_<方案>_<变体>.json`，同名 `.conf` 仅保存 `pipe_path`，部署设置位于 Pipeline 的 `deployment`。
方案文件保持平铺，Pipeline Studio 可直接发现、打开并另存；测试替身方案继续位于
`demo/fixtures/mock/`。Model、Backend、权重和节点参数以文件内容与原生 Catalog 为准。

`default` 保留原始方案参数；`cpu` 是开源 Backend 的 CPU 演示配置（ASR 需启用 whisper.cpp）；`kite`
使用可选 Kite Backend（部分方案同时使用 ONNX Runtime）；`rules` 只运行规则节点。
这些名字不表示效果或生产验收已经通过。

JSON 字符串翻译的运行命令、输入输出与复用范围见[翻译方案](../doc/solutions/translate.md)。

需要选择可运行预设时查询 `alg_pipeline_tool catalog` 的 `profiles` 并核对资源；详细流程见
[Pipeline Studio](../tools/pipeline_studio/README.md)。

## 手写 JSON 的补全

完成 CMake 配置后，在 VS Code 打开仓内 [edgeflow.code-workspace](../edgeflow.code-workspace)，
执行任务 **Refresh Pipeline JSON Schema**。
任务构建当前 `build/alg_pipeline_tool` 并导出本地 Schema，仓内已关联 `configs/pipeline*.json`。
其他编辑器可以关联同一文件；命令行导出入口为：

```bash
./build/alg_pipeline_tool export-schema > build/pipeline.schema.json
```

选定 `node_type` 后可查看该节点的配置字段、端口名、说明、默认值及范围；Model/Backend 配置随类型选择。
注册或构建选项变化后重新刷新。Schema 只包含所选工具构建的能力；编辑 Mock 配置时用
`alg_pipeline_tool_test export-schema` 导出到另一文件，再关联 `demo/fixtures/mock/pipeline*.json`。
默认关联不覆盖 Mock 和故意非法的测试夹具，其他构建目录或自建方案需要调整编辑器关联。

固定文档结构与原生解析器共用 Core / Integration 的声明，Node/Model/Backend 参数来自 Definition。
无需向 Pipeline 添加 `$schema`（运行时仍拒绝该字段）。Schema 的默认值仅用于提示，不改写配置。
复杂 object/array 的内部结构、模型引用、DAG、来源、并行安全及自定义参数语义仍以
`alg_pipeline_tool validate/plan` 为准；Schema 无报错不等于配置已通过预检。
