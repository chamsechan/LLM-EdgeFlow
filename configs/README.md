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

## 配置中的连接与模型

节点保留 `id`、`node_type`、`config`，直接用顶层 `inputs` / `outputs` 将逻辑端口映射到数据名。
必需输入必须明确连接，可选输入省略时表示未连接；输出省略映射时使用逻辑端口名。
Validator 从输入数据的唯一生产者推导依赖，`depends_on` 仅用于额外执行顺序，可以省略。
节点在数组中的位置不决定执行顺序；生产者缺失、多个生产者或成环会被拒绝。

模型能力从 `model_type` 的注册 Definition 获取。节点的模型引用（如 `config.bind_model`）
必须显式填写 `models[].model_id`；普通参数仍按 Definition 补齐默认值。
`max_parallel_workers` 范围为 1–64，默认 1，大于 1 时启用并行调度和相应安全检查。
旧的节点 `ports`、根级 `execution_mode` 和 `models[].capability` 字段不再接受。

## 业务入口与输出配置

每份 Pipeline 显式填写 `deployment.io.io_binding`，它决定外部 C 结构体与内部数据的转换契约。
框架从注册关系获得业务边界、输入/输出转换器、端口映射和输出类型，不靠文件名或后缀猜测。
Demo 根据配置自动选取运行入口，Profile 只保存配置路径、数据集和执行参数。
同一业务的多个 binding 必须声明一致的外部协议、载体和有效槽位，注册审计及部署预检都会校验。

```json
{
  "deployment": {
    "io": {
      "io_binding": "keyword_match.operator.v1"
    }
  }
}
```

必需输出槽自动采用注册的默认 allocator、容量与零 metadata。仅覆盖值写入
`deployment.io.out_mem`；输出类型直接来自注册的槽位定义。
可选输出槽通过显式槽配置启用，`{}` 表示默认值；`out_mem` 可以省略，`io_binding` 必须保留。
`out_mem.params` 表示分配器布局参数，Node 算法参数仍在节点 `config` 中。

`validate/plan` 与 Operator 共用部署准备和校验入口；`resolve-conf` 同时列出实际业务、
binding 与完整输出池规格。派生的业务名仅为查询结果，不需要写回配置。

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
