# 任务：修改提示词并核对业务结果

`prompt-config` 从已有 Demo Profile 生成独立方案和效果样例，方便修改提示词后核对结果。

## 准备

先按 [README](../../README.md#快速开始)完成构建。本例使用测试工具和固定回答的 mock 模型。

```bash
cmake --build build --target alg_pipeline_tool_test alg_demo
python3 tools/dev_recipe.py prepare prompt-config \
  --name entity_prompt_task --profile entity_extract_custom_mock \
  --tool build/alg_pipeline_tool_test --build-dir build \
  --pipeline demo/fixtures/mock/pipeline_entity_prompt_task.json --json
```

生成 `pipeline_entity_prompt_task.json`、同名 `.conf` 和 `_effects.json`，不会覆盖已有文件。
目标 Pipeline 放在原资源目录中，保证相对路径仍能找到模型与配套文件。

## 修改与验收

打开生成的 `pipeline_entity_prompt_task.json`，找到 `name` 为 `generate_entities` 的节点，
将其 `params.prompt_template` 字段改为：

```json
"你是实体与名词抽取助手。请只返回 JSON，不添加解释。\n输入：{{input}}"
```

保留短语 `实体与名词抽取助手`，测试模型会返回固定的 `{"nouns":["mock entity"]}`。
本例验证修改与运行流程；提示词效果需使用真实模型评估。

效果文件按输入顺序记录 `/output/...` 的业务期望。有意改变输出时按需求修改期望，
不要从运行结果自动回填；只检查 `/status` 不足以验收业务结果。

执行 prepare 输出的完整 verify 命令。它校验配置与资产，运行方案并核对输出；
本例应通过一条样本，结果包含 `nouns: ["mock entity"]`。失败时报告阶段并返回非零状态。

## 其他方案与自动化

Recipe 支持单个输出 Converter；工具和 Demo 必须来自当前项目的同一构建目录。
自动化调用可使用输出中的 `next_commands[].argv`。

以下 Profile 提供仓库维护的确定性样例：

| Profile | 默认效果文件 | 资产目录与清单 |
| --- | --- | --- |
| keyword_match_rules | tests/fixtures/effects/keyword_exact.json | configs；configs/asset_manifest.json |
| entity_extract_mock | tests/fixtures/effects/entity_mock_exact.json | demo/fixtures/mock；tests/fixtures/asset_manifest_test.json |
| entity_extract_custom_mock | 同上 | 同上 |

其他 Profile 在 prepare 时提供 `--effects`；自有资产清单使用 `--manifest`。
模型及文件参数相对 Pipeline 目录，视觉 projector 相对运行配置目录。
效果样例使用自身的 dataset，详见[效果验收指南](../VERIFIABLE_SELECTION.md)。
