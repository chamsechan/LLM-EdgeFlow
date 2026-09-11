# 任务：修改提示词并核对业务结果

`prompt-config` 从所选原生工具的 Catalog/Profile 创建独立的 Pipeline、部署 conf 和效果样例。
修改生成的配置后，使用 prepare 打印的 verify 命令验证同一份产物。

## 准备

先配置并构建工具。确定性 mock Profile 使用测试工具；生产方案使用生产工具。
工具和 Demo 必须属于同一 `--build-dir`，该目录的 CMake 源码根必须是当前项目。

```bash
cmake --build build --target alg_pipeline_tool_test alg_demo
python3 scripts/dev_recipe.py prepare prompt-config \
  --name EntityPromptTask --profile entity_extract_custom_mock \
  --tool build/alg_pipeline_tool_test --build-dir build \
  --pipeline configs/pipeline_entity_prompt_task.json --json
```

生成三份文件：`pipeline_entity_prompt_task.json`、同名 `.conf` 和 `_effects.json`。
现有文件不会被覆盖。conf 保留源 Profile 的整个 `data.mem_que`，包括 allocator、params、
容量与元数据字段，并由原生 Resolver 检查；`data.outputs` 在写入或构建前被拒绝。

## 修改与验收

在生成的 Pipeline 中修改提示词。效果文件包含可定位到真实数据集的路径、完整请求 ID 和
独立的 `/output/...` 业务期望；有意改变业务结果时，应按需求修改期望，不能从本次运行结果
自动回填期望。只检查 `/status` 不足以通过 recipe 的前置检查。

直接执行 `next_commands[].argv` 或其中的完整 command。verify 会检查配置、执行计划、部署
实际指向、资产及 Demo 输出；任何缺失、样本不一致或运行失败都会返回非零状态及失败阶段。
JSON 中只有真正完成的步骤会进入 `completed_steps`，效果结果保留在 `metrics` 中。

以下 Profile 提供仓库维护的确定性样例：

| Profile | 默认效果文件 | 模型根与清单 |
| --- | --- | --- |
| keyword_match_rules | tests/fixtures/effects/keyword_exact.json | models；models/asset_manifest.json |
| entity_extract_mock | tests/fixtures/effects/entity_mock_exact.json | 项目根；tests/fixtures/asset_manifest_test.json |
| entity_extract_custom_mock | 同上 | 同上 |

其他 Profile 必须在 prepare 提供 `--effects`；模型位于其他资产包时同时提供 `--model-root`
和 `--manifest`。这些路径会被完整带入生成的 verify 命令。效果样例使用其自身 dataset，
不会被 Profile 的 dataset 静默替换。

mock 使用仓库自有的中性文本 fixture，结果仅证明任务与验收工具正确工作。真实模型质量与设备
性能按[模型、构建与效果验收指南](../VERIFIABLE_SELECTION.md)另行验证。
