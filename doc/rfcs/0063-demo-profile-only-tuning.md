# RFC 0063: Demo 执行参数仅由 Profile 提供

- **RFC 编号**：0063-demo-profile-only-tuning
- **创建日期**：2026-09-20
- **文档状态**：Completed
- **关联分支**：`refactor/demo-profile-only-tuning`
- **目标版本**：当前开发版本
- **负责人 / 作者**：LLM-EdgeFlow maintainers
- **关联决策**：局部取代 RFC-0005 §6.2–6.3 的四项执行参数 CLI 与覆盖规则。

## 1. 范围与决策

删除 Demo 的 `--chip`、`--device-id`、`--batch-size`、`--depth` 及覆盖标记；
旧参数返回退出码 2。Profile 保留原校验，缺省值为 `cpu / 0 / 1 / 1`。
其他 CLI 覆盖、SDK 接口、Pipeline schema、批次分块与输出池语义不变。

删除无消费者的 `has_output_dir` 和 Suite 文件读取包装函数；Suite 在一次校验后的文档上
选择、合并。Catalog 列举也只读取一次 Profile。C++ 默认值由 Demo 公共辅助头拥有；
Studio 只复制显式执行字段，预检直接按 batch/depth 缺省 1 计算，不增加默认值查询协议。
工具使用 `DeploymentIoConfig`，Studio 使用已有 `resolve-conf` 的 `pipeline_path`，
删除项目根目录同名文件回退；SDK 业务绑定与容量校验保留。

## 2. 兼容与迁移

调用方使用 `--profiles-file <path> --profile <name>`；工具缺省平台从 `ax650` 统一为 `cpu`。
Studio 在输出目录写入 `demo-profile.json`，只包含业务、配置、数据集和显式执行字段，
不复制 Control。每次保存更新此文件，生成命令读取最新配置；不维护哈希快照或版本冲突协议。
临时任务结束清理 Profile；持久命令需保留输出目录中的 Profile 文件。
JSON Prompt 和效果验证工具删除与默认值等价的 CLI 参数。原生 `resolve-conf --depth` 保留。
相对 `pipe_path` 基于 `.conf` 目录解析并禁止越界；Studio 在选择 Profile 时执行原生部署预检。

不保留本次范围内旧接口兼容：删除 `resolve-conf` 的单输出别名 `output_pool`，统一按槽位
读取 `output_pools`；Studio 内存状态只保留 `conf_path`，部署 I/O 只取 Pipeline 文档。
Demo 平台名统一为 `ax650`、`ascend310p`、`ascend910b`、`rk3588`、`cuda`、`cpu`
（大小写不敏感），删除其他名称别名并迁移仓内 Profile。

## 3. 验证与完成条件

保留旧参数拒绝、Profile 非默认/缺省/非法值、Suite 与业务实跑、原生配置解析和保存命令
实跑测试；删除生成文件布局、命令结构、帮助文案及重复解析路径测试。执行统一门禁
`./scripts/run_all_tests.sh`。浏览器、真实模型与硬件验收不属于本次范围。

## 4. 实施结果

代码、测试与文档已按上述范围收敛；独立评审、聚焦测试与最终统一门禁确认完成。
用法见 [Demo 接入](../dev_guide/business_onboarding.md#5-统一-demo-接入) 和
[Studio 运行指南](../../tools/pipeline_studio/README.md#运行当前方案)。
