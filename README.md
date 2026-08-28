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
- 📊 **图形化算法方案闭环**：C++ Definition/Catalog 驱动统一 CLI 与 **交互式 Web DAG 工作台 (`./show --web`)**，支持方案创建、结构编辑、静态校验、原子保存及隔离的真实 Demo 草稿运行。

---

## 🏛️ 4 层分层架构 (Architecture)

| 分层 | 核心职责 | 核心组件 / 文件 |
| :--- | :--- | :--- |
| **Layer 1: C ABI 适配层** | 客户端解包、类型强转与 `noexcept` 异常屏障 | `include/company_alg_interface.h`<br>`src/adapter/company_c_adapter.cpp` |
| **Layer 2: 管线与黑板层** | DAG 拓扑执行、请求级动态黑板与模型容器 | `Pipeline`, `AlgContext`, `SessionContext`, `TraceableItem<T>` |
| **Layer 3: 通用能力算子池** | 前处理分片、模型调用封装、规则快筛与结构化解析 | `INode`, `NodeBase`, `src/common_nodes/*` (11类通用算子) |
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

| 业务类型 / Enum | 业务名称 | 输入模态 | 挂载模型 / 算法能力 | 输出模态 | 配置文件 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Biz 1 (`ALG_BIZ_TYPE_DOC_QA`)** | **智能长文档问答** | 长文本+提问 | Embedding + 1.5B LLM | 切片数+意图+生成回答 | [`pipeline_doc_qa.json`](configs/pipeline_doc_qa.json) |
| **Biz 2 (`ALG_BIZ_TYPE_KEYWORD_MATCH`)** | **关注词规则匹配** | 文本句子 | 纯规则树快筛 (零模型) | 命中的类别与词 JSON | [`pipeline_keyword_match.json`](configs/pipeline_keyword_match.json) |
| **Biz 3 (`ALG_BIZ_TYPE_ENTITY_EXTRACT`)** | **实体名词抽取** | 文本句子 | 0.6B Qwen / llama.cpp | 抽取的名词列表 JSON | [`pipeline_entity_extract.json`](configs/pipeline_entity_extract.json) |
| **Biz 4 (`ALG_BIZ_TYPE_COMPLIANCE_AUDIT`)** | **对话风控质检** | 对话流+渠道 | Embedding + Rerank + 7B LLM | 风险分+等级+质检判决 | [`pipeline_dialogue_audit.json`](configs/pipeline_dialogue_audit.json) |
| **Biz 5 (`ALG_BIZ_TYPE_OCR_DOC_QA`)** | **多模态发票抽取** | 图像路径+Query | OCR 框检测识别 + LLM | 识别框数+结构化发票 JSON | [`pipeline_ocr_doc_qa.json`](configs/pipeline_ocr_doc_qa.json) |
| **Biz 6 (`ALG_BIZ_TYPE_AUDIO_ASR_INTENT`)** | **语音识别与槽位抽取** | 原始音频 PCM | Speech ASR + NLU 槽位提取器 | 转写文本+意图槽位 JSON | [`pipeline_audio_asr_intent.json`](configs/pipeline_audio_asr_intent.json) |
| **Biz 7 (`ALG_BIZ_TYPE_CROSS_RERANK`)** | **纯语义精排矩阵打分** | 1 Query + N 候选 | ONNX Cross-Encoder 矩阵计算 | Top-K 打分与排序索引 | [`pipeline_cross_rerank.json`](configs/pipeline_cross_rerank.json) |

---

## 🚀 快速开始 (Quick Start)

```bash
# 1. 编译工程 (Ninja + ccache 并行加速，自动拉取 GTest, ONNX Runtime 与 llama.cpp)
cmake -B build -G Ninja -DLLM_EDGEFLOW_USE_CCACHE=ON
cmake --build build -j$(nproc)

# 2. 并行自动化执行全量测试套件 (36 组 CTest & Google Test 并行执行)
ctest --test-dir build -j$(nproc) --output-on-failure

# 3. 运行 7 大业务端到端全链路集成演示 (默认 Smoke 组合)
./alg_demo

# 4. 基于 Profile 运行特定业务配置
./alg_demo --profile entity_extract_mock
./alg_demo --profile doc_qa_onnx

# 5. 查看所有支持的业务与 Profile 清单
./alg_demo --list

# 6. 批量自动化执行全部 Demo 套件 (Smoke / Real / All)
cd .. && ./scripts/run_all_demos.sh smoke
```

---

## 📊 DAG 可视化调试 (Visualizer)

```bash
# 1. 嵌入式/终端原生 ASCII 拓扑打印 (纯 C++ 零依赖)
./build/alg_show configs/pipeline_ocr_doc_qa.json

# 2. 打开方案列表或直接打开指定方案
./show --web
./show configs/pipeline_dialogue_audit.json --web

# 3. AI / 自动化使用同一个 C++ Catalog 与 Validator
./build/alg_pipeline_tool catalog --business smart_doc_qa_v1
./build/alg_pipeline_tool describe-node PromptBuilderNode
./build/alg_pipeline_tool validate configs/pipeline_doc_qa.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa.json
```

工作台是开发期工具，仅绑定 `127.0.0.1`，不设置账号或令牌鉴权。它只管理
`configs/pipeline_[a-z0-9_]+.json`，通过 SHA-256 revision 检测 IDE/Git 外部修改，
保存前调用 C++ Validator 并原子替换。节点坐标只存于浏览器 `localStorage`，不会污染
Pipeline JSON；未保存草稿可在隔离临时目录中调用真实 `alg_demo`，不会改写正式 `.conf`
或仓库 `results/`。

---

## 📁 代码目录布局 (Layout)

```text
LLM-EdgeFlow/
├── include/
│   ├── company_alg_interface.h  # Layer 1: 标准 C ABI 导出头文件
│   ├── core/                    # Layer 2: 框架核心 (AlgContext, Pipeline, TraceableItem)
│   ├── operator/                # Layer 1: Operator 门面接口 (operator_interface.h)
│   └── engine/                  # Layer 4: 引擎接口 (FixedBatchExecutor, IModelEngine)
├── src/
│   ├── adapter/                 # Layer 1: C ABI 与 Operator 安全胶水层
│   ├── core/                    # Layer 2: Pipeline 调度器与配置校验器实现
│   ├── biz/                     # Layer 3: 7 大多模态业务算子库 (前处理/后处理/业务编排)
│   ├── common_nodes/            # Layer 3: 通用跨业务算子 (LlmGenerateNode 等)
│   ├── engine/                  # Layer 4: 异构引擎实现 (mock_npu, onnx, llama_cpp)
│   └── tools/                   # C++ 工具集 (alg_show.cpp, alg_pipeline_tool.cpp)
├── doc/
│   ├── developer_guide.md       # 4 层扩展开发说明书
│   └── rfcs/                    # RFC 架构演进与治理文档 (RFC 0001 ~ 0011)
├── configs/                     # 11 大标准化业务配置 (JSON & .conf)
├── demo/                        # 参数化多业务端到端演示与 Runner
│   ├── profiles.json            # 预定义执行 Profile 清单 (单一事实源)
│   ├── common/                  # 通用参数解析、注册表、数据读取、结果落盘与 RAII Runner
│   └── biz/                     # 7 大独立业务 Demo 适配实现 (*_demo.cpp)
├── tests/                       # Google Test (GTest) 单元测试套件 (37 组测试)
├── scripts/                     # 自动化测试、Demo 调度与代码格式化工具
└── tools/visualizer/            # 交互式 Web DAG 可视化平台
```

---

## 📝 更新日志 (Changelog)

- **v4.1.0 (I/O 契约驱动的通用 Node 架构与编排重构 - RFC 0012)** *(2026-08)*
  - 🏛️ **I/O 契约驱动的通用 Node 架构**：彻底废弃按业务名或流水线阶段名绑定的 26 个特定算子，确立 `Node Family` (I/O 契约)、`Node Type` (单一操作语义 + 执行契约)、`Node Instance` (端口绑定 + 配置 + 模型) 三层解耦模型，实现全栈零重复代码。
  - 🧱 **11 个第一阶段通用 Common Nodes 落地**：落地 `TextTemplateNode`、`TextChunkNode`、`TextRuleMatchNode`、`StructuredJsonParseNode`、`TextEmbeddingNode`、`VectorTopKNode`、`TextRerankNode`、`LlmGenerateNode`、`AsrTranscribeNode`、`OcrDetectNode`、`TextCorpusSourceNode`。
  - 🔌 **显式逻辑端口绑定机制 (`BoundInput<T>` / `BoundOutput<T>`)**：在 Pipeline JSON 中通过 `ports: { "inputs": {...}, "outputs": {...} }` 将 Node 逻辑端口灵活映射至黑板物理键，支持同一类型算子在同一 Pipeline 中多次复用（例如 Query Embedding 与 Doc Embedding 独立复用同一算子）。
  - 🛡️ **单趟执行计划波前并发冲突校验 (`PipelineValidator`)**：在 DAG 拓扑分层时静态校验同一波前层中是否存在无 override 权限的重复输出键写入冲突，实现 Fail-Closed 静态安全防线。
  - 🧪 **37 组全量 Google Test 单元测试与 6 阶段质量门禁 100% 通过**：37 项 CTest 100% 通过，7 大业务端到端自动化测试、C11 ABI 合规检查与 CLI 双模验证全部通过。

- **v4.0.0 (Operator 与计算 Platform 概念与命名全栈解耦 - RFC 0011)** *(2026-08)*
  - 🏛️ **Operator 与 Platform 语义彻底解耦**：确立 `Integration -> Operator -> Pipeline -> Node -> Engine -> Platform` 全局术语链。`Operator` 代表对外暴露的算法实例生命周期与 Named I/O 契约，`Platform`（`ComputePlatform`）代表底层硬件计算平台（AX650、Ascend、RK3588、CUDA、CPU）。
  - 🗂️ **全栈目录结构与头文件规整**：头文件由 `include/platform/` 迁移至 `include/operator/`（`operator_interface.h`、`company_operator_types.h`），适配层由 `src/adapter/platform/` 迁移至 `src/adapter/operator/`，`src/adapter/platform_operator_adapter.cpp` 迁移至 `src/adapter/operator/operator_adapter.cpp`。
  - 🏷️ **C++ 命名空间与类型升级**：命名空间统一为 `llm_edgeflow::operator_api`，类型与注册表重构为 `OperatorValueTypeRegistry`、`OperatorBizBridgeRegistry`、`OperatorControlRegistry`、`OperatorOutputPool`、`CompanyOperator*`，硬件枚举命名为 `ComputePlatform`（`kAx650`, `kAscend310P`, `kAscend910B`, `kRk3588`, `kCuda`, `kCpu`）。
  - 🔌 **统一 Demo 与测试套件**：Demo CLI 参数 `--chip` 校验底层硬件执行平台，Runner 统一调用 `RunOperatorWithExtractor`，测试套件全面迁移至 `test_operator_api`、`test_operator_output_pool`、`test_operator_value_registry`、`test_operator_biz_bridge_registry`。
  - 🧪 **37 组全量 Google Test 单元测试与 6 阶段质量门禁 100% 通过**：37 项 CTest 全绿通过，C11 ABI 合规检查、LayerGuard 分层防腐、架构图源校验与 Demo 全链路烟测全部通过。

- **v3.1.0 (全栈 business 命名收敛与缩写统合为 biz - RFC 0010)** *(2026-08)*
  - 🗂️ **全栈目录结构与源码路径统一为 `biz`**：目录 `src/business/` $\rightarrow$ `src/biz/`，`demo/businesses/` $\rightarrow$ `demo/biz/`，`src/adapter/platform/business_bridges/` $\rightarrow$ `src/adapter/platform/biz_bridges/`，消除了冗余词根。
  - 🏛️ **C++ 核心接口与类名全面精简**：`IBusinessAdapter` $\rightarrow$ `IBizAdapter`，`BusinessAdapterRegistry` $\rightarrow$ `BizAdapterRegistry`，`PlatformBusinessBridgeRegistry` $\rightarrow$ `PlatformBizBridgeRegistry`，`PlatformBusinessSlot` $\rightarrow$ `PlatformBizSlot`，`BusinessDefinition` $\rightarrow$ `BizDefinition`，`PipelineCatalog::FindBusiness` $\rightarrow$ `FindBiz`，同时保留类型别名保证 100% 向后兼容。
  - 📄 **JSON 配置与 Pipeline 架构字段收敛**：配置文件字段由 `"business_name"` 统一为主格式 `"biz_name"`（内置兼容 `"business_name"`），节点定义分类由 `"business"` 统合为 `"biz"`，`NodeDefinition.biz_names` 与各契约全局常量 `k*BizName` 全面规范化。
  - 🛠️ **CLI 与工具链升级**：`alg_pipeline_tool`、`alg_show`、`scripts/show.py`、`demo/profiles.json` 支持 `--biz` 参数与 `"biz_name"` 属性。
  - 🧪 **37 组全量 Google Test 单元测试与 6 阶段质量门禁 100% 通过**：全量 CTest (37/37)、LayerGuard 分层防腐门禁、Google C++ 规范与 7 大业务端到端自动化测试全部无缝通过。

- **v2.1.0 (架构契约收敛与文档一致性修复 - RFC 0008)** *(2026-08)*
  - 🧭 **SSOT 契约注册与自发现体系**：引入 `REGISTER_NODE_WITH_DEFINITION` 与 `REGISTER_ENGINE_WITH_DEFINITION` 宏，统一将 Node / Engine 元数据定义在实现文件中声明并自动注册至 `PipelineCatalog`，彻底废除中心式硬编码 Catalog。
  - 🧠 **强类型黑板契约 (`BlackboardKey<T>`)**：重构 `AlgContext` 支持 `BlackboardKey<T>`，提供编译期类型安全、常量端口名绑定与零字符串拼写风险。
  - ⚡ **单趟执行计划 (`ValidatedPipelinePlan`)**：重构 `PipelineValidator::ValidateAndPlan` 一次性完成 JSON 解析、白名单校验、DAG 拓扑排序与波前分层，`Pipeline::Build` 直接消费不可变执行计划，彻底消除二次重复解析、DAG 排序与私有扩展诊断过滤代码。
  - 🏛️ **Layer 3 算子浅基类体系 (`NodeBase`)**：建立 `NodeBase`（提供 `final noexcept` 异常防火墙、上下文校验与 `Require/Publish/Fail` helper）、`ModelBoundNode<E>`（单模型绑定与访问）与 `TraceableUnaryInferenceNode<E, In, Out>`（单批次推理模板），全库 27 个生产算子全部完成基类迁移。
  - 🗂️ **算子目录归属规范化**：`LlmGenerateNode` 规整至 `common_nodes`（category `"common"`），`PromptBuilderNode`、`VectorSearchNode`、`RerankRefineNode` 规整至 `business/doc_qa`；`DenseRetrievalNode` 预编码政策库并实现真实余弦相似度检索召回。
  - 🔒 **统一验证与文档交付门禁**：Core、Pipeline、CLI、Web 与 Runtime 共用 9 类非法配置 fixture；架构 SVG 由固定 PlantUML 版本、Jar 校验和及源 SHA provenance 生成，源/资产漂移与损坏资产均 fail-closed；Sanitizer fast 使用独立 ASan/UBSan 构建和 emulator-only 路径。
  - 🧪 **33 组 CTest 单元测试矩阵与全量回归**：新增 Validated Plan、Node Base、Definition Schema、文档漂移与架构图生成等专项套件，33 项 CTest、11 个官方 Pipeline Validate/Plan、6 阶段自动化回归及 ASan/UBSan fast 全部通过。
- **v2.0.0 (SSOT 契约注册与强类型黑板交付 - RFC 0008 阶段一)** *(2026-08)*
  - 🏛️ **注册即声明（Self-Describing Registration SSOT）**：算子与引擎注册宏升级为 `REGISTER_NODE_WITH_DEFINITION` 与 `REGISTER_ENGINE_WITH_DEFINITION`，节点元数据（`inputs`, `outputs`, `config_fields`, `model_capability`, `model_config_field`, `parallel_safe`, `business_names`）随同定义自包含声明并自动汇入 `PipelineCatalog`，彻底消除外部硬编码 Catalog。
  - 🧠 **强类型动态黑板契约 (`BlackboardKey<T>`)**：统一黑板 Typed Key 声明规范，提供编译期类型安全与运行时动态类型检查，避免无类型裸字符串与类型不匹配隐患；`TraceableItem<T>` 支持批处理样本多源追溯与硬件定长解构。
  - 🔌 **业务适配器单一事实源绑定 (`IBusinessAdapter`)**：各业务 Adapter 实现 `GetDescriptor()` 自包含声明业务输入输出 C 结构体契约、批大小上限、Ingress/Egress 端口定义与合法 Pipeline 名称，并在注册期由 `BusinessAdapterRegistry` 自动挂载至 `PipelineCatalog`。
  - ✅ **集中式 PipelineValidator 校验收敛**：统一由 `PipelineValidator::Validate` 进行无副作用预检，`Pipeline::Build` 共享完全一致的节点/引擎依赖校验、黑板数据流匹配、模型能力匹配及 Kahn 拓扑排序逻辑。
  - 🧪 **25 组 CTest 单元测试矩阵与全量回归**：新增 `test_catalog_contract_ssot` 与 `test_typed_blackboard_contracts` 专项单测，25 项 Google Test 单元测试与 6 阶段自动化回归 100% 通过。
- **v1.6.0 (全库 Pipeline 显式 DAG 标准化与旧式配置维护解耦 - RFC 0007)** *(2026-08)*
  - 📐 **全量官方方案显式 DAG 升级**：`configs/` 目录下全部 11 个官方预置方案全面标准化为显式 DAG 格式（`id` + `depends_on`），消除隐式顺序数组的历史遗留形态，与底层 Kahn 拓扑排序及 Wavefront 调度严格契合。
  - 🔒 **严格显式 DAG 强类型约束**：彻底清除 C++ 底层对隐式顺序配置的兼容分支与 bypass 逻辑，强制要求所有节点显式提供 `id` 与 `depends_on`，统一使用 Kahn 拓扑排序，杜绝双轨维护与隐式 ID 合成债务。
  - 🎨 **Web Studio 零弹窗平滑拓扑编辑**：优化 `tools/visualizer/app.js` 交互逻辑，官方方案开箱即显式 DAG，简化节点与连线操作，彻底移除阻塞式确认弹窗。
  - 🧪 **全链路 6 阶段质量门禁回归**：更新 `run_all_tests.sh` 覆盖全量 11 个方案的 CLI 双模测试，23 项 Google Test 单元测试与 6 阶段自动化测试套件 100% 通过。
- **v1.5.1 (全面审查证据与质量门禁修正)** *(2026-08)*
  - 🧪 **可复现 Sanitizer**：ASan/UBSan 改用独立构建目录，支持显式 sanitizer 集合，禁用用户级 ccache，并动态执行全部 CTest 与 Smoke Profile。
  - 📐 **真实格式门禁**：`format.sh --check`、CI 和六阶段回归改为只读 clang-format 检查，并统一固定 clang-format 18.x，格式偏差与工具版本漂移不再被 `git diff --check` 漏掉。
  - 🔒 **C ABI 布局证据**：业务枚举通过 `INT32_MAX` guard 与纯 C11 编译期断言锁定 32 位 ABI。
  - 📋 **审查结论校正**：按最新主干的 23 项 CTest 和 27 个节点实现重新冻结证据，将 ASan、TSan、真实硬件等缺失证据如实标为 `NOT VERIFIED`。
- **v3.0.0 (平台 C 结构体槽位绑定与输出内存池 - RFC 0009)** *(2026-08)*
  - 🏛️ **纯 C11 平台值类型镜像定义 (`include/platform/company_platform_types.h`)**：定义 `CompanyString`（长度+显式指针）、`CompanyBuffer`、`CompanyAny`、`CompanyFrame`、`CompanyOdOutput` 及 7 大业务聚合平台镜像结构体，彻底替代裸定长 char 数组，实现零冗余内存开销并支持任意合法 UTF-8 字符串与二进制数据。
  - 🔌 **平台 Operator 接口 ABI v3 升级 (`include/platform/platform_operator_interface.h`)**：`CreateParam` 升级支持 `model_path` + `cfg_file_name` 双路径解耦与目录穿越/逃逸安全校验；移除废弃的 `depth_num` 与裸函数指针内存分配器；导出 `ValidatePlatformConfigBinding` 双路径公开预检 API 与 `MakeBorrowedPlatformInput` 零拷贝借用助手；`SOVERSION` 升级为 3。
  - 🏷️ **平台值类型注册中心与业务桥接中心 (`PlatformValueTypeRegistry` & `PlatformBusinessBridgeRegistry`)**：构建 17 种标准类型规范后缀（`string`, `buffer`, `frame`, `od_out`, `*_in`, `*_out` 等）的单一事实源；7 个业务桥接器模块化独立拆分，统一管理多槽位逻辑名映射、类型校验与局部影子 DTO 内存池。
  - 🏊 **会话级预分配输出内存池与弱引用安全 Deleter (`PlatformOutputPool`)**：基于 `data.mem_que` 配置在 `Platform_Create` 预分配固定深度输出内存块；`Platform_Process` 租用块并在成功时交付包装 `OutputPoolDeleter` 的 `std::shared_ptr<void>`；内存池耗尽时自适应阻塞与条件变量唤醒；句柄释放时安全 Drain，违规提前销毁返回 `-1` 并通过弱引用保障下游安全无野指针。
  - 🧪 **全量 Google Test 单元测试与端到端回归**：更新 Platform Operator、输出池、值类型与业务桥接测试，并覆盖 C11 ABI、Demo 和脚本生成器隔离，37 组 CTest 100% 通过，6 阶段回归门禁 100% 通过。
- **v1.5.0 (图形化算法方案工作台与配置编排闭环 - RFC 0006)** *(2026-08)*
  - 🧭 **运行时 Catalog/Definition**：节点与引擎构造器同步注册 Definition，导出业务 Adapter ingress/egress、类型化端口、配置约束、模型能力与并行安全信息；新增节点可自动出现在 CLI 和 Web。
  - ✅ **统一静态 Validator 与 CLI**：`Pipeline::Build` 在模型加载前复用无副作用预检；新增 `alg_pipeline_tool` 的 `catalog`、`describe-node`、`init`、`normalize`、`validate`、`plan` 六类版本化 JSON 命令。
  - 🎛️ **可编辑 SVG 工作台**：原生 ES Modules 实现打开/新建/保存/另存、节点增删拖动、拉线删线、自动布局、参数表单、Graph/JSON 双向同步、诊断定位与 Profile 草稿运行。
  - 🔒 **本机开发服务**：仅绑定回环地址且不引入账号/令牌权限；限制方案文件名和真实路径，revision 防覆盖、临时文件原子保存，并提供单任务异步运行、取消、超时和 2 MiB 日志上限。
  - 🤖 **技能闭环**：`pipeline-composer` 改为 Catalog→describe→compose→validate→Smoke 的短流程；Developer Guide 改为四层按需引用路由，两个技能均通过 `quick_validate.py`。
  - 🧪 **工作台回归矩阵**：新增 C++ Catalog/Validator/normalize 测试和 Python 文件/API/CLI/真实 Demo 闭环测试，全部既有 Pipeline 纳入统一静态校验。
- **v1.4.0 (参数化业务 Demo Runner 与执行配置解耦交付 - RFC 0005)** *(2026-08)*
  - 🧩 **独立业务 Demo 模块化解耦 (`demo/businesses/`)**：将原 `demo/main.cpp` 中单体硬编码的 7 大业务逻辑拆分为独立的 `*_demo.cpp` 文件，通过 `DemoRegistry` 与 `REGISTER_DEMO_BUSINESS` 宏实现字符串解耦与动态业务注册。
  - 📋 **Profile 声明式配置清单 (`demo/profiles.json`)**：引入统一 Profile Schema 作为 Demo 默认参数的唯一事实源，支持 CLI 命令行参数、Profile 配置与安全默认值的无缝优先级合并覆盖。
  - 🛡️ **通用 Platform Operator RAII 执行器 (`operator_runner.h`)**：统一收敛平台生命周期 `Init/Create/Process/Destroy/Deinit`，内置预检快速失败、强类型芯片白名单校验（`ChipType`）、精准耗时统计与 RAII 句柄安全释放。
  - 💾 **结构化结果落盘契约 (`results/<profile>/`)**：标准落盘 `results.jsonl`（包含 `request_id`、`status`、`latency_ms` 与业务 JSON）与 `summary.json`，支持临时文件原子写入（`.tmp` $\rightarrow$ 重命名）及 `--append` 追加模式。
  - 📜 **批量自动化 Demo 调度脚本 (`scripts/run_all_demos.sh`)**：提供统一批量调度入口，支持 `smoke`（默认轻量套件）、`real` 与 `all` 组合，并与 6 阶段自动化测试套件深度整合。
  - 🧪 **DemoRunnerTest 全量测试套件**：新增 `tests/test_demo_runner.cpp` 单元测试，全面覆盖 CLI 参数解析、芯片白名单、Profile 校验与冲突、数据读取、结果原子落盘及 7 大业务端到端集成，21 组 CTest 100% 通过。
- **v1.3.0 (平台 Operator 兼容门面与命名 I/O 双轨交付 - RFC 0004)** *(2026-08)*
  - 🔌 **平台 OperatorFunc 函数表兼容门面**：新增独立 C++ 平台头文件 `include/platform/platform_operator_interface.h`，导出 `Get_LLM_EDGEFLOW_OperatorTable()`，为公司调度平台提供 `Init` / `Create` / `Process` / `Control` / `Destroy` / `Deinit` 全 `noexcept` 隔离函数表。
  - 🧱 **Layer 1 共享算法运行时 (`SharedAlgorithmRuntime`)**：下沉提炼共享执行引擎，实现单点 `ValidateBatch` $\rightarrow$ `Unpack` $\rightarrow$ `Pipeline::Execute` $\rightarrow$ `Pack` 数据流，纯 C ABI 与 C++ 平台 Operator 门面双轨共用底层 Runtime，杜绝双轨逻辑分裂。
  - 📄 **平台部署配置解析器 (`CompanyConfResolver`)**：解析公司平台 `.conf` 部署配置，支持基于 `.conf` 目录相对路径规范化，实现 Pipeline JSON 模型路径内存动态重写与单/多模型映射覆盖，保持 Pipeline JSON 严格白名单与零磁盘临时文件。
  - 🏷️ **命名 I/O 派发与零拷贝提取 (`PlatformIoRegistry`)**：定义基于点后缀（`namespace.type_suffix`）的槽位派发体系（如 `camera_0.frame` $\rightarrow$ `frame`，`detector.od_out` $\rightarrow$ `od_out`），通过 `std::shared_ptr<void>::get()` 零拷贝借用外部指针转换为 C 指针数组。
  - 🔄 **强类型动态控制映射 (`PlatformControlRegistry`)**：将平台 `ControlCommand` 枚举与参数结构体强类型校验后无缝接入 `Pipeline::Control`。
  - 🧪 **全量回归与测试矩阵**：新增 `test_platform_operator` 全场景测试套件（覆盖 11 类生命周期、命名 I/O 异常、多模型覆盖、全业务执行与并发互斥），Demo 改为平台标准 `OperatorFunc` 驱动，18 组 CTest 100% 通过。
- **v1.2.0 (Pipeline 严格解析、Fail-Closed 注册与结构化诊断交付 - RFC 0003)** *(2026-08)*
  - 📐 **严格配置解析与白名单校验 (PLB-004, R1-ACC-003/006)**：实现集中式强类型解析器 `ParsePipelineConfig`，严格拒绝根节点/Model/Node 未知字段；废除顶层未知参数自动合并进算子配置的隐式行为；校验 `comment` 强类型与 `execution_mode: sequential` + `max_parallel_workers` 排他组合。
  - 🛡️ **注册中心重复拒绝与 Fail-Closed 防护 (PLB-006, RECHECK-R1-002)**：`NodeFactory` 与 `EngineFactory` 在检测到同名类型重复注册时，无条件保留首个注册实例并标记 `has_conflict_ = true`；Pipeline 构建阶段实施 Fail-Closed 拦截；移除生产头文件中的重置调试接口，确保进程级注册冲突不可篡改。
  - 🩺 **精细化结构诊断与异常阶段隔离 (PLB-011, R1-ACC-001, RECHECK-R1-001/003)**：定义 21 种细粒度 `PipelineErrorCode`，涵盖配置无法打开、语法解析、引擎/算子创建与初始化异常；物化边界建立全生命周期异常拦截网络并映射至标准 JSON Pointer path，杜绝异常逃逸；引入 `BuildingStateGuard` 保证构建异常确定性收敛至 `State::kFailed`。
  - 🔄 **一次性构建生命周期状态机保护 (R1-ACC-002)**：`Pipeline` 引入 `State::kEmpty -> State::kBuilding -> State::kReady / State::kFailed` 显式状态机，彻底封禁重复 Build 与脏状态复用；`Execute` 与 `Control` 仅在 `Ready` 状态工作，未就绪或失败实例安全拦截。
  - 🔒 **Registry 锁粒度优化与自锁消除 (R1-ACC-004, RECHECK-R1-003)**：`Create` 调整为锁内检索复制函数对象、锁外执行构造，彻底消除算子构造期重入 Registry 导致的死锁风险；新增带超时保护的进程级独立测试套件。
  - 🧪 **18 组全量 CTest 单元测试矩阵与 6 阶段全量回归**：新增 `test_pipeline_config`、`test_registry_conflict`、`test_registry_reentrant` 测试套件，全量表驱动断言解析预检零副作用与物化异常，全部 18 组 CTest 及 6 阶段端到端回归 100% 通过。
- **v1.1.0 (Adapter 契约与内存安全加固交付 - RFC 0002)** *(2026-08)*
  - 🛡️ **输出截断防御与容量保护 (RECHECK-001, ADP-005)**：`AdapterValidationHelper::CheckedStringCopy` 在缓冲区容量不足时自动记录结构化诊断并返回 `false`；全部生产 Adapter 及模板严密校验返回值，遇截断即时返回标准错误码 `COMPANY_ALG_ERR_BUFFER_TOO_SMALL` (`-4`)，彻底杜绝静默截断伪成功。
  - 🔒 **Pipeline 精确白名单与 Fail-Closed 绑定 (RECHECK-002, ADP-006)**：`AdapterDescriptor` 引入 `allowed_pipeline_names` 显式白名单列表；基类 `ValidatePipelineBinding` 实施 Fail-Closed 默认防御，彻底废弃名称模糊子串推断，杜绝恶意或误配置 Pipeline 绑定。
  - 🚫 **未实现策略注册拦截 (RECHECK-003, ADP-008)**：`BusinessAdapterRegistry` 在注册期严格校验 `ownership_policy`、`thread_model` 和 `cardinality`，非当前支持组合（`kCopyIn + kStatelessThreadSafe + kOneToOne`）直接拒绝注册并标记冲突。
  - 🩺 **结构化诊断贯通与有界扫描 (RECHECK-004, ADP-001)**：`IBusinessAdapter::Unpack/Pack` 全面引入 `AdapterStatus` 诊断上下文，`Alg_Process` 实时捕获并输出精准的 `field_path`、`sample_index` 和错误码；引入 `RequireBoundedString`（`strnlen`）与 `CheckedMultiply` 数组总字节上限约束。
  - 🧩 **4 套独立可编译模板事实源 (RECHECK-005, ADP-010)**：在 `include/adapter/templates/` 下发布 Flat Struct、Tagged Union、Nested Dynamic Array、Nested Pointer Tree 4 套现代 C++ 模板头文件并纳入编译器与 CI 门禁，开发指南与技能文件签名 100% 同步。
  - 🧪 **复杂结构安全契约测试与 Sanitizer 门禁 (RECHECK-006, ADP-011)**：新增 9 组深度安全与生命周期单元测试（涵盖直接外部内存篡改隔离、递归指针树深度熔断、跨线程并发），并提供 `scripts/run_sanitizers.sh` 在 ASan/UBSan 模式下通过全量内存安全验证。
- **v1.0.0 (LLM-EdgeFlow 初始稳定发布 - RFC 0001)** *(2026-08)*
  - 🛡️ **标准纯 C ABI 隔离与安全屏障**：`include/company_alg_interface.h` 采用纯 C 指针数组批处理接口，全量 6 大导出函数严格保证 `COMPANY_ALG_NOEXCEPT` 异常全拦截；提供 C11 原生编译器验证套件 `test_c11_abi_compliance`。
  - 🧩 **独立业务适配器体系**：引入 `BusinessAdapterRegistry` 与 `AdapterValidationHelper` 统一各业务适配器的批输入与输出缓冲区契约。
  - ⚡ **异构推理引擎 PIMPL 隔离**：原生集成微软 ONNX Runtime、llama.cpp (GGUF) 与 MockNPU 引擎，切换芯片零改动业务代码。
  - 🎯 **定长 DMA 硬件批调度器 (`FixedBatchExecutor`)**：实现自动分块、Dummy Pad 补齐/剥离与 `(req_id, sub_id)` 样本溯源。
  - 🧠 **动态类型安全黑板 (`AlgContext`)**：基于 `std::shared_mutex` 支持读读并发、写写互斥。
  - 📐 **原生 DAG 拓扑调度与 7 大多模态业务全覆盖**：Kahn 算法拓扑波前分层调度 (`execution_mode: "parallel"`)，原生支持问答、抽取、关注词、风控、OCR、语音 ASR 与语义精排 7 大业务。

---

## 📄 License & Standards

- **Language**: C++17 (Strictly formatted via `.clang-format`)
- **Test Framework**: Google Test (GTest v1.14.0 via CMake FetchContent)
- **License**: [MIT License](LICENSE)
