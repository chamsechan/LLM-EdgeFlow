# 🚀 LLM-EdgeFlow

<p align="center">
  <strong>High-Performance C++ Pipeline &amp; Heterogeneous Inference Framework for Edge AI &amp; LLMs</strong><br>
  <em>面向边缘芯片与端侧场景的高性能 C++ 多模态推理编排框架</em>
</p>

<p align="center">
  <a href="https://github.com/chamsechan/LLM-EdgeFlow/actions/workflows/ci.yml"><img src="https://github.com/chamsechan/LLM-EdgeFlow/actions/workflows/ci.yml/badge.svg" alt="CI Status"/></a>
  <a href="https://en.cppreference.com/w/cpp/17"><img src="https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=c%2B%2B" alt="C++17"/></a>
  <a href="https://cmake.org/"><img src="https://img.shields.io/badge/CMake-3.16%2B-064F8C?style=flat-square&logo=cmake" alt="CMake"/></a>
  <a href="https://github.com/microsoft/onnxruntime"><img src="https://img.shields.io/badge/Backend-ONNX%20Runtime-0078D4?style=flat-square&logo=microsoft" alt="ONNX Runtime"/></a>
  <a href="https://github.com/ggerganov/llama.cpp"><img src="https://img.shields.io/badge/Backend-llama.cpp%20(GGUF)-F97316?style=flat-square" alt="llama.cpp"/></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-emerald?style=flat-square" alt="License"/></a>
</p>

LLM-EdgeFlow 通过声明式 Pipeline，把纯 C ABI 接入、DAG 调度、可复用能力节点与异构推理 Backend 组织成一条可校验、可观测、可替换的端侧推理链路。同一个算法库可以通过 JSON 配置组合不同业务，无需把硬件或模型细节写入业务节点。

## 核心能力

- **安全接入**：6 个标准 C 导出函数均由 `noexcept` 异常屏障保护，公共头文件保持纯 C ABI；同一 handle 的 Process/Control 串行执行。
- **声明式编排**：Pipeline 在执行前完成 Schema、端口类型、DAG 和并发写冲突校验，并直接消费不可变执行计划。
- **请求级黑板**：`AlgContext` 通过 `Read/Publish` 传递不可变强类型快照，保留 `(req_id, sub_id)` 样本溯源。
- **能力与硬件解耦**：模型语义通过中性协议连接 ONNX Runtime、llama.cpp 和后续 NPU Backend。
- **定长硬件批处理**：`FixedBatchExecutor` 统一负责切块、Dummy Padding、结果剥离与样本映射。
- **统一工具链**：Catalog、Validator、命令行工具和 Web DAG 工作台共享同一套 C++ Definition。

## 四层架构

<p align="center">
  <img src="doc/assets/architecture_flow.svg" alt="LLM-EdgeFlow Architecture" width="100%"/>
</p>

| 分层 | 职责 | 核心组件 |
| :--- | :--- | :--- |
| **Layer 1：C ABI 与 Operator 接入** | 解包、输入输出契约、生命周期和异常隔离 | `company_alg_interface.h`、Adapter、Operator |
| **Layer 2：Pipeline 与黑板** | 配置校验、DAG 计划、调度与请求状态 | `Pipeline`、`PipelineValidator`、`AlgContext` |
| **Layer 3：通用能力节点** | 无请求状态的前处理、推理调用和后处理 | `NodeBase`、`src/common_nodes/`、`include/nodes/` |
| **Layer 4：模型与 Backend** | 模型能力、中性执行协议和硬件批调度 | `IModel`、`IInferenceBackend`、`FixedBatchExecutor` |

完整的职责边界、数据流和类图参见[架构设计](doc/architecture.md)，扩展实现参见[开发者指南](doc/developer_guide.md)。

## 业务与成熟度

| 业务 | 输入 | 主要能力 | 配置与状态 |
| :--- | :--- | :--- | :--- |
| 长文档问答 | 长文本与问题 | Embedding + LLM | [生产配置](configs/pipeline_doc_qa.json) |
| 关注词匹配 | 文本 | 规则树快筛 | [生产配置](configs/pipeline_keyword_match.json) |
| 实体抽取 | 文本 | llama.cpp / Qwen | [生产配置](configs/pipeline_entity_extract.json) |
| 对话合规审计 | 对话与渠道 | Embedding + Rerank + LLM | [生产配置](configs/pipeline_dialogue_audit.json) |
| 跨编码器精排 | Query 与候选集 | ONNX Cross-Encoder | [生产配置](configs/pipeline_cross_rerank.json) |
| OCR 文档问答 | 图像与问题 | OCR + LLM | [Smoke/Test 配置](tests/fixtures/stage7/smoke/pipeline_ocr_doc_qa.json) |
| ASR 意图识别 | PCM 音频 | ASR + 槽位抽取 | [Smoke/Test 配置](tests/fixtures/stage7/smoke/pipeline_audio_asr_intent.json) |

OCR 与 ASR 的强类型契约已经接入框架，但仓库尚未交付对应的生产模型与 Backend；当前确定性实现仅用于测试和 Smoke Demo。

## 快速开始

```bash
# 配置并构建
cmake -B build -G Ninja -DLLM_EDGEFLOW_LINKER=auto
cmake --build build -j$(nproc)

# 运行完整质量门禁
./scripts/run_all_tests.sh

# 查看可用业务与 Profile，并运行 Smoke 套件
./build/alg_demo --list
./build/alg_demo --suite smoke
```

运行特定 Profile：

```bash
./build/alg_demo --profile entity_extract_mock
./build/alg_demo --profile doc_qa_onnx
```

真实推理 Profile 需要相应模型资产；无需模型的确定性 Smoke Profile 可用于验证完整控制流。项目不设置 ccache 程序或缓存路径，本地可使用 compiler wrapper，CI 可使用标准 CMake launcher。

## Pipeline 工具

```bash
# 终端查看拓扑
./build/alg_show configs/pipeline_doc_qa.json

# 查询 Catalog、校验配置和查看执行计划
./build/alg_pipeline_tool catalog --biz smart_doc_qa_v1
./build/alg_pipeline_tool validate configs/pipeline_doc_qa.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa.json

# 启动仅绑定 127.0.0.1 的 Web DAG 工作台
./show --web
```

详细用法和安全边界参见 [Pipeline Studio 指南](tools/visualizer/README.md)。

## 文档导航

| 目标 | 文档 |
| :--- | :--- |
| 了解系统边界与数据流 | [架构设计](doc/architecture.md) |
| 扩展 Adapter、Pipeline、Node、Model 或 Backend | [开发者指南](doc/developer_guide.md) |
| 使用公共 C 日志接口 | [Logging 指南](doc/logging.md) |
| 查看或编辑 Pipeline DAG | [Pipeline Studio 指南](tools/visualizer/README.md) |
| 查阅需求设计与验收记录 | [RFC 索引](doc/rfcs/README.md) |
| 浏览全部项目文档 | [文档目录](doc/README.md) |
| 查看版本演进摘要 | [Changelog](doc/CHANGELOG.md) |
| 参与开发与交付 | [Contributing](CONTRIBUTING.md) |

## 开发与交付

开发前先按变更风险判断是否需要 RFC，并在独立分支实施。交付前运行唯一完整门禁：

```bash
./scripts/run_all_tests.sh
```

完整生命周期见 [Contributing](CONTRIBUTING.md)，Agent 架构与路由见
[AGENTS.md](AGENTS.md)，长期设计决策见 [RFC 目录](doc/rfcs/README.md)。当前架构里程碑
为 **v6.0.0**；仓库尚未发布对应 Git tag，版本演进以 [Changelog](doc/CHANGELOG.md) 和已完成
RFC 为准。

## License

[MIT License](LICENSE). Third-party component notices are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
