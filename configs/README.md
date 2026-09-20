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
