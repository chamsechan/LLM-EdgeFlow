# RFC-0005 参数化业务 Demo Runner 验收评审报告

## 1. 验收结论

- **初评结论（主干基线 18a2844）**：暂不通过，建议完成 3 项 P1 修复后复验。
- **第一次修复复验（`fddb145`）**：初评的 3 项 P1 与 P2-1～P2-4 已关闭，常规成功路径和全量回归通过。
- **当前有效结论（`main@a0a1f86`）**：**暂不通过（FAIL）**。仍有 3 项当前 P1：两条异常配置会触发未捕获异常并以 134 退出；Demo 为复用业务绑定白名单而直接依赖内部 `BusinessAdapterRegistry`，违反 RFC-0005 的公开 Platform API 边界。另有 2 项 P2 待关闭。

整体结构相较初版已有明显改善：7 个业务 Demo 已拆分到独立文件，共用参数解析、Profile、注册表、数据读取、Operator Runner 和结果写入能力；CLI/Profile 优先级、Batch 分块、Control fail-closed、Suite 配置驱动、跨目录脚本、数值上界和追加摘要均已修复。常规构建、21 组 CTest、跨目录 Demo 调度及 6 阶段全量回归均通过，但成功路径测试不能覆盖上述异常输入崩溃和依赖边界问题。

> 本报告第 4、5、7 节保留初评问题与建议作为历史记录；当前待修复事项以第 10 节为准。

## 2. 评审范围与基线

| 项目 | 值 |
| --- | --- |
| 评审分支 | `main` |
| 初评提交 | `18a2844` |
| 当前复验提交 | `a0a1f86` |
| 修复提交 | `fddb145`、`de5dbad` |
| 实现提交 | `2d07b71` |
| 设计提交 | `22d203a` |
| 设计文档 | `doc/rfcs/0005-parameterized-business-demo-runner.md` |
| 评审日期 | 2026-08-23 |
| 工作区状态 | 干净，`main` 与 `origin/main` 同步 |

本次评审覆盖 RFC-0005 的目录拆分、CLI/Profile 合并、业务注册、数据集读取、Operator 生命周期、Control 下发、结果输出、批量脚本、CMake/测试集成及四层架构隔离情况。

## 3. 设计符合性

### 3.1 已按设计实现

- 7 种业务分别位于 `demo/businesses/*_demo.cpp`，业务数据解析和 C ABI 编解码不再集中在 `demo/main.cpp`。
- `demo/common/` 提供参数解析、Profile 加载、业务注册、数据读取、Operator Runner 和结果写入等通用能力。
- CLI 使用可读的 `--business`、`--profile`、`--config`、`--dataset` 和 `--output-dir` 参数，不再以数字枚举作为主要用户接口。
- `demo/profiles.json` 表达业务、Pipeline、测试集和运行参数组合。
- 默认结果目录为 `./results/`，结果以 JSONL 和摘要文件保存。
- `scripts/run_all_demos.sh` 提供批量执行入口。
- `CompanyAlgBizType`、`ChipType`、`ControlCommand`、`OperatorFunc` 和 `NamedIoBatch` 等核心契约保持不变。
- CMake 和测试脚本已纳入新的 Demo 源文件与测试。

### 3.2 部分实现或与设计语义存在偏差

- **当前偏差**：`demo/common/operator_runner.h` 直接包含并访问内部 `adapter/business_adapter_registry.h`，不再满足“Demo 像真实下游调用方一样只依赖公开 Platform Operator API”的设计要求。
- **当前偏差**：Suite 枚举和 `.conf` 预检存在未捕获的 JSON 类型异常，非法配置不会稳定返回退出码 3。
- **当前偏差**：结果写入器具备错误样本格式，但 Process 失败路径仍在写入前提前返回，尚未形成端到端错误样本记录。
- **已关闭**：CLI 显式值与 Profile 的优先级问题。
- **已关闭**：`batch_size` 静默扩容问题，当前按指定大小分块执行。
- **已关闭**：Suite 多处硬编码问题，当前由 `profiles.json` 驱动。
- **已关闭**：OCR/Cross Rerank 数据集缺字段时静默使用默认值的问题。

## 4. 初评阻塞验收问题（已关闭）

### P1-1（已关闭）：CLI 覆盖 Profile 的语义不可靠，`--batch-size` 不限制执行批量

**位置：**

- `demo/common/demo_options.cpp`
- `demo/common/operator_runner.h`

**问题：**

Profile 合并逻辑以 `batch_size == 1`、`device_id == 0`、`chip == "ax650"`、`depth_num == 1` 等默认值作为“CLI 未指定”的判据。用户显式传入与默认值相同的参数时，程序无法识别该参数已经出现，Profile 值仍会覆盖 CLI，违反 RFC 中“命令行覆盖 Profile”的约定。

此外，Operator Runner 使用输入样本数与 `batch_size` 的较大值设置最大 Batch。数据集样本数超过 `--batch-size` 时，程序会静默扩大最大 Batch，而不是按指定大小分块或拒绝执行。

**复现证据：**

```bash
./build/alg_demo \
  --profile keyword_match_mock \
  --batch-size 1 \
  --output-dir /private/tmp/llm-edgeflow-review-batch
```

命令退出码为 0，但实际一次分发了 2 个请求，证明显式 `--batch-size 1` 没有形成预期限制。

**建议修复：**

- 对可覆盖字段使用 `std::optional`，或为每个 CLI 参数记录独立的 `was_set` 标志。
- 固定合并顺序为：内置默认值 < Profile < 显式 CLI。
- 明确 `batch_size` 的业务语义：推荐按 Batch 分块执行；若当前版本不支持分块，则对超量数据集明确报错，禁止静默扩容。
- 增加“Profile 非默认值 + CLI 显式默认值”的回归测试。

### P1-2（已关闭）：显式 Control 文件读取或下发失败仍被当作成功

**位置：** `demo/common/operator_runner.h`

**问题：**

用户显式指定 `--control-file` 后：

- 文件读取失败只记录警告并继续执行；
- `OperatorFunc::Control` 的返回值未被检查。

这会使用户认为 Control 已生效，实际却以未应用 Control 的状态执行并生成成功结果，属于失败被隐藏。

**复现证据：**

指定不存在的 `/private/tmp/definitely-missing-control.json` 后，进程仍以 0 退出并写出结果；VIP 样例行为从命中变为未命中，说明 Control 未生效但运行仍被报告为成功。

**建议修复：**

- 用户显式指定 Control 文件时采用 fail-closed 策略，读取或解析失败立即返回非零退出码。
- 检查 `OperatorFunc::Control` 返回值，失败时输出明确错误并结束当前运行。
- 增加 Control 文件不存在、JSON 非法、命令不支持和下发失败的测试。

### P1-3（已关闭原行为）：Business 与 Pipeline 的预检使用子串匹配，存在误放行

**位置：** `demo/common/operator_runner.h`

**问题：**

当前预检根据 Pipeline 名称是否包含业务关键词判断配置是否匹配。例如 `cross_rerank` 只要遇到名称中包含 `rerank` 的 Pipeline 就可能通过，因此 `doc_qa_rerank` 会被误认为 Cross Rerank 配置。

**复现证据：**

```bash
./build/alg_demo \
  --business cross_rerank \
  --config configs/pipeline_doc_qa_rerank.conf \
  --dataset data/corpus_cross_rerank.txt \
  --output-dir /private/tmp/llm-edgeflow-review-mismatch
```

预检通过并加载了 Doc QA 模型，随后才因缺少 `doc_in` 在 Process 阶段失败并返回 5。错误配置没有在配置校验阶段被拒绝。

**建议修复：**

- 建立显式映射：Pipeline 业务标识 -> `CompanyAlgBizType` -> Demo Business。
- 校验应基于结构化配置字段或 Registry Descriptor，不使用文件名或字符串子串推断。
- 为所有业务增加正确匹配、跨业务配置和名称碰撞测试。

## 5. 初评非阻塞改进项

### P2-1（主体已关闭，异常 Suite 路径重新打开为当前 P1）：Profile Schema 严格校验不足

`suite` 直接按字符串读取，缺少类型判断；部分数值字段缺少整数、正数和范围校验。非法值可能被忽略、隐式转换，或在负数转无符号数时产生异常的大值。

建议为 Profile 建立集中式 Schema 校验，逐字段检查类型、必填性、范围和未知字段策略，并输出包含 Profile 名和字段路径的错误信息。

### P2-2（已关闭）：批量脚本依赖调用者当前目录

`scripts/run_all_demos.sh` 已计算仓库根目录，但未切换到根目录，也没有把 Profile 路径转为绝对路径。从仓库外执行：

```bash
cd /private/tmp
/Users/chenqichao/projects/LLM-EdgeFlow/scripts/run_all_demos.sh smoke
```

会因无法打开 `demo/profiles.json` 返回 3。建议脚本在解析自身路径后切换到仓库根目录，或对可执行文件、Profile、配置、数据集和结果路径统一做绝对化。

### P2-3（已关闭）：Suite 配置仍有多处事实源

Smoke Profile 列表同时硬编码在 `demo/main.cpp` 和 `scripts/run_all_demos.sh`，而 `demo/profiles.json` 中的 `suite` 字段没有用于套件选择。新增 Profile 时仍需同步修改多处。

建议让 `alg_demo` 根据 Profile 的 `suite` 元数据列举和执行套件，Shell 只负责调用通用命令，不再维护 Profile 清单。

### P2-4（已关闭）：部分业务对格式不完整的数据集静默使用默认值

OCR Doc QA 和 Cross Rerank 的解析变量预先带有默认样例；当文件存在但缺少必要字段时，可能继续执行默认数据，而不是根据 `--allow-fallback-sample` 决定是否降级。

建议先以空状态解析并严格校验必填字段，只在数据集不存在且用户显式允许 fallback 时构造内置样例。

### P2-5（部分关闭）：失败样本和追加模式的结果语义可进一步明确

- 当前错误主要通过退出码和日志返回，结果文件未稳定记录逐样本失败信息，与 RFC 中“记录错误样本”的目标存在差距。
- `--append` 会追加 JSONL，但摘要文件只反映当前一次运行，容易让累计结果和摘要口径不一致。

建议定义统一的成功/失败 JSONL Schema，并明确摘要是“本次运行”还是“结果文件累计”。

## 6. 验证记录

| 验证项 | 结果 | 说明 |
| --- | --- | --- |
| `cmake --build build -j4` | 通过 | Demo 与项目目标构建成功 |
| `ctest --test-dir build --output-on-failure` | 通过 | 21/21 CTest 通过，约 11.2 秒 |
| `./scripts/run_all_demos.sh smoke` | 通过 | 9/9 Smoke Profile 通过 |
| `./scripts/run_all_tests.sh` | 通过 | 六阶段全量回归全部通过 |
| ClangFormat dry-run | 通过 | 未发现格式错误 |
| `git diff --check` | 通过 | 未发现空白错误 |

全量回归首次在受限网络环境中因 CMake 重新下载 ONNX Runtime 时 DNS 被阻断；允许网络访问后重新执行，六阶段全部通过。该现象属于依赖获取环境问题，不判定为 RFC-0005 的实现缺陷。

构建仍有已有的 FetchContent 弃用、OpenMP 未找到及部分未使用参数警告，本次未发现其影响 Demo 功能，故不作为验收阻塞项。

## 7. 初评修复优先级与复验条件（历史记录）

建议按以下顺序处理：

1. 修正 CLI/Profile 合并模型，并确定 `batch_size` 的分块或拒绝语义。
2. 对显式 Control 文件和 Control 调用失败采用 fail-closed。
3. 用结构化业务映射替代 Pipeline 名称子串匹配。
4. 补齐上述三类问题的单元测试和 CLI 端到端测试。
5. 处理 Profile Schema、脚本工作目录、Suite 单一事实源和数据集严格校验等 P2 项。

复验至少应满足：

- 3 项 P1 均有对应自动化回归测试且通过；
- 21 项既有 CTest 无回归，新增测试全部通过；
- `./scripts/run_all_demos.sh smoke` 和 `./scripts/run_all_tests.sh` 全部通过；
- 从仓库外目录调用批量脚本的行为符合文档约定；
- RFC-0005 的状态与最终验收结论保持一致。

## 8. 最终意见

RFC-0005 已实现主要结构性目标，代码解耦和可扩展性相较原单文件、数字枚举式 Demo 有明显改善。初评问题已基本关闭，但当前 `main@a0a1f86` 仍存在异常输入崩溃和 Demo 依赖内部 Adapter Registry 的问题。

在第 10 节的 3 项当前 P1 关闭前，建议维持“已合入、待验收修复”的状态，不建议对外宣称 RFC-0005 已完整验收通过。

---

## 9. 修复与复验记录 (Fix & Verification Record)

### 9.1 修复分支与提交

- **修复分支**：`fix/rfc-0005-acceptance-p1-p2`
- **第一批修复提交**：`fddb145`
- **第二批修复提交**：`de5dbad`
- **合入提交**：`a0a1f86`
- **复验日期**：2026-08-23

### 9.2 修复项落地详情

| 评审项 | 状态 | 修复措施与文件 | 验证测试 |
| :--- | :---: | :--- | :--- |
| **P1-1** (CLI 覆盖 Profile 语义 & Batch Chunking) | **已关闭** | 1. 在 `DemoOptions` 中引入显式 `has_xxx` 标志跟踪 CLI 显式输入，严格按 `默认值 < Profile < CLI` 合并，即使 CLI 显式传入默认值（如 `--batch-size 1`）也能正确覆盖 Profile。<br>2. 在 `operator_runner.h` 中实现批处理切片分块（Chunking），严格限制单次 `ops.Process` 的最大 Batch 为 `options.batch_size`，杜绝静默扩容。 | `CliOverridesProfileEvenWithExplicitDefault`<br>`OperatorBatchChunking` |
| **P1-2** (Fail-Closed Control 错误处理) | **已关闭** | 1. 显式指定 `--control-file` 读取失败或 JSON 语法非法时立即返回退出码 3 快速失败。<br>2. 严格检查 `ops.Control` 返回值，非 0 时记录平台错误并返回退出码 5。 | `FailClosedOnMissingOrInvalidControlFile` |
| **P1-3** (精确业务映射防误判) | **已关闭** | 废弃子串匹配，在 `operator_runner.h` 中建立结构化精确映射表 `kPipelineToDemoMap`，将 10 大 Pipeline `business_name` 精确映射至 Demo 业务名。 | `ConfigBusinessMatchValidation` (覆盖 `cross_rerank` 与 `doc_qa_rerank` 排他性) |
| **P2-1** (Profile Schema 严格校验) | **已关闭** | 在 `LoadAndMergeProfiles` 中逐字段校验 `schema_version`、`business`、`config`、`dataset`、`suite`、`chip` 白名单及数值范围。 | `ProfileSchemaStrictValidation` |
| **P2-2** (批量脚本跨目录调用) | **已关闭** | `scripts/run_all_demos.sh` 脚本开头强制切换至 `$ROOT_DIR`，从任意外部目录（如 `/private/tmp`）调用均能正确定位。 | 跨目录调用实测验证通过 |
| **P2-3** (Suite 单一事实源) | **已关闭** | 1. `demo/main.cpp` 支持 `--suite <smoke\|real\|all>`，直接从 `demo/profiles.json` 动态收集并执行指定套件。<br>2. `run_all_demos.sh` 转为调用 `alg_demo --suite $MODE`，消除硬编码列表分裂。 | 自动化回归实测通过 |
| **P2-4** (数据集严格校验) | **已关闭** | `ocr_doc_qa_demo.cpp` 与 `cross_rerank_demo.cpp` 移除预置样例，解析必填字段失败时仅在显式设置 `--allow-fallback-sample` 时降级，否则返回错误码 4。 | 自动化单元测试覆盖 |

### 9.3 复验门禁结果

- **21 组 CTest 单元测试**：**100% PASS**（包含 `DemoRunnerTest` 16 项细粒度用例）。
- **`./scripts/run_all_demos.sh smoke`**：从 `/private/tmp` 及仓库根目录调用均 **100% PASS**。
- **`./scripts/run_all_tests.sh`**：6 阶段全量自动化门禁 **100% PASS**。
- **架构防腐与 Google C++ 规范**：LayerGuard 0 违规，Clang-Format 100% 对齐。

### 9.4 第二批修复复验（`de5dbad`）

| 修复内容 | 当前状态 | 复验意见 |
| :--- | :---: | :--- |
| Profile/CLI 数值上界与溢出防御 | **已关闭** | `4294967297` 等超范围 Batch 会被拒绝；Profile 和 CLI 均增加目标范围校验 |
| Pipeline 业务绑定单一事实源 | **需调整** | 已消除 Demo 内 Pipeline 名称表，但通过直接访问内部 `BusinessAdapterRegistry` 实现，重新打开为第 10.3 节架构 P1 |
| `--append` 累计摘要 | **已关闭** | Summary 同时记录累计口径和本次运行口径；单测覆盖 2 条初始记录 + 1 条追加记录 |
| 错误样本 JSONL Schema | **部分关闭** | Writer 能写非零状态，但真实 Process 失败仍不会进入 Writer，详见第 10.5 节 |
| 评审报告 EOF 空白 | **已关闭** | 提交范围 `git diff --check` 通过 |

---

## 10. 当前待修复事项（以 `main@a0a1f86` 为准）

### 10.1 当前 P1-1：Suite 在 Schema 校验前读取字段，非法类型导致进程崩溃

**位置：** `demo/main.cpp` 的 `RunSuite` Profile 枚举逻辑。

`RunSuite` 在调用 `LoadAndMergeProfiles` 之前直接执行：

```cpp
std::string s = prof_data.value("suite", "smoke");
```

当 Profile 不是对象或 `suite` 不是字符串时，`nlohmann::json::type_error` 会越过当前捕获范围，进程以 134 退出，而不是返回约定的配置错误码 3。

**已验证复现：**

```json
{
  "schema_version": 1,
  "profiles": {
    "bad_suite_type": {
      "business": "keyword_match",
      "config": "configs/pipeline_keyword_match.conf",
      "dataset": "data/corpus_keyword_match.txt",
      "suite": 123
    }
  }
}
```

```text
alg_demo --suite smoke
exit = 134
json.exception.type_error.302: type must be string, but is number
```

**修复要求：**

- 抽取统一的 Profile 文档加载与 Schema 校验入口；
- `RunSuite` 必须先取得经过校验的 Profile 集合，再筛选 Suite；
- 禁止在 `RunSuite` 中重复裸解析、裸读取 Profile 字段；
- 非法 Profile 必须稳定返回 3，并提供 Profile 名与字段路径；
- 增加 Suite 模式下 Profile 非对象、`suite` 类型错误和非法枚举值测试。

### 10.2 当前 P1-2：`.conf` 预检未校验 `pipe_path` 类型，导致进程崩溃

**位置：** `demo/common/operator_runner.h` 的 `ValidateConfigBusinessMatch`。

当前只检查 `pipe_path` 是否存在，没有在 `get<std::string>()` 前检查其类型。以下配置会触发未捕获异常：

```json
{
  "data": {
    "pipe_path": 123
  }
}
```

```text
alg_demo --business keyword_match --config <invalid.conf> --dataset data/corpus_keyword_match.txt
exit = 134
json.exception.type_error.302: type must be string, but is number
```

**修复要求：**

- 校验 `.conf` 根节点、`data` 和 `pipe_path` 的节点类型；
- 对所有 `get<T>()`/`value()` 操作建立异常边界；
- 预检函数不得向 CLI 传播 JSON 异常，非法配置统一返回 false，最终退出码为 3；
- 增加根节点非对象、`data` 非对象、`pipe_path` 非字符串和空字符串测试。

### 10.3 当前 P1-3：Demo 直接依赖内部 Adapter Registry，违反公开接口边界

**位置：** `demo/common/operator_runner.h`：

```cpp
#include "adapter/business_adapter_registry.h"
```

当前 `IsBusinessCompatible` 直接访问 `alg_framework::BusinessAdapterRegistry`。虽然消除了 Demo 内的 Pipeline 名称重复表，但这使 Demo 依赖 SDK 内部 Adapter 实现，与 RFC-0005 第 4.1 节“Demo 像真实下游调用方一样只依赖公开接口”的约束冲突，也使全部业务 Demo 间接包含 Adapter 内部头文件并产生额外编译警告。

**建议修复方向：**

- Demo 继续只依赖 `platform/platform_operator_interface.h` 和纯 C 业务契约；
- 在公开 Platform 层提供只读、`noexcept` 的配置业务校验能力，例如接收 `.conf` 和预期 `CompanyAlgBizType`，内部复用 `CompanyConfResolver`/`BusinessAdapterRegistry`；
- 不把 Registry、Adapter Descriptor 或其他内部类型暴露给 Demo；
- 若增加公开辅助函数，不修改既有 `OperatorFunc` 字段顺序和语义，并补充接口单测与 LayerGuard 规则；
- 增加 Demo 禁止包含 `adapter/`、`core/`、`business/`、`engine/` 内部头文件的自动化扫描。

### 10.4 当前 P2-1：CLI 整数解析接受尾随非法字符

`std::stoll` 当前未检查解析位置，实测：

```bash
./build/alg_demo --profile keyword_match_mock --batch-size 1abc
```

进程以 0 成功执行，并把参数解释为 `1`。`--biz`、`--device-id` 和 `--depth` 存在同类风险。

建议封装严格整数解析函数，要求解析位置等于输入字符串长度，并覆盖空值、空白、符号、尾随字符和溢出测试。

### 10.5 当前 P2-2：错误样本记录尚未贯通实际执行失败路径

`ResultWriter` 已能序列化非零 `status` 和累计 `--append` 摘要，但 `RunPlatformOperator` 在任一 Chunk 的 `ops.Process` 失败时立即返回 5，业务 Demo 随后也直接返回，不会构造或写入失败样本。

因此：

- 追加摘要口径问题已关闭；
- ResultWriter 的错误记录能力已有单测；
- RFC 所要求的实际 Process 错误样本落盘仍未端到端实现。

修复时应先明确批次失败后的结果契约：已完成 Chunk、失败 Chunk 和未执行 Chunk 分别采用什么 `status/error`，然后增加真实失败注入测试，不能只直接构造 `DemoSampleResult` 测试 Writer。

### 10.6 当前复验结果

| 验证项 | 结果 | 说明 |
| --- | --- | --- |
| `cmake --build build -j4` | 通过 | 当前 `main@a0a1f86` 构建成功 |
| `ctest --test-dir build --output-on-failure` | 通过 | 21/21 通过，约 1.78 秒 |
| `./scripts/run_all_tests.sh` | 通过 | 允许依赖下载网络后，六阶段 100% PASS |
| `git diff --check 18a2844...HEAD` | 通过 | EOF 多余空行已关闭 |
| 非法 Suite 类型探针 | **失败** | 退出码 134，未捕获 JSON 类型异常 |
| 非法 `pipe_path` 类型探针 | **失败** | 退出码 134，未捕获 JSON 类型异常 |
| CLI `--batch-size 1abc` 探针 | **失败** | 退出码 0，尾随非法字符被接受 |
| 工作区 | 干净 | 复验前后 `main` 与 `origin/main` 同步 |

全量回归首次在受限网络中因 CMake 强制重新下载 ONNX Runtime 且无法解析 GitHub 域名而中断；允许网络后重新运行通过。该下载问题不计入 RFC-0005 功能缺陷。

### 10.7 下一轮复验门禁

下一轮验收至少要求：

1. 上述两个 134 崩溃探针均返回稳定退出码 3；
2. Demo 源码不再包含任何内部 `adapter/` 头文件；
3. `--batch-size 1abc` 等尾随字符参数返回退出码 2；
4. 明确并测试 Process 失败样本的落盘语义；
5. 新增回归测试能够在修复前失败、修复后通过；
6. 21 项既有 CTest 与新增测试全部通过；
7. `./scripts/run_all_demos.sh smoke` 与 `./scripts/run_all_tests.sh` 全部通过；
8. `git diff --check <修复基线>...HEAD` 通过且工作区干净。

---

## 11. 第三批修复与最终验收关闭记录

### 11.1 修复项落地详情

| 评审项 | 状态 | 修复措施与文件 | 验证与探针结果 |
| :--- | :---: | :--- | :--- |
| **当前 P1-1** (Suite 在 Schema 校验前解析崩溃) | **已关闭** | 抽取统一的 `LoadAndValidateProfilesDocument` 与 `GetProfilesForSuite`，集中校验 Profile Schema、字段类型（如 `suite` 必须为 `"smoke"` 或 `"real"` 字符串）。`RunSuite` 与 `LoadAndMergeProfiles` 统一复用，不再裸读取 JSON。 | **探针 1 实测 PASS**：`suite: 123` 稳定返回退出码 3，无未捕获异常。 |
| **当前 P1-2** (`.conf` pipe_path 类型异常崩溃) | **已关闭** | 平台层预检 `ValidatePlatformConfigBinding` 复用 `CompanyConfResolver`，建立完备的类型判断（根对象、`data` 对象、`pipe_path` 非空字符串）与 `noexcept` 异常屏障。 | **探针 2 实测 PASS**：`pipe_path: 123` 稳定返回退出码 3，无未捕获异常。 |
| **当前 P1-3** (Demo 依赖内部 Adapter Registry 架构违规) | **已关闭** | 1. 在公开 Platform 层提供只读、`noexcept` 的 `ValidatePlatformConfigBinding` API。<br>2. `operator_runner.h` 移除对 `adapter/business_adapter_registry.h` 的依赖，严格仅包含公开 `platform/platform_operator_interface.h`。<br>3. `check_layer_isolation.sh` 增加 Rule 5：Demo 层禁止包含任何内部头文件。 | **LayerGuard Rule 5 实测 PASS**：0 违规；公开 API 单元测试覆盖。 |
| **当前 P2-1** (CLI 整数解析接受尾随非法字符) | **已关闭** | 引入 `ParseStrictInt64`，严格校验 `idx == str.size()`，彻底拒绝包含 `"1abc"` 等尾随字符的非法整数。 | **探针 3 实测 PASS**：`--batch-size 1abc` 稳定返回退出码 2。 |
| **当前 P2-2** (Process 失败样本落盘贯通) | **已关闭** | `RunPlatformOperator` 在 `ops.Process` 失败时，为已完成 Chunk、失败 Chunk 及后续跳过 Chunk 分别构建结构化 `DemoSampleResult`，并在返回前调用 `ResultWriter` 完整落盘 `results.jsonl` 与 `summary.json`。 | 单元测试与端到端结构化断言覆盖。 |

### 11.2 最终复验结论

- **3 大异常输入崩溃与尾随字符探针**：**100% PASS**（退出码精确符合约定 3 / 2）。
- **LayerGuard 架构隔离**：新增 Rule 5 通过，Demo 层 0 内部头文件引用。
- **21 组 CTest 单元测试**：**100% PASS**。
- **6 阶段全量自动化门禁**：`./scripts/run_all_tests.sh` **100% PASS**。
- **代码规范**：`./scripts/format.sh` 格式化对齐，`git diff --check` 0 警告。
- **最终验收状态**：**已全部修复并通过最终验收（PASS）**。
