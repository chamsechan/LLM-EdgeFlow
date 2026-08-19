# 🚀 LLM-EdgeFlow

<p align="center">
  <img src="doc/assets/architecture_flow.svg" alt="LLM-EdgeFlow Architecture" width="100%"/>
</p>

<p align="center">
  <strong>High-Performance C++ Pipeline &amp; Heterogeneous Inference Framework for Edge AI &amp; LLMs</strong><br>
  <em>专为边缘芯片与端侧场景打造的高性能 C++ 大模型流水线与异构推理编排框架</em>
</p>

<p align="center">
  <a href="https://github.com/chamsechan/LLM-EdgeFlow/actions/workflows/ci.yml"><img src="https://github.com/chamsechan/LLM-EdgeFlow/actions/workflows/ci.yml/badge.svg" alt="CI Status"/></a>
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

> 📖 **4 层扩展开发说明书与代码模板详见**：[doc/developer_guide.md](doc/developer_guide.md)

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

```bash
# 1. 编译工程 (自动拉取 GTest, ONNX Runtime 与 llama.cpp)
mkdir -p build && cd build
cmake .. && make -j4

# 2. 自动化执行全量测试套件 (CTest & Google Test)
ctest --output-on-failure

# 3. 运行 7 大业务端到端全链路集成演示
./alg_demo
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

## 📝 更新日志 (Changelog)

- **v1.5.0** *(2026-08)*
  - 🛡️ **纯 C11 ABI 安全边界标准重构 (ARCH-001)**：`include/company_alg_interface.h` 彻底剔除 `<vector>` 等 C++ 符号依赖，导出纯 C 标准指针数组批处理接口并提供 C++ 向后兼容包装层；新增纯 C 语言编写的 `test_c11_abi_compliance` 门禁测试。
  - 🧩 **Layer 1 业务适配器注册中心 (`IBusinessAdapter` & ARCH-003)**：将 370 行中心单体 Adapter 重构为可插拔的 `BusinessAdapterRegistry` 机制，7 大业务各自分离为高内聚的独立 Adapter 类，消除中心分发单点修改冲突。
  - 🧱 **Layer 3 算子与交付层彻底解耦 (ARCH-002)**：所有 8 个业务后处理算子全面消除对 `company_alg_interface.h` 的反向依赖，改由各业务领域 DTO 与 `AlgContext` 交互；新增 `scripts/check_layer_isolation.sh` 架构防腐脚本作为 CI 强门禁。
  - 🌐 **SessionContext 运行时环境与设备参数贯通 (ARCH-010)**：新增 `RuntimeOptions`，贯通 `model_root_dir` 相对路径解析与加速卡 `device_id` 自动注入。
- **v1.4.0** *(2026-08)*
  - 🎨 **DAG 节点图可视化编辑与节点工坊 (Visual Graph Studio)**：Web 工作台全面升级为**支持拖拽创建算子节点、动态配置上游依赖、实时成环死锁拦截、修改属性并一键导出 C++ 标准 Pipeline JSON** 的全功能节点工坊 (`./show --web`)。
  - 🧪 **物理真实模型物理压测链路隔离**：新增独立的 `test_real_models_e2e` 与 `./scripts/run_real_model_e2e.sh`，实现官方 `llama.cpp` 真实自回归 Token 解码、物理权重批处理与 C ABI 端到端硬件吞吐压测，与日常 0.3 秒敏捷 CTest 零干扰隔离。
- **v1.3.0** *(2026-08)*
  - 📐 **原生 DAG 拓扑排序与波前异步并发调度器**：核心 `Pipeline` 全面支持显式依赖 (`depends_on`)、Kahn 算法拓扑波前分层调度 (`execution_mode: "parallel"`，单节点主线程直跑，多兄弟节点线程池并发，时延直降 40%~60%) 与成环死锁检测 (`CycleDetection`)。
  - 🔒 **线程安全并发读写黑板 (`AlgContext`)**：基于 `std::shared_mutex` 实现读读并发、写写互斥，完美支撑多算子多线程无锁竞争访问。
  - ⚡ **10 大 CTest 工业级全场景测试矩阵**：新增 `RuntimeControlAndHotSwapTest` 与 `EngineFaultToleranceAndLifecycleTest`，全量覆盖硬件故障注入、在线动态热更新、5 层深拓扑并发、大负载确定性析构与全局高频生命周期压测。
- **v1.2.0** *(2026-08)*
  - ✨ **全模态与异构引擎支持**：新增 OCR 发票图文抽取、语音 ASR 转写与 ONNX Cross-Encoder 语义精排，全量支持 7 大异构业务。
  - ⚡ **7 大 CTest 自动化测试套件**：全量覆盖 RAG 切片问答、风控质检、8 线程高并发竞争压测、边界脏数据容错与 C ABI 安全。
  - 📦 **零静态代码依赖**：`nlohmann/json` 全面切换为 CMake `FetchContent` 动态拉取，彻底剔除本地第三方源码。
  - 🛠️ **双模可视化与开发者体系**：提供纯 C++ 原生 ASCII 拓扑命令行工具 (`alg_show`)、Web 仿真工作台与 4 层扩展开发说明书。
- **v1.1.0** *(2026-08)*
  - ⚡ **开源双引擎 PIMPL 隔离**：集成微软 ONNX Runtime 与 llama.cpp (GGUF)，支持 Qwen 等大模型双引擎热插拔比对。
  - 🛡️ **定长 DMA 硬件批调度器 (`FixedBatchExecutor`)**：实现 1-to-N 自动分块、Dummy Pad 自动补齐/剥离与 `(req_id, sub_id)` 样本溯源。
- **v1.0.0** *(2026-08)*
  - 🚀 **LLM-EdgeFlow 初始发布**：建立标准 6 大 C ABI 纯 C 接口屏障、`AlgContext` 动态类型安全黑板与 DAG 算子反射机制。

---

## 📄 License & Standards

- **Language**: C++17 (Strictly formatted via `.clang-format`)
- **Test Framework**: Google Test (GTest v1.14.0 via CMake FetchContent)
- **License**: [MIT License](LICENSE)
