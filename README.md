# LLM-EdgeFlow

**面向边缘与端侧应用的 C++ 算法编排框架。**

[![CI](https://github.com/chamsechan/LLM-EdgeFlow/actions/workflows/ci.yml/badge.svg)](https://github.com/chamsechan/LLM-EdgeFlow/actions/workflows/ci.yml)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](CMakeLists.txt)
[![License: MIT](https://img.shields.io/badge/License-MIT-green)](LICENSE)

LLM-EdgeFlow 用算法节点处理规则、检索、文本、图像和语音，由 Pipeline JSON 描述节点连线，供宿主程序通过 C++ Operator SDK 调用。
项目目前处于实际业务接入前的开发与验证阶段，仓库提供可运行示例和自动化测试，模型效果、性能及目标设备适配需按具体场景验收。

[快速开始](#快速开始) · [架构与扩展](#架构与扩展) · [示例方案](#示例方案) · [开发文档](#开发文档)

## 按任务开始

首次使用请先完成[快速开始](#快速开始)，再按任务选择入口：

| 你要做什么 | 主要修改 | 入口 |
| :--- | :--- | :--- |
| 拉方案：组合已有能力 | Pipeline 配置、规则、提示词与连线 | [Studio 编排练习](tools/pipeline_studio/README.md#第一次编排) · [修改提示词](doc/dev_guide/recipe_prompt_config.md) |
| 开发 Node：补充业务算法 | 普通 C++ 函数、端口声明和测试 | [自定义 Node 入门](doc/dev_guide/first_custom_node.md) |
| 增加 Adapter：接入请求与响应 | 输入/输出转换器，优先复用现有载体 | [业务接入指南](doc/dev_guide/business_onboarding.md) |

<p align="center">
  <a href="doc/assets/framework_overview.svg">
    <img src="doc/assets/framework_overview.svg" alt="LLM-EdgeFlow 工作原理：配置经校验后由 Pipeline 调度通用与自定义节点，处理宿主输入并返回结果，需要推理时通过 Model 调用 Backend。" width="100%"/>
  </a>
</p>

## 快速开始

以下命令在仓库根目录执行（Linux CPU；macOS Apple Silicon 使用 `macos-arm64-cpu` 预设，见[跨平台构建](doc/VERIFIABLE_SELECTION.md#跨平台构建)）。
构建需要 C++17 编译器、CMake 3.19+、Ninja、Python 3 和 Git；首次构建需联网拉取固定版本依赖，模型权重需单独准备。

### 1. 获取并构建

```bash
git clone https://github.com/chamsechan/LLM-EdgeFlow.git
cd LLM-EdgeFlow

cmake --preset=inferlet-cpu -B build
cmake --build build --target alg_sdk alg_demo alg_pipeline_tool alg_show --parallel 4
```

上述 Linux 构建启用 Inferlet，也可运行无模型示例。其他 Backend 见[构建变体](doc/VERIFIABLE_SELECTION.md#构建变体)；Inferlet 模型运行见 [CPU 验证](doc/VERIFIABLE_SELECTION.md#inferlet-cpu-验证)。主要产物：

- `build/libllm_edgeflow_sdk.so`：集成到宿主程序的算法共享库
- `build/alg_demo`：统一示例运行入口
- `build/alg_pipeline_tool`：查询 Catalog、校验 Pipeline、查看执行计划
- `build/alg_show`：在终端查看 Pipeline 节点与连线

### 2. 运行第一个方案

运行无需模型权重的关键词匹配示例：

```bash
./build/alg_pipeline_tool validate configs/pipeline_keyword_match_rules.json
./build/alg_demo --profile keyword_match_rules \
  --dataset tests/fixtures/effects/keyword_inputs.txt \
  --output-dir results/quickstart
```

三条输入均处理成功（第一、三条命中 `SYSTEM_INIT`，第二条未命中），逐条结果与统计写入 `results/quickstart/keyword_match_rules/` 下的 `results.jsonl` 和 `summary.json`。如需体验运行时更新规则，可执行 `./build/alg_demo --profile keyword_match_control`。

### 3. 查看与编辑流程

```bash
# 查询当前构建可用的业务契约与节点
./build/alg_pipeline_tool catalog
./build/alg_pipeline_tool describe-node text_rule_match

# 查看经过校验的执行计划
./build/alg_pipeline_tool plan configs/pipeline_keyword_match_rules.json

# 启动本地 Web 工作台
./tools/pipeline_studio/server.py configs/pipeline_keyword_match_rules.json --web
```

本地 Web 工作台监听 `127.0.0.1`，支持连线、参数编辑、检查与试运行。后续可按 [Studio 编排练习](tools/pipeline_studio/README.md#第一次编排)修改规则并用 Demo 验证。

## 架构与扩展

框架自上而下分为四层，通常先复用节点编排 Pipeline，出现能力缺口时再扩展对应层（详见[架构设计](doc/architecture.md)）：

| 职责 | 何时扩展 | 主要入口 |
| :--- | :--- | :--- |
| 接入适配层 · Integration | 请求/响应格式或语义变化，或新增宿主载体 | `include/adapter/`、`include/edgeflow/operator/`、`src/adapter/` |
| 流程编排层 · Orchestration | 现有配置校验、调度或上下文无法满足需求 | `include/core/`、`src/core/` |
| 能力节点层 · Capability Nodes | 新增领域算法、数据处理或模型调用组合 | `src/custom_nodes/`（通用操作在 `src/common_nodes/`） |
| 模型执行层 · Model Execution | 新增模型前后处理语义或接入推理后端 | `src/engine/models/`、`src/engine/backends/` |

### 一个方案由哪些文件组成

一个可运行方案通常包含三类配置（详见[运行当前方案](tools/pipeline_studio/README.md#运行当前方案)与[配置目录](configs/README.md)）：

- **Pipeline JSON**：声明 `io.input` / `io.output` 转换器、节点连线、模型路径与参数，如 [pipeline_keyword_match_rules.json](configs/pipeline_keyword_match_rules.json)。
- **部署 `.conf`**：通过 `pipe_path` 指向目标 Pipeline JSON，如 [pipeline_keyword_match_rules.conf](configs/pipeline_keyword_match_rules.conf)。
- **Demo Profile（可选）**：在 [demo/profiles.json](demo/profiles.json) 中保存业务、配置与数据集预设。

## 示例方案

涉及模型的方案需准备匹配的构建变体与模型权重，实际可用能力以目标构建的 `./build/alg_pipeline_tool catalog` 为准。

| 场景 | 处理方式 | 配置与运行条件 |
| :--- | :--- | :--- |
| 关注词匹配 | 文本规则匹配与分类 | [配置](configs/pipeline_keyword_match_rules.json)；无需模型权重 |
| 实体抽取 | LLM 生成与结构化解析 | [Inferlet 配置](configs/pipeline_entity_extract_inferlet.json)；Linux 默认构建 + 语言模型 |
| 文档问答 | 分块、向量检索与 LLM 回答 | [配置](configs/pipeline_doc_qa_default.json)；`full-cpu`，ONNX Runtime + llama.cpp |
| 对话合规审核 | 检索、精排与 LLM 分析 | [配置](configs/pipeline_dialogue_audit_default.json)；`full-cpu`，向量/精排/语言模型 |
| Cross-Encoder 精排 | 问题与候选文本相关性评分 | [配置](configs/pipeline_cross_rerank_cpu.json)；ONNX Runtime + 精排模型 |
| OCR 票据问答 | 图像转写（纯文本）、字段抽取与问答 | [配置](configs/pipeline_ocr_invoice_qa_kite.json)；Kite + 视觉/语言模型，见 [kiteLLM 说明](doc/kitellm.md) |
| 语音识别与意图 | Whisper 转写与规则分类 | [配置](configs/pipeline_audio_asr_intent_cpu.json)；`full-cpu`，whisper.cpp |
| 翻译 | JSON 请求中 `query` 翻译为中文 | [配置](configs/pipeline_translate_cpu.json)；`full-cpu`，llama.cpp |

查看全部预设或运行无需真实模型的 Smoke 套件（真实模型效果评估见[业务效果验收](doc/VERIFIABLE_SELECTION.md#业务效果验收)）：

```bash
./build/alg_demo --list
./build/alg_demo --suite smoke
```

## 开发文档

| 目标 | 入口 |
| :--- | :--- |
| 查阅全部文档与开发 Skills | [文档目录](doc/README.md) |
| 理解节点端口、模型绑定与运行时控制 | [Node 常用写法](doc/dev_guide/custom_node_concepts.md) · [Control 入门](doc/dev_guide/first_control.md) |
| 扩展框架、模型或后端 | [开发者指南](doc/developer_guide.md) · [架构设计](doc/architecture.md) |
| 源码目录、头文件边界与命名约定 | [源码布局与命名](doc/dev_guide/source_layout.md) · [配置目录](configs/README.md) |
| 准备模型、构建变体与效果验收 | [模型、构建与效果验收](doc/VERIFIABLE_SELECTION.md) · [发布记录](doc/CHANGELOG.md) |

## 参与开发

在独立分支上开发，交付前运行完整质量门禁（需 `clang-format 18`、Java 17+ 及 CMake 所选 Python 环境中的 `jsonschema`）：

```bash
./scripts/run_all_tests.sh
```

开发与交付规约见 [CONTRIBUTING.md](CONTRIBUTING.md)，测试说明见 [tests/README.md](tests/README.md)，Agent 规则见 [AGENTS.md](AGENTS.md)。

## 许可证

采用 [MIT License](LICENSE)，第三方组件及许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
