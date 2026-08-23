# RFC 0005: 参数化业务 Demo Runner 与执行配置解耦

- **RFC 编号**：0005-parameterized-business-demo-runner
- **创建日期**：2026-08-23
- **文档状态**：Completed
- **关联分支**：`feat/parameterized-business-demo-runner`
- **目标版本**：v2.4.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机 (Motivation & Context)

当前 `demo/main.cpp` 同时承担以下职责：

- 命令行参数解析；
- 7 种业务的测试集读取与解析；
- 业务 C 输入/输出结构体组装；
- 命名 I/O 槽位绑定；
- Platform Operator 的 `Init/Create/Process/Destroy/DeInit` 生命周期管理；
- Control 参数下发；
- 终端结果打印；
- `--biz 1..7` 数字分发以及默认全业务串行执行。

这种实现能够用于早期功能展示，但随着业务类型、模型实现和 Pipeline 变体增加，逐渐暴露出以下问题：

1. 新增业务必须继续修改同一个 `main.cpp` 和数字 `switch`，容易产生冲突。
2. 数字业务 ID 缺乏可读性，CLI 使用者需要额外查询映射关系。
3. “业务类型”和“运行方式”混在一起。同一业务可能同时存在 Mock、ONNX Runtime、llama.cpp 或真实硬件等多个运行配置。
4. 测试集默认路径同时硬编码在业务函数和主分发逻辑中，存在重复配置。
5. 测试集不存在时会静默使用内置样例，不利于 CI 和交付环境发现路径配置错误。
6. 结果只打印到终端，无法形成稳定、可追踪、便于自动比较的结果文件。
7. 全业务执行顺序写死在 C++ 中，不利于区分轻量 Smoke 测试与依赖真实模型的运行方式。

本 RFC 计划将 Demo 重构为：

> 一个业务一个 Demo 实现文件，一个通用参数化 Runner，一个 Profile 配置清单，以及一个批量执行脚本。

设计遵循配置优先原则：Pipeline 算法流程继续由 `configs/*.json` 组合，Demo 代码只承担外部测试数据与既有业务 ABI 结构之间的编解码，不在 Demo 中复制 Pipeline 或业务算法实现。

---

## 2. 目标、范围与非目标 (Goals, Scope & Non-Goals)

### 2.1 设计目标

- [ ] 将每个业务的数据解析、ABI 结构体组装和结果序列化拆分到独立文件。
- [ ] 提供统一的 Platform Operator 生命周期执行器。
- [ ] 使用可读字符串替代 Demo CLI 的 `--biz 1..7` 数字选择方式。
- [ ] 使用 Profile 表达同一业务的不同模型、Pipeline 和数据集组合。
- [ ] 支持命令行覆盖配置文件、测试集和结果目录。
- [ ] 默认将结果保存到 `./results/`，同时保留必要的终端摘要。
- [ ] 提供统一脚本执行 Smoke、Real 和 All 三类 Demo 套件。
- [ ] 保持 Platform Operator、C ABI、Pipeline、Node 和 Engine 的现有契约不变。

### 2.2 范围内 (In-Scope)

- `demo/` 目录重构；
- Demo CLI 和 Profile Schema；
- Demo 业务注册表；
- 通用数据读取、路径解析和结果写入；
- `scripts/run_all_demos.sh`；
- CMake Demo 目标调整；
- Demo 参数、Profile、业务注册和结果输出测试；
- `scripts/run_all_tests.sh` 中端到端 Demo 阶段的调用更新；
- README 使用方式更新。

### 2.3 非目标 (Non-Goals / Out-of-Scope)

- 不修改 `CompanyAlgBizType`、`ChipType` 或 `ControlCommand` 的核心枚举契约。
- 不把业务 C ABI 输入/输出结构体改成通用 JSON ABI。
- 不修改 `OperatorFunc`、`NamedIoBatch` 或 Platform Operator 生命周期协议。
- 不要求所有业务测试集立即统一为同一种文件格式。
- 不在 Demo 层实现新的 Pipeline Node、推理引擎或业务算法。
- 不把真实模型下载和部署逻辑放入通用 Demo Runner。
- 不将 shell 脚本作为配置文件的唯一事实源。

---

## 3. 术语定义 (Terminology)

### 3.1 Business

Business 表示稳定的业务输入输出契约，例如：

- `entity_extract`
- `keyword_match`
- `doc_qa`
- `dialogue_audit`
- `ocr_doc_qa`
- `audio_asr`
- `cross_rerank`

Business 决定使用哪一种公司业务 C 输入/输出结构体、如何解析测试集、绑定哪些命名 I/O 槽位以及如何序列化结果。

### 3.2 Profile

Profile 表示一次可直接运行的执行配置。多个 Profile 可以复用同一个 Business，例如：

- `entity_extract_mock`
- `entity_extract_llamacpp`
- `doc_qa_mock`
- `doc_qa_onnx`
- `doc_qa_rerank`
- `doc_qa_rerank_real`

Profile 负责声明默认 `.conf`、测试集、执行套件和必要的运行参数，不重新定义业务 ABI。

### 3.3 Runner

Runner 是与业务无关的通用执行层，负责参数解析、Profile 合并、Platform Operator 生命周期、错误处理、统计和结果文件管理。

### 3.4 Demo Case

Demo Case 是单个业务对应的薄适配实现，只负责：

1. 读取并校验本业务测试数据；
2. 维护字符串、PCM、图片路径等输入内存的生命周期；
3. 组装业务 C 输入和输出结构体；
4. 提供命名 I/O 槽位；
5. 调用通用 Runner；
6. 将业务结果转换成统一 JSONL 记录。

---

## 4. 总体架构 (Architecture)

```text
CLI 参数 ──────────────┐
                      │ 覆盖
demo/profiles.json ───┤
                      ▼
               DemoOptions
                      │
                      ▼
              DemoRegistry
                      │ business/profile
                      ▼
        businesses/*_demo.cpp
          │ 数据编解码与槽位声明
          ▼
          OperatorRunner
          │ Init/Create/Process/Destroy/DeInit
          ▼
       Platform Operator API
          │
          ▼
 results/<profile>/results.jsonl

scripts/run_all_demos.sh
          │ 只负责套件和顺序
          └──────────────> alg_demo --profile <name>
```

### 4.1 四层架构映射

本 RFC 属于四层架构之外的示例与集成工具重构，不改变任何核心依赖方向：

- **Layer 1**：仅通过现有 `OperatorFunc` 和公开 Platform 类型调用，不新增 ABI。
- **Layer 2**：不直接访问 `Pipeline`、`AlgContext` 或 `SessionContext`。
- **Layer 3**：不直接实例化业务 Node 或公共 Node。
- **Layer 4**：不直接访问模型 Engine 或 `FixedBatchExecutor`。

Demo 必须像真实下游调用方一样，只依赖公开接口，不绕过 Platform Operator 进入内部层级。

### 4.2 推荐目录结构

```text
demo/
├── main.cpp
├── profiles.json
├── common/
│   ├── dataset_reader.cpp
│   ├── dataset_reader.h
│   ├── demo_options.cpp
│   ├── demo_options.h
│   ├── demo_registry.cpp
│   ├── demo_registry.h
│   ├── operator_runner.h
│   ├── result_writer.cpp
│   └── result_writer.h
└── businesses/
    ├── audio_asr_demo.cpp
    ├── cross_rerank_demo.cpp
    ├── dialogue_audit_demo.cpp
    ├── doc_qa_demo.cpp
    ├── entity_extract_demo.cpp
    ├── keyword_match_demo.cpp
    └── ocr_doc_qa_demo.cpp

scripts/
└── run_all_demos.sh
```

`demo/main.cpp` 只保留以下职责：

1. 解析命令行；
2. 加载并合并 Profile；
3. 选择业务 Demo Case；
4. 返回统一进程退出码。

---

## 5. 核心接口设计 (Core Interfaces)

以下接口为设计草案，最终命名可以在实现评审时微调，但职责边界不得改变。

### 5.1 DemoOptions

```cpp
struct DemoOptions {
  std::string profile;
  std::string business;
  std::filesystem::path config_path;
  std::filesystem::path dataset_path;
  std::filesystem::path output_dir = "./results";

  int batch_size = 1;
  int device_id = 0;
  std::string chip = "ax650";
  uint32_t depth_num = 1;

  std::optional<std::filesystem::path> control_file;
  bool append = false;
  bool allow_fallback_sample = false;
};
```

`chip` 在 CLI 和 Profile 中使用可读字符串，在调用 Platform Operator 前通过严格白名单转换为 `ChipType`。未知字符串必须失败，不能映射为默认芯片。

### 5.2 Demo 注册接口

```cpp
using DemoRunFunction = int (*)(const DemoOptions& options);

struct DemoDescriptor {
  std::string business_name;
  DemoRunFunction run = nullptr;
};

class DemoRegistry {
 public:
  static DemoRegistry& Instance();
  bool Register(DemoDescriptor descriptor);
  const DemoDescriptor* Find(std::string_view business_name) const;
  std::vector<std::string> ListBusinessNames() const;
};
```

注册表必须拒绝空业务名、空函数和重复业务名。注册失败应在进程初始化阶段暴露，不允许后注册覆盖先注册。

### 5.3 通用 Operator Runner

```cpp
template <typename TInput, typename TOutput>
int RunPlatformOperator(const DemoOptions& options,
                        std::string_view input_slot,
                        std::string_view output_slot,
                        std::vector<TInput>& inputs,
                        std::vector<TOutput>* outputs,
                        const ControlRequest* control = nullptr);
```

Runner 负责：

- 校验配置路径、Batch、Device、Chip 和 Depth；
- 全局 `Init/DeInit` 的成对调用；
- 单次运行 `Create/Destroy` 的成对调用；
- 以借用型 `shared_ptr<void>` 构造 `NamedIoBatch`；
- 保证异常和提前返回路径仍能销毁句柄；
- 将 Platform 错误转换为统一 Demo 错误信息；
- 收集总耗时、样本数量、成功数和失败数。

业务 Demo 不得重复实现 Operator 生命周期。

### 5.4 业务 Demo Case

单业务文件可以导出一个运行函数：

```cpp
int RunEntityExtractDemo(const DemoOptions& options);
```

该函数内部负责：

```text
读取 entity_extract 测试集
        │
        ▼
创建字符串所有者与 CompanyEntityInputStruct
        │
        ▼
调用 RunPlatformOperator<TInput, TOutput>
        │
        ▼
将 CompanyEntityOutputStruct 转换为 JSONL
```

字符串或二进制缓冲区的所有者必须至少存活到 `Process` 返回，禁止把临时对象的 `c_str()` 或 `data()` 地址存入输入结构体。

---

## 6. CLI 设计 (Command-Line Interface)

### 6.1 推荐调用方式

Profile 模式：

```bash
./build/alg_demo --profile entity_extract_mock
./build/alg_demo --profile doc_qa_onnx
```

直接参数模式：

```bash
./build/alg_demo \
  --business entity_extract \
  --config configs/pipeline_entity_extract.conf \
  --dataset data/corpus_entity_extract.txt \
  --output-dir ./results
```

Profile 加局部覆盖：

```bash
./build/alg_demo \
  --profile entity_extract_mock \
  --dataset /tmp/custom_entity_cases.txt \
  --output-dir ./custom-results
```

### 6.2 参数清单

| 参数 | 必需性 | 默认值 | 说明 |
| :--- | :---: | :--- | :--- |
| `--profile <name>` | 条件必需 | 无 | 选择一个预定义运行 Profile |
| `--business <name>` | 条件必需 | Profile 值 | 选择业务 Demo Case，使用字符串而非数字 |
| `--config <path>` | 条件必需 | Profile 值 | Platform `.conf` 文件 |
| `--dataset <path>` | 条件必需 | Profile 值 | 本业务测试集；兼容旧参数别名 `--data` |
| `--output-dir <dir>` | 可选 | `./results` | 结果根目录 |
| `--batch-size <n>` | 可选 | Profile 值或 `1` | Platform 最大 Batch |
| `--device-id <n>` | 可选 | Profile 值或 `0` | 设备 ID |
| `--chip <name>` | 可选 | Profile 值或 `ax650` | 芯片字符串白名单 |
| `--depth <n>` | 可选 | Profile 值或 `1` | 输出对象预分配深度 |
| `--control-file <path>` | 可选 | Profile 值 | Control JSON 文件 |
| `--append` | 可选 | `false` | 追加而不是覆盖结果文件 |
| `--allow-fallback-sample` | 可选 | `false` | 测试集缺失时允许使用内置样例 |
| `--list` | 可选 | 无 | 列出可用 Business 和 Profile |
| `--help` | 可选 | 无 | 显示帮助 |

### 6.3 配置优先级

配置合并优先级固定为：

```text
命令行显式参数 > Profile 配置 > 程序安全默认值
```

任何路径在合并后仍为空或不存在，都必须在调用 `Init/Create` 前失败。

### 6.4 枚举边界

核心枚举继续保留：

- `CompanyAlgBizType` 用于业务 Adapter 与 I/O Registry 的稳定索引；
- `ChipType` 用于硬件白名单校验；
- `ControlCommand` 用于强类型控制协议。

本 RFC 只移除 Demo CLI 的数字 `--biz 1..7` 和中心化 `switch`。字符串参数必须在 Demo 边界转换并校验，不能向 SDK 内部扩散为任意字符串协议。

---

## 7. Profile 配置设计 (Profile Configuration)

`demo/profiles.json` 是 Demo 默认运行参数的唯一事实源：

```json
{
  "schema_version": 1,
  "profiles": {
    "entity_extract_mock": {
      "business": "entity_extract",
      "config": "configs/pipeline_entity_extract.conf",
      "dataset": "data/corpus_entity_extract.txt",
      "suite": "smoke",
      "batch_size": 1,
      "device_id": 0,
      "chip": "ax650",
      "depth": 1
    },
    "entity_extract_llamacpp": {
      "business": "entity_extract",
      "config": "configs/pipeline_entity_extract_llamacpp.conf",
      "dataset": "data/corpus_entity_extract.txt",
      "suite": "real",
      "batch_size": 1,
      "device_id": 0,
      "chip": "cpu_generic",
      "depth": 1
    }
  }
}
```

### 7.1 Profile 校验规则

- `schema_version` 必须是受支持版本；
- Profile 名不得为空且不得重复；
- `business` 必须已在 `DemoRegistry` 注册；
- `config` 和 `dataset` 必须是非空字符串；
- `suite` 仅允许 `smoke` 或 `real`；
- `batch_size` 和 `depth` 必须大于 0；
- `device_id` 必须大于等于 0；
- `chip` 必须属于显式白名单；
- CLI 选择的 Business 必须与 Profile Business 一致；
- `.conf` 最终解析出的 Pipeline Business 必须与 Demo Case 兼容，必须在 `Process` 前进行预检并快速失败。

### 7.2 Profile 与 Pipeline 配置的边界

- Profile 描述“如何运行 Demo”。
- `.conf` 描述平台部署入口和模型路径覆盖。
- Pipeline JSON 描述算法节点、依赖和模型绑定。

Profile 不得复制 Pipeline 节点配置、Prompt、模型列表或业务规则，避免形成第二份算法配置。

---

## 8. 测试集设计 (Dataset Design)

第一阶段继续兼容现有测试集：

- 单行文本：实体提取、关键词匹配；
- 标签段落：文档问答、对话审核、OCR、Cross Rerank；
- 音频：现阶段允许业务 Demo 生成受控 PCM，后续扩展为音频文件输入。

通用 `dataset_reader` 只提供基础能力：

- 非空非注释行读取；
- `[TAG]` 段落读取；
- 明确的文件不存在和格式错误诊断。

每个业务如何解释标签和字段仍由对应 `*_demo.cpp` 决定。本 RFC 不尝试用一个万能解析器描述所有模态。

默认行为必须由“文件不存在时使用内置样例”改为失败关闭。只有显式传入 `--allow-fallback-sample` 时，业务 Demo 才能使用内置示例。

---

## 9. 结果输出契约 (Result Contract)

### 9.1 目录约定

默认输出根目录为 `./results/`：

```text
results/
├── entity_extract_mock/
│   ├── results.jsonl
│   └── summary.json
├── keyword_match/
│   ├── results.jsonl
│   └── summary.json
└── doc_qa_onnx/
    ├── results.jsonl
    └── summary.json
```

`results/` 应加入 `.gitignore`，禁止提交运行产物。

### 9.2 JSONL 公共字段

每个输入样本输出一行 JSON：

```json
{
  "schema_version": 1,
  "profile": "entity_extract_mock",
  "business": "entity_extract",
  "request_id": 30001,
  "status": 0,
  "latency_ms": 1.25,
  "output": {
    "entities": []
  }
}
```

公共字段由 `ResultWriter` 负责，`output` 对象由业务 Demo 生成。错误样本必须记录非零 `status` 和 `error`，不得伪装成空成功结果。

### 9.3 写入策略

- 默认覆盖当前 Profile 的历史结果，保证 CI 可重复；
- 只有显式 `--append` 才允许追加；
- 创建目录和打开文件失败时返回非零退出码；
- 结果先写临时文件，成功完成后再替换目标文件，避免异常中断留下“完整结果”的假象；
- 终端只输出摘要和错误，完整业务结果以 JSONL 为准。

---

## 10. 批量执行脚本 (Batch Orchestration)

新增统一入口：

```bash
./scripts/run_all_demos.sh
./scripts/run_all_demos.sh smoke
./scripts/run_all_demos.sh real
./scripts/run_all_demos.sh all
```

规则如下：

- 无参数默认执行 `smoke`；
- `smoke` 只执行不依赖外部真实模型资产的 Profile；
- `real` 只执行依赖真实模型或真实硬件的 Profile；
- `all` 依次执行 `smoke` 和 `real`；
- 任意 Profile 失败时脚本立即返回非零；
- 脚本只声明 Profile 名和运行顺序，不重复声明配置、数据集、Batch、Chip 等默认值；
- `scripts/run_all_tests.sh` 的端到端阶段调用 `run_all_demos.sh smoke`。

真实模型缺失不应让默认 Smoke 套件失败；Real 套件应在运行前给出清晰的资产缺失诊断。

---

## 11. 生命周期、错误处理与资源安全

### 11.1 生命周期不变量

- 成功 `Init` 后必须执行一次 `DeInit`；
- 成功 `Create` 后必须执行一次 `Destroy`；
- `Process`、结果转换或文件写入失败时仍必须释放 Handle；
- Demo Runner 不拥有业务输入字符串指针所引用的数据，业务 Demo 必须保证其生命周期；
- 同一次运行不得在多个位置重复初始化或释放全局 Platform 环境。

建议以小型 RAII 类型封装全局环境和 Handle，避免手工早退遗漏。

### 11.2 退出码约定

| 退出码 | 含义 |
| :---: | :--- |
| `0` | 所有样本执行成功且结果完整落盘 |
| `2` | CLI 参数错误 |
| `3` | Profile 或配置校验错误 |
| `4` | 测试集读取或解析错误 |
| `5` | Platform Operator 生命周期或 Process 错误 |
| `6` | 结果目录或结果文件写入错误 |
| `7` | 业务结果转换错误 |

底层 Platform 错误码应保留在诊断文本和 `summary.json` 中，但进程出口统一映射到上述稳定分类。

---

## 12. 兼容性与迁移策略

### 12.1 可执行文件兼容

继续保留可执行文件名 `alg_demo`，避免破坏 README、CI 和下游脚本。

迁移期建议：

- `--data` 作为 `--dataset` 的兼容别名保留一个版本；
- `--biz <number>` 输出弃用警告，并映射到对应字符串业务，后续版本删除；
- 无参数执行暂时等价于默认 Smoke 套件，保持当前端到端回归行为；
- 新脚本和文档统一使用 `--profile`、`--business` 和 `--dataset`。

### 12.2 配置兼容

现有 `configs/*.conf`、`configs/*.json` 和 `data/corpus_*.txt` 不做破坏性迁移。第一阶段只增加 Profile 清单和独立业务 Demo 文件。

---

## 13. CMake 与构建组织

`alg_demo` 继续作为单一可执行程序，由显式源文件列表组成：

```cmake
set(ALG_DEMO_SRCS
    demo/main.cpp
    demo/common/demo_options.cpp
    demo/common/demo_registry.cpp
    demo/common/dataset_reader.cpp
    demo/common/result_writer.cpp
    demo/businesses/entity_extract_demo.cpp
    demo/businesses/keyword_match_demo.cpp
    demo/businesses/doc_qa_demo.cpp
    demo/businesses/dialogue_audit_demo.cpp
    demo/businesses/ocr_doc_qa_demo.cpp
    demo/businesses/audio_asr_demo.cpp
    demo/businesses/cross_rerank_demo.cpp)

add_executable(alg_demo ${ALG_DEMO_SRCS})
target_link_libraries(alg_demo PRIVATE alg_sdk)
```

不建议为 7 个业务复制 7 个 `main()` 和 7 套 CLI。若未来交付明确要求独立二进制，可在通用 Runner 上增加极薄的业务入口目标，而不复制业务实现。

---

## 14. 测试与质量验收计划 (Testing & Verification Plan)

### 14.1 单元测试

新增 `tests/test_demo_runner.cpp`，至少覆盖：

- CLI 必需参数、未知参数、整数范围和帮助输出；
- CLI、Profile 和默认值的优先级；
- Profile Schema 版本和字段类型校验；
- 重复业务注册和未知业务查找；
- Chip 字符串白名单转换；
- 配置 Business 与 Demo Business 不一致时快速失败；
- 测试集不存在时默认失败；
- `--allow-fallback-sample` 显式生效；
- 结果目录创建、默认覆盖、显式追加和写入失败；
- RAII 在 Create、Process 和结果写入失败时仍完成资源清理。

### 14.2 业务回归

7 个业务分别使用现有 Mock/Smoke Profile 执行，并验证：

- 进程退出码为 0；
- `results.jsonl` 行数与输入样本数一致；
- 每行 JSON 可解析且公共字段齐全；
- `request_id` 与输入对应；
- `summary.json` 的成功数、失败数和总数一致；
- 现有 Platform Operator 和全部业务 Pipeline 测试继续通过。

### 14.3 全量门禁

实现完成后必须执行：

```bash
./scripts/format.sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
./scripts/run_all_tests.sh
./scripts/run_all_demos.sh smoke
```

Real 套件作为具备模型资产和对应硬件环境时的附加验收，不应替代默认全量回归门禁。

---

## 15. 实施路线与里程碑 (Implementation Milestones)

### 阶段一：公共执行层抽取

- [x] 抽取 `DemoOptions` 和 CLI Parser；
- [x] 抽取 Dataset Reader；
- [x] 抽取 Platform Operator RAII Runner；
- [x] 保持当前 7 个业务行为不变。

### 阶段二：业务文件拆分

- [x] 将 7 个 `DemoXxx` 迁移为独立 `*_demo.cpp`；
- [x] 引入字符串业务注册表；
- [x] 删除 `main.cpp` 中的数字 `switch`；
- [x] 增加注册冲突和未知业务测试。

### 阶段三：Profile 与结果落盘

- [x] 增加 `demo/profiles.json`；
- [x] 实现 CLI 覆盖和严格校验；
- [x] 实现 JSONL 与 Summary 输出；
- [x] 将 `results/` 加入 `.gitignore`。

### 阶段四：脚本、回归与文档

- [x] 增加 `scripts/run_all_demos.sh`；
- [x] 更新 `scripts/run_all_tests.sh`；
- [x] 更新 README Demo 使用说明；
- [x] 执行 CTest 和全量六阶段回归；
- [x] 将 RFC 状态更新为 `Completed`。

---

## 16. 验收标准 (Acceptance Criteria)

满足以下全部条件后，本 RFC 才能标记为 `Completed`：

1. `demo/main.cpp` 不再包含 7 个业务的输入解析和结果打印实现。
2. 每个业务存在独立 `*_demo.cpp`，且业务之间没有相互依赖。
3. Demo CLI 不再依赖数字 `--biz 1..7` 进行正常分发。
4. 核心 SDK 枚举和 Platform Operator ABI 没有被字符串化或削弱。
5. 一个 Business 可以被多个 Profile 复用，无需复制业务 Demo 代码。
6. 配置、测试集和输出目录均可通过参数覆盖。
7. 默认结果稳定写入 `./results/<profile>/`。
8. 测试集缺失时默认失败，不再静默回退。
9. `run_all_demos.sh smoke` 可以执行全部默认轻量业务。
10. 全量 CTest 和 `scripts/run_all_tests.sh` 100% 通过。

---

## 17. 设计权衡与被否决方案 (Trade-offs & Rejected Alternatives)

### 17.1 为每个业务创建独立可执行程序

优点是运行入口物理隔离；缺点是会复制 `main()`、CLI 和 CMake 目标，并增加交付二进制数量。

本 RFC 选择“一个业务一个源文件、一个通用可执行程序”。如果未来平台交付明确要求独立二进制，再增加薄入口目标。

### 17.2 完全依赖配置文件推断业务

Platform Resolver 可以根据 Pipeline `business_name` 推断内部 `CompanyAlgBizType`，但 Demo 仍必须选择正确的外部 C 输入/输出结构体和测试集解析器。

因此 Profile 必须显式声明 `business`，同时与 `.conf` 解析结果交叉校验。不能仅依赖配置猜测并在 `Process` 阶段才发现槽位或类型错误。

### 17.3 删除核心枚举并全部改用字符串

字符串适合 CLI 和配置边界，但不适合替代内部稳定契约。完全字符串化会降低编译期约束并削弱注册表校验。

因此仅将用户输入参数化，核心枚举保持不变。

### 17.4 将全部默认配置写入 shell

这会使 C++、CI、README 和 shell 出现多套默认值。Profile 清单应作为唯一事实源，shell 只负责编排 Profile。

### 17.5 强制所有测试集统一格式

文本、文档问答、图片、音频和 Rerank 的数据形态差异明显，立即统一为万能格式会引入额外解析复杂度。

第一阶段保留现有格式和业务解析器，只统一路径参数、错误行为和结果输出契约。

---

## 18. 关联文档

- [RFC-0001：4 层架构隔离与统一分层抽象基线](0001-four-tier-architecture-foundation.md)
- [RFC-0002：C ABI Adapter 契约安全与内存防越界加固](0002-c-abi-adapter-security-hardening.md)
- [RFC-0004：平台 Operator 接口与命名 I/O 兼容层设计](0004-platform-operator-interface-compatibility.md)
- [Pipeline Composer Skill](../../.agents/skills/pipeline-composer/SKILL.md)

---

## 19. 变更记录 (Changelog)

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-08-23 | v0.1.0 | 创建参数化业务 Demo Runner 与执行配置解耦设计草案 | LLM-EdgeFlow Team |
