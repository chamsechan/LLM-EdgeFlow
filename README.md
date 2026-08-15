# 🚀 LLM-EdgeFlow

<p align="center">
  <img src="doc/assets/architecture_flow.svg" alt="LLM-EdgeFlow Architecture" width="100%"/>
</p>

<p align="center">
  <strong>High-Performance C++ Pipeline &amp; Heterogeneous Inference Framework for Edge AI &amp; LLMs</strong><br>
  <em>专为边缘芯片与端侧场景打造的高性能 C++ 大模型流水线与异构推理编排框架</em>
</p>

<p align="center">
  <a href="https://en.cppreference.com/w/cpp/17"><img src="https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=c%2B%2B" alt="C++17"/></a>
  <a href="https://cmake.org/"><img src="https://img.shields.io/badge/CMake-3.16%2B-064F8C?style=flat-square&logo=cmake" alt="CMake"/></a>
  <a href="https://github.com/google/googletest"><img src="https://img.shields.io/badge/GoogleTest-v1.14.0-34A853?style=flat-square&logo=google" alt="Google Test"/></a>
  <a href="https://github.com/microsoft/onnxruntime"><img src="https://img.shields.io/badge/Engine-ONNX%20Runtime-0078D4?style=flat-square&logo=microsoft" alt="ONNX Runtime"/></a>
  <a href="https://github.com/ggerganov/llama.cpp"><img src="https://img.shields.io/badge/Engine-llama.cpp%20(GGUF)-F97316?style=flat-square" alt="llama.cpp"/></a>
  <a href="https://github.com/chamsechan/LLM-EdgeFlow/blob/main/LICENSE"><img src="https://img.shields.io/badge/License-MIT-emerald?style=flat-square" alt="License"/></a>
  <a href="#"><img src="https://img.shields.io/badge/Arch-x86__64%20%7C%20aarch64-8B5CF6?style=flat-square" alt="Arch"/></a>
</p>

---

## 🌟 核心特性 (Key Highlights)

- 🛡️ **标准纯 C ABI 隔离与安全屏障**：导出标准 6 大 C 接口，内置 `noexcept` 异常防火墙，彻底杜绝下游宿主崩溃。
- 🎯 **定长硬件 DMA 批处理调度 (`FixedBatchExecutor`)**：专为端侧/NPU 定长 Batch 设计，全泛型自动切块、补齐 Dummy Pad、推理后剥离并保持 `(req_id, sub_id)` 样本溯源。
- ⚡ **异构推理引擎 PIMPL 解耦**：原生封装 **ONNX Runtime**、**llama.cpp (GGUF)** 与 **专有 NPU DMA 内核**，切换芯片/引擎无需改动任何业务代码，纯 JSON 配置热插拔。
- 🧠 **动态类型安全黑板 (`AlgContext`)**：基于 `std::any` 传递多模态复杂张量与结构体，零冗余内存拷贝，请求结束自动释放。
- 📊 **原生双模可视化工具链**：内置嵌入式专用 **纯 C++ 原生命令行工具 (`./build/alg_show`)** 与 **交互式 Web DAG 工作台 (`./show --web`)**。

---

## 🏛️ 4 层分层架构 (Architecture)

| 分层 | 核心职责 | 核心组件 / 文件 |
| :--- | :--- | :--- |
| **Layer 1: C ABI 适配层** | 客户端解包、类型强转与 `noexcept` 异常屏障 | `include/company_alg_interface.h`<br>`src/adapter/company_c_adapter.cpp` |
| **Layer 2: 管线与黑板层** | DAG 拓扑执行、请求级动态黑板与模型容器 | `Pipeline`, `AlgContext`, `SessionContext`, `TraceableItem<T>` |
| **Layer 3: 业务专属算子池** | 前处理分片、模型调用封装、规则快筛与后处理精准对齐 | `INode`, `NodeFactory`, `src/business/*` (7大业务算子) |
| **Layer 4: 异构引擎层** | 纯虚能力接口、定长批调度模板与三方引擎实现 | `FixedBatchExecutor`, `ONNX Runtime`, `llama.cpp`, `MockNPU` |

<details>
<summary><b>🔍 查看详细 UML 类图设计 (Class Diagram)</b></summary>

<p align="center" style="margin-top: 10px;">
  <img src="doc/assets/architecture_class_diagram.svg" alt="LLM-EdgeFlow UML Class Diagram" width="100%"/>
</p>

</details>

---

## 📦 已支持的 7 大多模态业务 (Multi-Modal Matrix)

同一算法库 `libcompany_alg_sdk.so`，仅需传入不同 JSON 配置文件即可无缝热切换：

| 业务 | 业务名称 | 输入模态 | 挂载模型 / 算法能力 | 输出模态 | 配置文件 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Biz 1** | **关注词规则匹配** | 文本句子 | 纯规则树快筛 (零模型) | 命中的类别与词 JSON | [`pipeline_keyword_match.json`](configs/pipeline_keyword_match.json) |
| **Biz 2** | **实体名词抽取** | 文本句子 | 0.6B Qwen / llama.cpp | 抽取的名词列表 JSON | [`pipeline_entity_extract.json`](configs/pipeline_entity_extract.json) |
| **Biz 3** | **智能长文档问答** | 长文本+提问 | Embedding + 1.5B LLM | 切片数+意图+生成回答 | [`pipeline_doc_qa.json`](configs/pipeline_doc_qa.json) |
| **Biz 4** | **对话风控质检** | 对话流+渠道 | Embedding + Rerank + 7B LLM | 风险分+等级+质检判决 | [`pipeline_dialogue_audit.json`](configs/pipeline_dialogue_audit.json) |
| **Biz 5** | **多模态发票抽取** | 图像路径+Query | OCR 框检测识别 + LLM | 识别框数+结构化发票 JSON | [`pipeline_ocr_doc_qa.json`](configs/pipeline_ocr_doc_qa.json) |
| **Biz 6** | **语音识别与槽位抽取** | 原始音频 PCM | Speech ASR + NLU 槽位提取器 | 转写文本+意图槽位 JSON | [`pipeline_audio_asr_intent.json`](configs/pipeline_audio_asr_intent.json) |
| **Biz 7** | **纯语义精排矩阵打分** | 1 Query + N 候选 | ONNX Cross-Encoder 矩阵计算 | Top-K 打分与排序索引 | [`pipeline_cross_rerank.json`](configs/pipeline_cross_rerank.json) |

---

## 🚀 快速开始 (Quick Start)

### 1. 编译 (CMake 自动拉取 GTest, ONNX Runtime 与 llama.cpp)
```bash
mkdir -p build && cd build
cmake .. && make -j4
```

### 2. 运行 7 大业务端到端演示
```bash
./build/alg_demo
```

### 3. 运行 Google Test (GTest) 单元测试套件
```bash
./build/test_batch_executor              # 定长硬件批调度与样本溯源单测
./build/test_framework_core              # 核心黑板/反射工厂/模型管理器单测
./build/test_c_abi_safety                # C ABI 安全边界与 50 轮生命周期压测
./build/test_qwen_engines_comparison     # Qwen (NPU vs llama.cpp) 交叉比对
./build/test_different_io_modalities     # 多模态异构 I/O 隔离单测
```

### 4. 自动化质量验收与代码格式化
```bash
./scripts/run_all_tests.sh               # 执行全量 6 阶段自动化回归测试
./scripts/format.sh                      # Google C++ Style 一键格式化
```

---

## 📊 DAG 可视化调试 (Visualizer)

```bash
# 1. 嵌入式/终端原生 ASCII 拓扑打印 (纯 C++ 零依赖)
./build/alg_show configs/pipeline_ocr_doc_qa.json

# 2. 启动交互式 Web DAG 工作台 (浏览器访问 http://localhost:8080)
./show configs/pipeline_dialogue_audit.json --web
```

---

## 📁 代码目录布局 (Layout)

```text
LLM-EdgeFlow/
├── include/
│   ├── company_alg_interface.h  # Layer 1: 标准 C ABI 导出头文件
│   ├── core/                    # Layer 2: 框架核心 (AlgContext, Pipeline, TraceableItem)
│   └── engine/                  # Layer 4: 引擎接口 (FixedBatchExecutor, IModelEngine)
├── src/
│   ├── adapter/                 # Layer 1: C ABI 安全胶水层 (company_c_adapter.cpp)
│   ├── core/                    # Layer 2: Pipeline 调度器实现
│   ├── business/                # Layer 3: 7 大多模态业务算子库
│   ├── engine/                  # Layer 4: 异构引擎实现 (mock_npu, onnx, llama_cpp)
│   └── tools/                   # 纯 C++ 原生 DAG 可视化工具 (alg_show.cpp)
├── configs/                     # 9 大标准化业务配置 (JSON)
├── demo/                        # 7 大业务端到端演示 (main.cpp)
├── tests/                       # Google Test (GTest) 单元测试套件
└── tools/visualizer/            # 交互式 Web DAG 可视化平台
```

---

## 📄 License & Standards

- **Language**: C++17 (Strictly formatted via `.clang-format`)
- **Test Framework**: Google Test (GTest v1.14.0 via CMake FetchContent)
- **License**: [MIT License](LICENSE)
