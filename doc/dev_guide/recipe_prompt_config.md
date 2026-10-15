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

prepare 生成 Pipeline JSON、同名 `.conf` 和 `_effects.json`。已有文件不会被覆盖。
将目标 Pipeline 放在原资源目录中。模型和配套文件使用该目录下的相对路径。

## 修改与验收

1. 打开 `pipeline_entity_prompt_task.json`。
2. 找到 `name` 为 `generate_entities` 的节点。
3. 将 `params.prompt_template` 改为以下内容：

```json
"你是实体与名词抽取助手。请只返回 JSON，不添加解释。\n输入：{{input}}"
```

保留短语 `实体与名词抽取助手`，测试模型会返回固定的 `{"nouns":["mock entity"]}`。
本例验证修改与运行流程。提示词效果需使用真实模型评估。

效果文件按输入顺序记录 `/output/...` 的预期值。
如果需要改变输出，按业务需求修改预期值。不要直接复制本次运行结果作为预期值。

执行 prepare 输出的完整 verify 命令。verify 检查配置、资产和业务输出。
本例应通过一条样本，结果包含 `nouns: ["mock entity"]`。如果验证失败，工具报告失败阶段并返回非零状态。

## 其他方案与自动化

Recipe 支持单个输出 Converter。工具和 Demo 必须来自当前项目的同一构建目录。
自动化调用可使用输出中的 `next_commands[].argv`。

以下 Profile 提供仓库维护的确定性样例：

| Profile | 默认效果文件 | 资产目录与清单 |
| --- | --- | --- |
| keyword_match_rules | tests/fixtures/effects/keyword_exact.json | configs；configs/asset_manifest.json |
| entity_extract_mock | tests/fixtures/effects/entity_mock_exact.json | demo/fixtures/mock；tests/fixtures/asset_manifest_test.json |
| entity_extract_custom_mock | 同上 | 同上 |

其他 Profile 在 prepare 时提供 `--effects`。自有资产清单使用 `--manifest`。
模型及文件参数相对 Pipeline 目录，视觉 projector 相对运行配置目录。
效果样例使用自身的 dataset，详见[效果验收指南](../VERIFIABLE_SELECTION.md)。
